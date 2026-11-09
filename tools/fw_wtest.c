/*
 * fw_wtest.c - compare the two documented FireWire call styles on one address.
 *
 * Why this exists: on Monterey every plain WriteQuadlet into the desk's private
 * space returns 0xe0008017 (kIOFireWireAddressError), while the same registers
 * read fine - and on Mojave the manager's own writes were ACCEPTED (see
 * docs/HOW-IT-WORKS.md). So the question is whether the difference is in the *call*.
 *
 * IOFireWireLib offers two addressing modes (IOFireWireLib.h, WriteQuadlet):
 *
 *   48-bit, device relative : pass device = GetDevice()  (what fw_read does)
 *   64-bit, absolute        : pass device = 0, put the node id in the address's
 *                             nodeID field, and check the generation
 *
 * The manager's capture shows it using nodeID 0x0000 for private-space calls and
 * 0xffc1 for the desk's IRM registers, so both are reproduced here.
 *
 * For every mode this tool does: read a CSR address (the desk's IRM block, which
 * the manager read successfully on Mojave), read the ISP register, then write the
 * ISP register back to itself. Nothing changes state when it works: the value
 * written is the value already there.
 *
 * Build:
 *     clang -Wall -O2 -o fw_wtest fw_wtest.c \
 *           -framework IOKit -framework CoreFoundation
 *
 * Run:
 *     ./fw_wtest
 *     ./fw_wtest 0xE00000E8 0xbb020000     # other address / raw wire quadlet
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>

#define N12_GUID 0x00a0de0000011013ULL

/* kIOMainPortDefault only exists from macOS 12; Mojave (10.14) still spells it
 * kIOMasterPortDefault. Same guard as fw_read.c so this builds on both disks. */
#if defined(MAC_OS_VERSION_12_0) && MAC_OS_VERSION_12_0
#define YAM_IO_PORT kIOMainPortDefault
#else
#define YAM_IO_PORT kIOMasterPortDefault
#endif

/* The address the manager used to read the desk's IRM registers. */
#define IRM_BW_AVAIL 0xF0000220u

static IOFireWireLibDeviceRef open_lib(io_service_t svc,
                                       IOCFPlugInInterface ***plugin_out)
{
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOReturn r = IOCreatePlugInInterfaceForService(
        svc, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    if (r != kIOReturnSuccess || plugin == NULL) {
        printf("IOCreatePlugInInterfaceForService -> 0x%08x\n", r);
        return NULL;
    }
    IOFireWireLibDeviceRef fw = NULL;
    HRESULT hr = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID), (LPVOID *)&fw);
    if (hr != S_OK || fw == NULL) {
        printf("QueryInterface -> 0x%08x\n", (unsigned)hr);
        (*plugin)->Release(plugin);
        return NULL;
    }
    *plugin_out = plugin;
    return fw;
}

static io_service_t find_n12(void)
{
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(YAM_IO_PORT,
                                     IOServiceMatching("IOFireWireDevice"), &it)
        != KERN_SUCCESS) {
        return IO_OBJECT_NULL;
    }
    io_service_t found = IO_OBJECT_NULL, svc;
    while ((svc = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        CFTypeRef ref = IORegistryEntryCreateCFProperty(
            svc, CFSTR("GUID"), kCFAllocatorDefault, 0);
        uint64_t guid = 0;
        if (ref != NULL) {
            if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
                CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt64Type, &guid);
            }
            CFRelease(ref);
        }
        if (guid == N12_GUID) {
            found = svc;
        } else {
            IOObjectRelease(svc);
        }
    }
    IOObjectRelease(it);
    return found;
}

/* One addressing mode. `node` goes into FWAddress.nodeID, `dev` into the call's
 * device argument. `fail_on_reset` says whether the generation is checked. */
struct mode {
    const char *name;
    io_object_t device;
    uint16_t    node;
    Boolean     fail_on_reset;
};

/* Read one quadlet with a given mode; returns the IOReturn and the raw value. */
static IOReturn rd(IOFireWireLibDeviceRef fw, const struct mode *m,
                   uint32_t lo, UInt32 gen, uint32_t *wire)
{
    FWAddress a;
    memset(&a, 0, sizeof(a));
    a.nodeID    = m->node;
    a.addressHi = 0xFFFF;
    a.addressLo = lo;
    UInt32 v = 0;
    IOReturn r = (*fw)->ReadQuadlet(fw, m->device, &a, &v,
                                    m->fail_on_reset, gen);
    *wire = __builtin_bswap32(v);   // ReadQuadlet hands back the value swapped
    return r;
}

int main(int argc, char **argv)
{
    /* Default: ISP 0's control quadlet, which the manager writes as 0x000002bb. */
    uint32_t lo    = 0xE00000E8;
    uint32_t value = 0xbb020000;    // the raw wire quadlet for 0x000002bb
    if (argc > 2) {
        lo    = (uint32_t)strtoul(argv[1], NULL, 16);
        value = (uint32_t)strtoul(argv[2], NULL, 16);
    }

    io_service_t svc = find_n12();
    if (svc == IO_OBJECT_NULL) {
        printf("no IOFireWireDevice with GUID 0x%016llx\n",
               (unsigned long long)N12_GUID);
        return 1;
    }
    IOCFPlugInInterface **plugin = NULL;
    IOFireWireLibDeviceRef fw = open_lib(svc, &plugin);
    if (fw == NULL) {
        return 1;
    }
    printf("Open() -> 0x%08x\n", (*fw)->Open(fw));

    UInt32 gen = 0;
    (*fw)->GetBusGeneration(fw, &gen);
    io_object_t dev = (*fw)->GetDevice(fw);
    printf("bus generation %u, device handle 0x%x\n\n", gen, (unsigned)dev);

    struct mode modes[] = {
        { "relative (device handle), nodeID 0x0000", dev, 0x0000, false },
        { "absolute, nodeID 0xffc1 (the desk)",      0,   0xffc1, true  },
        { "absolute, nodeID 0x0001 (node 1)",        0,   0x0001, true  },
    };

    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        uint32_t v = 0;
        IOReturn r;
        printf("%s\n", modes[i].name);

        r = rd(fw, &modes[i], IRM_BW_AVAIL, gen, &v);
        printf("  read  CSR  0x%08x -> 0x%08x (r=0x%08x)\n", IRM_BW_AVAIL, v, r);

        r = rd(fw, &modes[i], lo, gen, &v);
        printf("  read  ISP  0x%08x -> 0x%08x (r=0x%08x)\n", lo, v, r);

        FWAddress a;
        memset(&a, 0, sizeof(a));
        a.nodeID    = modes[i].node;
        a.addressHi = 0xFFFF;
        a.addressLo = lo;
        IOReturn w = (*fw)->WriteQuadlet(fw, modes[i].device, &a, value,
                                         modes[i].fail_on_reset, gen);
        printf("  write ISP  0x%08x <- 0x%08x (r=0x%08x)\n\n", lo, value, w);
    }

    /* Control group: the standard CSR registers the bus manager is entitled to
     * write on the isochronous resource manager (the desk). If even these are
     * refused, the refusal is about the device as a whole, not about the
     * Yamaha private space. Values are written back unchanged. */
    printf("control: CSR registers the bus manager may write\n");
    struct mode rel = { "relative", dev, 0x0000, false };
    uint32_t csr[] = { 0xF0000220u, 0xF0000224u, 0xF0000228u, 0xF0000240u };
    const char *csr_name[] = { "BUS_MANAGER_ID ", "BANDWIDTH_AVAIL",
                               "CHANNELS_AVAIL ", "BROADCAST_CHAN " };
    for (unsigned i = 0; i < 4; i++) {
        uint32_t v = 0;
        IOReturn r = rd(fw, &rel, csr[i], gen, &v);
        if (r != kIOReturnSuccess) {
            printf("  %s 0x%08x  read -> 0x%08x\n", csr_name[i], csr[i], r);
            continue;
        }
        FWAddress a;
        memset(&a, 0, sizeof(a));
        a.addressHi = 0xFFFF;
        a.addressLo = csr[i];
        /* write the value back in wire order (rd() already converted) */
        IOReturn w = (*fw)->WriteQuadlet(fw, dev, &a, __builtin_bswap32(v),
                                         false, 0);
        printf("  %s 0x%08x  = 0x%08x  write-back -> 0x%08x\n",
               csr_name[i], csr[i], v, w);
    }
    printf("\n");

    printf("0x00000000 = accepted   0xe0008017 = address error from the node\n"
           "0xe00002d6 = timeout (nothing answered)   0xe00002cc = lock refused\n");

    (*fw)->Close(fw);
    (*fw)->Release(fw);
    (*plugin)->Release(plugin);
    IOObjectRelease(svc);
    return 0;
}
