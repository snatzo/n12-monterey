/*
 * fw_read.c - read quadlets out of the n12's own address space.
 *
 * Why this exists: everything so far has been inferred from the driver side.
 * The device has its own state - the sample rate it is running at, and the
 * isochronous channels it is configured to transmit and receive on - and that
 * state is readable over FireWire. If it is there, we can set the driver to
 * match instead of guessing, which is exactly what the "1394 wiring" exchange
 * would have told us.
 *
 * The n12 is a TCAT DICE II device (see docs/HOW-IT-WORKS.md), so the
 * vendor register block lives at 0xFFFFE0000000 and the standard CSR/config-ROM
 * area at 0xFFFFF0000000. This reads both and prints the quadlets, so a
 * channel number (0-63) or a sample rate (44100 / 48000 / 96000) can be spotted
 * by eye.
 *
 * Reads are always safe and are the default. Writing is the last piece of the
 * "1394 wiring" (CISP::SetIsochChannel writes one quadlet to `base + 4`; the
 * init-entry creators write small blocks), so a write mode exists behind
 * explicit flags - `--write`, `--writeblock`, `--rmw` - and is never reached
 * unless asked for. Nothing here writes by default.
 *
 * Build:
 *     clang -Wall -O2 -o fw_read fw_read.c \
 *           -framework IOKit -framework CoreFoundation
 *
 * Run (read-only):
 *     ./fw_read                       # the DICE block and the CSR block
 *     ./fw_read 0xFFFFE0000000 128    # base and quadlet count
 *     ./fw_read --block 0xFFFFE0400000 0xC4
 *     ./fw_read --findchan 0xFFFFE0000890 0x20
 *     ./fw_read --plug                 # decode the core-space / plug tree
 *
 * Run (writes - use with care):
 *     ./fw_read --write   0xFFFFE0000894 0x00010004     # one quadlet
 *     ./fw_read --rmw     0xFFFFE0000894 0xFF81FFFF 0x00010004
 *     ./fw_read --writeblock 0xFFFFE0300000 87654321000000000000010000001000
 *
 * Run (Lock / compare-and-swap - the transaction the vendor enabler uses):
 *     ./fw_read --lockprobe 0xFFFFE0000000 512    # SAFE: same-value lock test
 *     ./fw_read --lock      0xFFFFE0000000 CMP NEW   # deliberate swap
 *
 * Run (bus control):
 *     ./fw_read --busreset            # restart the 1394 bus, then probe again
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>

#define N12_GUID 0x00A0DE0000011013ULL

#if defined(MAC_OS_VERSION_12_0) && MAC_OS_VERSION_12_0
#define YAM_IO_PORT kIOMainPortDefault
#else
#define YAM_IO_PORT kIOMasterPortDefault
#endif

/*
 * Which IOKit service to open.
 *
 * The default is Apple's IOFireWireDevice for the n12. But the Yamaha
 * connection manager does not only use that node: a capture of its own calls
 * (2026-11-06) shows it also creates a plug-in interface on a node published by
 * the Yamaha driver, `YamahaFWStreamDevice`, which sits under the FireWire
 * local node. If that node answers the standard FireWire device interface, then
 * our writes can go through the Yamaha kext - the same path the manager uses.
 *
 *   N12_FW_CLASS=YamahaFWStreamDevice   select the class to match
 *   N12_FW_ANY=1                        take the first match, skip the GUID test
 *                                       (the Yamaha node has no "GUID" property)
 */
static const char *service_class(void)
{
    const char *c = getenv("N12_FW_CLASS");
    return (c != NULL && c[0] != '\0') ? c : "IOFireWireDevice";
}

/* True when N12_FW_ANY=1: accept the first service of the class. */
static int any_service(void)
{
    const char *a = getenv("N12_FW_ANY");
    return a != NULL && a[0] == '1';
}

/* Find the service to open: the n12's IOFireWireDevice, or whatever class
 * N12_FW_CLASS names. Returns IO_OBJECT_NULL if nothing matches. */
static io_service_t find_n12_device(void)
{
    CFMutableDictionaryRef matching = IOServiceMatching(service_class());
    if (matching == NULL) {
        return IO_OBJECT_NULL;
    }
    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(YAM_IO_PORT, matching, &iter)
            != KERN_SUCCESS) {
        return IO_OBJECT_NULL;
    }

    io_service_t found = IO_OBJECT_NULL;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        // N12_FW_ANY=1: the class name alone is the identity test. This is the
        // way to reach a node that has no "GUID" property, such as the Yamaha
        // driver's node.
        if (any_service()) {
            found = svc;
            break;
        }
        CFTypeRef guidRef = IORegistryEntryCreateCFProperty(
            svc, CFSTR("GUID"), kCFAllocatorDefault, 0);
        if (guidRef != NULL) {
            uint64_t guid = 0;
            if (CFGetTypeID(guidRef) == CFNumberGetTypeID()) {
                CFNumberGetValue((CFNumberRef)guidRef, kCFNumberSInt64Type,
                                 &guid);
            }
            CFRelease(guidRef);
            if (guid == N12_GUID) {
                found = svc;
                break;
            }
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(iter);
    return found;
}

/*
 * Open IOFireWireLib on a device service. Returns the device interface, or NULL
 * with a reason printed.
 */
static IOFireWireLibDeviceRef open_firewire_lib(io_service_t service,
                                                IOCFPlugInInterface ***outPlugin)
{
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
        &plugin, &score);
    if (kr != KERN_SUCCESS || plugin == NULL) {
        printf("could not load IOFireWireLib: 0x%08x\n", kr);
        printf("  (the FireWire kext must be loaded, which it is, since the\n"
               "   n12 is on the bus)\n");
        return NULL;
    }

    IOFireWireLibDeviceRef fw = NULL;
    HRESULT hr = (*plugin)->QueryInterface(
        plugin,
        CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
        (LPVOID *)&fw);
    if (hr != S_OK || fw == NULL) {
        printf("QueryInterface for the device interface failed: 0x%08x\n",
               (unsigned)hr);
        (*plugin)->Release(plugin);
        return NULL;
    }
    *outPlugin = plugin;
    return fw;
}

/*
 * Dump `count` quadlets starting at `base`, as 48-bit device-relative reads.
 * device-relative addressing means the device argument is the service we built
 * the interface on, and no node id is needed.
 */
static void dump_region(IOFireWireLibDeviceRef fw, io_object_t device,
                        uint32_t base_hi, uint32_t base_lo, int count)
{
    printf("\n=== 0x%04x%08x, %d quadlets ===\n", base_hi, base_lo, count);
    int reported = 0;
    for (int i = 0; i < count; i++) {
        FWAddress addr;
        memset(&addr, 0, sizeof(addr));
        addr.addressHi = base_hi;
        addr.addressLo = base_lo + (uint32_t)i * 4;

        UInt32 val = 0;
        IOReturn r = (*fw)->ReadQuadlet(fw, device, &addr, &val,
                                        false /* don't fail on reset */, 0);
        if (i % 4 == 0) {
            printf("  +0x%03x:", i * 4);
        }
        if (r == kIOReturnSuccess) {
            // The device presents its CSR space byte-swapped relative to how
            // ReadQuadlet hands it back: the config ROM's signature reads
            // 0x34393331 rather than 0x31333934 ("1394"), and the GUID appears
            // as 00dea000 13100100 rather than 00a0de00 00011013. Swapping
            // makes the values readable, so that is what is printed.
            val = __builtin_bswap32(val);
            printf(" %08x", val);
        } else {
            // Show the first failure's code: a permission problem and an
            // unimplemented address are very different answers.
            if (!reported) {
                printf("  [first read failed: 0x%08x]", r);
                reported = 1;
            }
            printf(" --------");
        }
        if (i % 4 == 3) {
            printf("\n");
        }
    }
    if (count % 4 != 0) {
        printf("\n");
    }
}

/*
 * Scan a region for the mLAN feature record signature: a quadlet 0x0000A0DE
 * (Yamaha's OUI) immediately followed by 0x00000200. CmLANAsync::GetFeatureInfo
 * looks for exactly that to find the entry that points at the port block, so
 * finding it here locates the same table without the bundle.
 */
static void find_signature(IOFireWireLibDeviceRef fw, io_object_t device,
                           uint32_t base_hi, uint32_t base_lo, int count)
{
    printf("\n=== scanning 0x%04x%08x for A0DE/0200 (%d quadlets) ===\n",
           base_hi, base_lo, count);
    int hits = 0;
    for (int i = 0; i + 1 < count; i++) {
        FWAddress addr;
        memset(&addr, 0, sizeof(addr));
        addr.addressHi = base_hi;
        addr.addressLo = base_lo + (uint32_t)i * 4;

        UInt32 a = 0;
        if ((*fw)->ReadQuadlet(fw, device, &addr, &a, false, 0) != kIOReturnSuccess) {
            continue;
        }
        a = __builtin_bswap32(a);
        if (a != 0x0000A0DE) {
            continue;
        }
        addr.addressLo += 4;
        UInt32 b = 0;
        if ((*fw)->ReadQuadlet(fw, device, &addr, &b, false, 0) != kIOReturnSuccess) {
            continue;
        }
        b = __builtin_bswap32(b);
        printf("  found 0xA0DE at 0x%04x%08x, next = 0x%08x%s\n",
               base_hi, base_lo + (uint32_t)i * 4, b,
               b == 0x200 ? "   <- feature record" : "");
        if (b == 0x200) {
            hits++;
        }
    }
    printf("  %d feature record(s) found\n", hits);
}

/*
 * Block-read `size` bytes at an address. The port block at 0xFFFFE0400000
 * answers a quadlet read with response code 6 (type_error) but is what
 * CmLANAsyncDev::Initialize reads with ReadBlockWithRetry, so it wants a block
 * read. Values are printed big-endian, matching what the bundle's parse sees
 * before it byteswaps.
 */
static void block_read(IOFireWireLibDeviceRef fw, io_object_t device,
                       uint32_t base_hi, uint32_t base_lo, uint32_t size)
{
    uint8_t *buf = calloc(1, size);
    if (buf == NULL) {
        return;
    }
    FWAddress addr;
    memset(&addr, 0, sizeof(addr));
    addr.addressHi = base_hi;
    addr.addressLo = base_lo;

    UInt32 got = size;
    IOReturn r = (*fw)->Read(fw, device, &addr, buf, &got, false, 0);
    printf("\n=== block read 0x%04x%08x, %u bytes -> 0x%08x (%u bytes) ===\n",
           base_hi, base_lo, size, r, got);
    if (r != kIOReturnSuccess) {
        free(buf);
        return;
    }
    for (UInt32 i = 0; i + 4 <= got; i += 4) {
        // The device is big-endian; print the quadlet in that order.
        uint32_t v = ((uint32_t)buf[i] << 24) | ((uint32_t)buf[i + 1] << 16)
                   | ((uint32_t)buf[i + 2] << 8) | (uint32_t)buf[i + 3];
        if (i % 16 == 0) {
            printf("  +0x%03x:", i);
        }
        printf(" %08x", v);
        if (i % 16 == 12) {
            printf("\n");
        }
    }
    printf("\n");
    free(buf);
}

/*
 * Scan for registers shaped like an isoch-channel register.
 *
 * CISP::SetIsochChannel builds a value with bit 16 set as a "valid" flag and the
 * 6-bit channel at bits 17-22, so a register in that shape looks like
 * 0x00010000 + (channel << 17) plus whatever other bits the register carries.
 * This prints every address in the range whose value has bit 16 set, with the
 * channel field decoded, so the wiring registers can be found by inspection.
 */
static void find_chan_regs(IOFireWireLibDeviceRef fw, io_object_t device,
                           uint32_t base_hi, uint32_t base_lo, int count)
{
    printf("\n=== scanning 0x%04x%08x for channel-shaped registers (%d) ===\n",
           base_hi, base_lo, count);
    int hits = 0;
    for (int i = 0; i < count; i++) {
        FWAddress addr;
        memset(&addr, 0, sizeof(addr));
        addr.addressHi = base_hi;
        addr.addressLo = base_lo + (uint32_t)i * 4;

        UInt32 v = 0;
        if ((*fw)->ReadQuadlet(fw, device, &addr, &v, false, 0) != kIOReturnSuccess) {
            continue;
        }
        v = __builtin_bswap32(v);
        if ((v & 0x10000) == 0) {
            continue;
        }
        printf("  0x%04x%08x = 0x%08x   channel=%u\n",
               base_hi, base_lo + (uint32_t)i * 4, v, (v >> 17) & 0x3F);
        hits++;
    }
    printf("  %d candidate register(s)\n", hits);
}

/*
 * Turn a run of hex digits (whitespace, ':' and '_' allowed) into bytes.
 * Returns the number of bytes written to `out`, or -1 on a bad digit or an
 * odd number of digits. `max` bounds the output.
 */
static int parse_hex_bytes(const char *s, uint8_t *out, int max)
{
    int n = 0;              // how many nibbles are pending in `hi`
    int hi = 0;             // the high nibble of the byte being assembled
    for (; *s != '\0'; s++) {
        char c = *s;
        if (c == ' ' || c == '\t' || c == ':' || c == '_') {
            continue;       // separators are cosmetic
        }
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            return -1;      // not a hex digit
        }
        if ((n & 1) == 0) {
            hi = v;         // first nibble of the pair
        } else {
            if (n / 2 >= max) {
                return -1;  // would overflow the caller's buffer
            }
            out[n / 2] = (uint8_t)((hi << 4) | v);
        }
        n++;
    }
    if ((n & 1) != 0) {
        return -1;          // dangling nibble
    }
    return n / 2;
}

/*
 * Write one quadlet. CISP::SetIsochChannel does exactly this through the HAL's
 * transporter interface slot 0x1EC, so this is the primitive the wiring is
 * built from.
 *
 * `value` is taken in the same representation find_chan_regs/dump_region print
 * (the device's big-endian value, e.g. 0x00010004). ReadQuadlet hands values
 * back byte-swapped, so by default this byte-swaps before writing to cancel
 * that out and make `--rmw` round-trip with what the read commands show.
 * `--no-bswap` writes the value raw for experiments.
 */
static int write_quadlet(IOFireWireLibDeviceRef fw, io_object_t device,
                         uint32_t base_hi, uint32_t base_lo,
                         uint32_t value, int do_bswap)
{
    FWAddress addr;
    memset(&addr, 0, sizeof(addr));
    addr.addressHi = base_hi;
    addr.addressLo = base_lo;

    UInt32 out = do_bswap ? __builtin_bswap32(value) : value;
    IOReturn r = (*fw)->WriteQuadlet(fw, device, &addr, out,
                                     false /* don't fail on reset */, 0);
    printf("write quadlet 0x%04x%08x <- 0x%08x%s -> 0x%08x\n",
           base_hi, base_lo, value, do_bswap ? " (byte-swapped on the wire)" : "",
           r);
    return r;
}

/*
 * Write a block of bytes. This is the counterpart to `--block` and to the
 * HAL's transporter interface slot 0x1F0, which the init-entry creators use
 * to put ISP/NCP/Sync entries on the device. Bytes are written exactly as
 * given (the device is big-endian, so supply big-endian bytes).
 */
static int write_block(IOFireWireLibDeviceRef fw, io_object_t device,
                       uint32_t base_hi, uint32_t base_lo,
                       const uint8_t *buf, uint32_t size)
{
    FWAddress addr;
    memset(&addr, 0, sizeof(addr));
    addr.addressHi = base_hi;
    addr.addressLo = base_lo;

    UInt32 len = size;
    IOReturn r = (*fw)->Write(fw, device, &addr, (void *)buf, &len,
                              false, 0);
    printf("write block   0x%04x%08x <- %u bytes -> 0x%08x (%u written)\n",
           base_hi, base_lo, size, r, len);
    return r;
}

/*
 * Read-modify-write one quadlet, in the printed (big-endian) space:
 *     new = (old & ~mask) | (value & mask)
 * This is the shape the plan wants for the wiring - change only the channel /
 * valid fields of a register and leave the rest alone.
 */
static int rmw_quadlet(IOFireWireLibDeviceRef fw, io_object_t device,
                       uint32_t base_hi, uint32_t base_lo,
                       uint32_t mask, uint32_t value, int do_bswap)
{
    FWAddress addr;
    memset(&addr, 0, sizeof(addr));
    addr.addressHi = base_hi;
    addr.addressLo = base_lo;

    UInt32 raw = 0;
    IOReturn r = (*fw)->ReadQuadlet(fw, device, &addr, &raw, false, 0);
    if (r != kIOReturnSuccess) {
        printf("rmw: read of 0x%04x%08x failed: 0x%08x\n", base_hi, base_lo, r);
        return r;
    }
    UInt32 old = __builtin_bswap32(raw);            // to the printed space
    UInt32 neu = (old & ~mask) | (value & mask);    // change the fields
    printf("rmw 0x%04x%08x: 0x%08x -> 0x%08x (mask 0x%08x, value 0x%08x)\n",
           base_hi, base_lo, old, neu, mask, value);
    return write_quadlet(fw, device, base_hi, base_lo, neu, do_bswap);
}

/* Big-endian field readers over a byte buffer (the device is big-endian). */
static uint32_t be32(const uint8_t *b, int o)
{
    return ((uint32_t)b[o] << 24) | ((uint32_t)b[o + 1] << 16)
         | ((uint32_t)b[o + 2] << 8) | (uint32_t)b[o + 3];
}
static uint32_t be16(const uint8_t *b, int o)
{
    return ((uint32_t)b[o] << 8) | (uint32_t)b[o + 1];
}

#define CORE_BASE 0xffffe0000020ULL   /* private space + 0x20, from CPrivateSpace */

/*
 * Read the core-space block (0x1120 bytes) and walk it the way
 * CCoreSpace::ParseDataBlock + CPlugLayoutTable::ParseDataBlock do:
 *   - the first 16 bytes are the ISP / NCP / Sync / Comm sub-space offsets;
 *   - the plug-layout table starts at +16 (a count, then layout records);
 *   - each layout names its ISP entries, then NCP, then the three Sync arrays.
 * This is read-only and only prints what it parses, so a wrong assumption shows
 * up as an out-of-range pointer rather than as damage.
 */
static void parse_core(IOFireWireLibDeviceRef fw, io_object_t device)
{
    // The whole core block, as big-endian bytes (buf[0] == device CORE_BASE).
    enum { CORE_SIZE = 0x1120 / 4 * 4 };   // round to quadlets
    uint8_t *buf = calloc(1, CORE_SIZE);
    if (buf == NULL) {
        return;
    }
    for (int i = 0; i < CORE_SIZE / 4; i++) {
        FWAddress a;
        memset(&a, 0, sizeof(a));
        a.addressHi = (uint32_t)(CORE_BASE >> 32);
        a.addressLo = (uint32_t)(CORE_BASE & 0xffffffffULL) + (uint32_t)i * 4;
        UInt32 v = 0;
        if ((*fw)->ReadQuadlet(fw, device, &a, &v, false, 0) == kIOReturnSuccess) {
            v = __builtin_bswap32(v);          // to big-endian order
            buf[i * 4 + 0] = (uint8_t)(v >> 24);
            buf[i * 4 + 1] = (uint8_t)(v >> 16);
            buf[i * 4 + 2] = (uint8_t)(v >> 8);
            buf[i * 4 + 3] = (uint8_t)(v);
        }
    }

    uint32_t isp_off  = be32(buf, 0);
    uint32_t ncp_off  = be32(buf, 4);
    uint32_t sync_off = be32(buf, 8);
    uint32_t comm_off = be32(buf, 12);
    printf("\n=== core space @ 0x%llx (%d bytes) ===\n",
           (unsigned long long)CORE_BASE, CORE_SIZE);
    printf("  ISP  data @ +0x%x (0x%llx)\n", isp_off,
           (unsigned long long)(CORE_BASE + isp_off));
    printf("  NCP  data @ +0x%x (0x%llx)\n", ncp_off,
           (unsigned long long)(CORE_BASE + ncp_off));
    printf("  Sync data @ +0x%x (0x%llx)\n", sync_off,
           (unsigned long long)(CORE_BASE + sync_off));
    printf("  Comm data @ +0x%x (0x%llx)\n", comm_off,
           (unsigned long long)(CORE_BASE + comm_off));

    // The plug-layout table data begins at core+16; a depth guard stops a
    // malformed pointer from looping.
    int p = 16;
    uint32_t nLayouts = be32(buf, p);
    p += 4;                                     // the count itself is consumed
    printf("  plug-layout table @ core+0x10, %u layout(s)\n", nLayouts);
    if (nLayouts > 64) {
        printf("   (implausible count - the offset model is wrong)\n");
        free(buf);
        return;
    }

    for (uint32_t l = 0; l < nLayouts; l++) {
        int layoutStart = p;
        uint32_t nameStored = be32(buf, p);
        uint32_t nameOff    = be32(buf, p + 4);
        uint32_t ispCount   = be32(buf, p + 8);
        int namePtr = p + 8 + (int)nameOff;
        p += 12;
        printf("\n  layout %u @ core+0x%x: nameStored=0x%x ispCount=%u names@core+0x%x\n",
               l, layoutStart, nameStored, ispCount, namePtr);

        for (uint32_t i = 0; i < ispCount; i++) {
            if (p + 4 > CORE_SIZE) { printf("    ISP %u: ran off the block\n", i); break; }
            uint32_t len = 4u * buf[p + 3];      // self-describing length
            printf("    ISP %u @ core+0x%x len=0x%x: raw=", i, p, len);
            for (int k = 0; k < 16 && p + k < CORE_SIZE; k++) printf("%02x", buf[p + k]);
            // CISPTypeEntry decodes: +0x18=BE16[4], +0x1c=BE16[6], +0x20=!(b8),
            // +0x21=(b8>>6)&1, +0x28=b8&0xf, +0x14=b9, +0x24=BE16[10], +0x2c=BE32[12]
            if (p + 16 <= CORE_SIZE) {
                printf("  valid=%d chan-ish[8]=0x%02x [9]=0x%02x cnt[12..15]=0x%x",
                       (buf[p + 8] < 0x80),
                       buf[p + 8], buf[p + 9], be32(buf, p + 12));
            }
            printf("\n");
            if (len == 0) break;
            p += (int)len;
        }

        // NCP chain: namePtr holds {u32 areaOff, u32 count} then 8-byte entries.
        int nameArea = namePtr;
        if (nameArea + 8 > CORE_SIZE) {
            printf("    (names out of range: core+0x%x)\n", nameArea);
            free(buf);
            return;
        }
        uint32_t ncpCount = be32(buf, nameArea + 4);
        int ncpPtr = nameArea + 4 + (int)be32(buf, nameArea);
        printf("    NCP count=%u @ core+0x%x\n", ncpCount, nameArea + 8);
        for (uint32_t i = 0; i < ncpCount; i++) {
            int q = nameArea + 8 + (int)i * 8;
            if (q + 8 > CORE_SIZE) break;
            printf("      NCP %u @ core+0x%x: ", i, q);
            for (int k = 0; k < 8; k++) printf("%02x", buf[q + k]);
            printf("  (BE16[0]=0x%x BE16[2]=0x%x b4=0x%02x b5=0x%02x b67=0x%x)\n",
                   be16(buf, q), be16(buf, q + 2), buf[q + 4], buf[q + 5],
                   (uint32_t)(((buf[q + 6] & 0x3f) << 8) | buf[q + 7]));
        }
        // Three Sync arrays follow, each {u32 off, u32 count} then 4-byte entries.
        int area = ncpPtr;
        const char *names[3] = { "SyncSrc", "SyncGrp", "SyncOut" };
        for (int s = 0; s < 3; s++) {
            if (area + 8 > CORE_SIZE) break;
            uint32_t cnt = be32(buf, area + 4);
            int entries = area + 8;
            printf("    %s count=%u @ core+0x%x:", names[s], cnt, entries);
            for (uint32_t i = 0; i < cnt && entries + 4 * (int)i + 4 <= CORE_SIZE; i++) {
                printf(" %08x", be32(buf, entries + (int)i * 4));
            }
            printf("\n");
            area = area + 4 + (int)be32(buf, area);   // next area
        }
        p = area;   // next layout starts after the SyncOut area
    }
    free(buf);
}

#define ISP_BASE 0xffffe00000e0ULL   /* core base + ISP offset */

/*
 * Read each ISP record's run-status register. CISP::IsActuallyRunning reads the
 * quadlet at (record base + 0x28) and returns bit 0; the record base is the
 * record's own address (CISPSpace::ParseDataBlock passes ISP_base + n*48).
 * Read-only.
 */
static void running_decode(IOFireWireLibDeviceRef fw, io_object_t device)
{
    enum { N = 6 };
    printf("\n=== ISP run status (record base + 0x28, bit 0) ===\n");
    for (int n = 0; n < N; n++) {
        FWAddress a;
        memset(&a, 0, sizeof(a));
        uint64_t at = ISP_BASE + (uint64_t)n * 48 + 0x28;
        a.addressHi = (uint32_t)(at >> 32);
        a.addressLo = (uint32_t)(at & 0xffffffffULL);
        UInt32 v = 0;
        IOReturn r = (*fw)->ReadQuadlet(fw, device, &a, &v, false, 0);
        if (r != kIOReturnSuccess) {
            printf("  ISP %d @ 0x%llx: read failed 0x%08x\n", n,
                   (unsigned long long)at, r);
            continue;
        }
        v = __builtin_bswap32(v);
        printf("  ISP %d @ 0x%llx = 0x%08x  running=%u\n", n,
               (unsigned long long)at, v, v & 1);
    }
}

/*
 * Decode the ISP data block: 48-byte records consumed by
 * CISPSpace::ParseDataBlock (0xcf82). This transcribes only the fields that
 * function derives from the first two big-endian words, which is where the
 * isoch channel lives; the upper bitfield words are left as raw bytes rather
 * than guessed at. Read-only.
 */
static void isp_decode(IOFireWireLibDeviceRef fw, io_object_t device)
{
    enum { REC = 12, N = 6 };                  // 6 records before the NCP block
    uint8_t buf[N * 48];
    memset(buf, 0, sizeof(buf));
    int fails = 0;
    for (int i = 0; i < N * REC; i++) {
        FWAddress a;
        memset(&a, 0, sizeof(a));
        a.addressHi = (uint32_t)(ISP_BASE >> 32);
        a.addressLo = (uint32_t)(ISP_BASE & 0xffffffffULL) + (uint32_t)i * 4;
        UInt32 v = 0;
        if ((*fw)->ReadQuadlet(fw, device, &a, &v, false, 0) == kIOReturnSuccess) {
            v = __builtin_bswap32(v);
            buf[i * 4 + 0] = (uint8_t)(v >> 24);
            buf[i * 4 + 1] = (uint8_t)(v >> 16);
            buf[i * 4 + 2] = (uint8_t)(v >> 8);
            buf[i * 4 + 3] = (uint8_t)(v);
        } else {
            fails++;
        }
    }

    printf("\n=== ISP data @ 0x%llx, %d x 48-byte records ===\n",
           (unsigned long long)ISP_BASE, N);
    // A failed read leaves the buffer zero, which would otherwise print as six
    // plausible-looking all-zero records. Say so loudly: an all-zero ISP dump is
    // not "the device is unconfigured", it is usually a device we could not read
    // (held exclusively by the vendor stack - see the Open() line - or no sudo).
    if (fails > 0) {
        printf("  *** %d of %d quadlet reads FAILED. The records below are\n"
               "      zero-filled placeholders, NOT device data. Check Open():\n"
               "      if it failed with 0xe00002c5 the vendor stack holds the\n"
               "      device (release it, or run with sudo).\n",
               fails, N * REC);
    }
    for (int r = 0; r < N; r++) {
        int o = r * 48;
        uint32_t w0 = be32(buf, o);            // -> entry bytes 0..3
        uint32_t w1 = be32(buf, o + 4);        // -> entry bytes 4..7 (channel)
        // Mirrors CISPSpace::ParseDataBlock's unpack of those two words.
        uint8_t e0 = (uint8_t)(((w0 >> 30) & 1) << 1 | (w0 >> 31) | ((w0 >> 21) & 0xF8));
        uint8_t e1 = (uint8_t)((w0 >> 16) & 0xFF);
        uint8_t e4 = (uint8_t)(((w1 >> 30) & 1) << 1 | (w1 >> 31) | ((w1 >> 21) & 0xF8));
        uint8_t e5 = (uint8_t)((w1 >> 16) & 0xFF);
        uint8_t e6 = (uint8_t)(((w1 >> 15) & 1) | ((w1 & 0x3F) << 1));
        unsigned channel = (e6 >> 1) & 0x3F;   // CISP::CreateInitEntryTable: (entry+6 >> 1) & 0x3F
        unsigned bit0    = e6 & 1;             // the separate 1-bit flag
        printf("  rec %d @ +0x%02x: w0=0x%08x w1=0x%08x  ->  e0=0x%02x e1=0x%02x "
               "e4=0x%02x e5=0x%02x e6=0x%02x  channel=%u flag=%u\n",
               r, o, w0, w1, e0, e1, e4, e5, e6, channel, bit0);
        printf("        raw:");
        for (int k = 0; k < 48; k++) printf(" %02x", buf[o + k]);
        printf("\n");
    }
}

/*
 * Probe which quadlets accept a write, without changing anything: for each
 * address, read the current value and write that same value back. A plain
 * register written with its own value is a no-op, so an accepted write reveals a
 * writable window and a refused one (address_error) reveals a read-only one.
 * Prints only the addresses whose write succeeded.
 */
static void probe_writes(IOFireWireLibDeviceRef fw, io_object_t device,
                         uint32_t base_hi, uint32_t base_lo, int count)
{
    printf("\n=== probing writes 0x%04x%08x .. +%d quadlets (write-back value) ===\n",
           base_hi, base_lo, count * 4);
    int writable = 0, readable = 0;
    // Collect the distinct failure codes. They mean very different things:
    //   0xe0008017  the device refused the write (address error)
    //   0xe0008020  a bus reset interrupted it (local, says nothing about the desk)
    //   0xe00002c5  another client holds the device
    IOReturn codes[4] = {0};
    int ncod = 0;
    for (int i = 0; i < count; i++) {
        FWAddress addr;
        memset(&addr, 0, sizeof(addr));
        addr.addressHi = base_hi;
        addr.addressLo = base_lo + (uint32_t)i * 4;
        UInt32 v = 0;
        if ((*fw)->ReadQuadlet(fw, device, &addr, &v, false, 0) != kIOReturnSuccess) {
            continue;                       // not readable - not our concern here
        }
        readable++;
        IOReturn w = (*fw)->WriteQuadlet(fw, device, &addr, v /* current value */,
                                         false, 0);
        if (w == kIOReturnSuccess) {
            printf("  WRITABLE  0x%04x%08x (value 0x%08x)\n",
                   base_hi, addr.addressLo, __builtin_bswap32(v));
            writable++;
        } else {
            int seen = 0;
            for (int k = 0; k < ncod; k++) {
                if (codes[k] == w) seen = 1;
            }
            if (!seen && ncod < 4) codes[ncod++] = w;
        }
    }
    printf("  %d readable, %d writable\n", readable, writable);
    if (ncod > 0) {
        printf("  write failure codes:");
        for (int k = 0; k < ncod; k++) {
            printf(" 0x%08x", codes[k]);
        }
        printf("\n    0xe0008017 = the device refused (address error);\n"
               "    0xe0008020 = local bus reset; 0xe00002c5 = held by another client\n");
    }
}

/*
 * Compare-and-swap (a 1394 "Lock") one quadlet. This is a different transaction
 * type from WriteQuadlet: instead of storing a value unconditionally, it stores
 * `newVal` only if the current value equals `cmpVal`, and returns the old value.
 * That atomicity is exactly why the mLAN "enabler" is set with a Lock rather
 * than a Write - in the vendor code, CTransporter::SetEnablerAddress issues a
 * compare-swap to the private-space base (0xFFFFE0000000), with masks that keep
 * the upper half. Section 45 only ever tested a plain WriteQuadlet, so this
 * transaction type is untested against the device.
 *
 * Values are named in the same printed (big-endian) space the read commands use;
 * `do_bswap` byte-swaps them on the wire, exactly like write_quadlet.
 */
static int lock_quadlet(IOFireWireLibDeviceRef fw, io_object_t device,
                        uint32_t base_hi, uint32_t base_lo,
                        uint32_t cmp, uint32_t newv, int do_bswap)
{
    FWAddress addr;
    memset(&addr, 0, sizeof(addr));
    addr.addressHi = base_hi;
    addr.addressLo = base_lo;

    // Swap both halves of the comparison the same way a write would, so the
    // printed values and the on-wire values stay consistent with --write.
    UInt32 c = do_bswap ? __builtin_bswap32(cmp)  : cmp;
    UInt32 n = do_bswap ? __builtin_bswap32(newv) : newv;
    IOReturn r = (*fw)->CompareSwap(fw, device, &addr, c, n,
                                    false /* don't fail on reset */, 0);
    printf("lock quadlet  0x%04x%08x: if==0x%08x then=0x%08x%s -> 0x%08x\n",
           base_hi, base_lo, cmp, newv,
           do_bswap ? " (byte-swapped on the wire)" : "", r);
    return r;
}

/*
 * Probe which quadlets accept a *Lock* (compare-and-swap), without changing
 * anything: read the current value, then compare-swap that same value onto
 * itself. Since cmpVal == newVal == the current value, an accepted lock rewrites
 * the same value and is a no-op - but whether the transaction is accepted at all
 * is the point. Section 45 found every private-space address refused a plain
 * WriteQuadlet; this asks whether the device accepts the Lock transaction type
 * there instead.
 */
static void probe_locks(IOFireWireLibDeviceRef fw, io_object_t device,
                        uint32_t base_hi, uint32_t base_lo, int count)
{
    printf("\n=== probing LOCKs (same-value compare-and-swap) 0x%04x%08x .. +%d quadlets ===\n",
           base_hi, base_lo, count * 4);
    int lockable = 0, readable = 0;
    for (int i = 0; i < count; i++) {
        FWAddress addr;
        memset(&addr, 0, sizeof(addr));
        addr.addressHi = base_hi;
        addr.addressLo = base_lo + (uint32_t)i * 4;
        UInt32 v = 0;
        if ((*fw)->ReadQuadlet(fw, device, &addr, &v, false, 0) != kIOReturnSuccess) {
            continue;                       // not readable - skip
        }
        readable++;
        // Compare-swap the value onto itself, so the write side is a no-op: a
        // success means the device took the Lock, an error means it refused it.
        IOReturn r = (*fw)->CompareSwap(fw, device, &addr, v, v, false, 0);
        if (r == kIOReturnSuccess) {
            printf("  LOCKABLE  0x%04x%08x (value 0x%08x)\n",
                   base_hi, addr.addressLo, __builtin_bswap32(v));
            lockable++;
        }
    }
    printf("  %d readable, %d lockable\n", readable, lockable);
}

/*
 * Allocate an isochronous channel in the bus's IRM. `--irm` is the last
 * host-side formality not yet tried: a talker is supposed to have its channel
 * allocated before it transmits, and the n12's config ROM says it is
 * IRM-capable. IOFireWireLib exposes the operation directly. This creates an
 * allocation, claims a channel and a bandwidth allowance, reports what the IRM
 * granted, holds for `seconds`, then releases. It writes only the bus's
 * CHANNELS_AVAILABLE / BANDWIDTH_AVAILABLE - never the device.
 */
static void irm_alloc(IOFireWireLibDeviceRef fw, int channel, uint32_t units,
                      int seconds)
{
    IOFireWireLibIRMAllocationRef irm = (*fw)->CreateIRMAllocation(
        fw, false, NULL, NULL,
        CFUUIDGetUUIDBytes(kIOFireWireIRMAllocationInterfaceID));
    if (irm == NULL) {
        printf("CreateIRMAllocation returned NULL (no IRM on the bus, or the\n"
               "  interface is older than v9)\n");
        return;
    }
    IOReturn r = (*irm)->allocateIsochResources(irm, (UInt8)channel, units);
    printf("allocateIsochResources(channel=%d, units=%u) -> 0x%08x\n",
           channel, units, r);
    UInt8 ac = 0;
    UInt32 au = 0;
    if ((*irm)->areIsochResourcesAllocated(irm, &ac, &au)) {
        printf("  allocated: channel=%u units=%u\n", ac, au);
    } else {
        printf("  not allocated\n");
    }
    printf("  holding for %d s (Ctrl-C to stop early)\n", seconds);
    for (int i = 0; i < seconds * 4; i++) {
        usleep(250 * 1000);
    }
    (*irm)->setReleaseIRMResourcesOnFree(irm, true);
    (*irm)->Release(irm);
    printf("  released\n");
}

/*
 * Restart the 1394 bus (a PHY "bus reset") and report the new generation.
 *
 * Why this exists: the desk refuses every configuration write and every Lock,
 * yet the same writes were accepted on Mojave, and its private space still holds
 * exactly the values the vendor manager put there. A bus reset is the one way to
 * make the desk re-initialise its link layer without cutting its power: every
 * node re-picks its node id, the desk restarts its async transaction handling,
 * and any write-protect a firmware may latch per bus generation is cleared.
 *
 * It cannot damage the desk's configuration, because that lives in the desk's
 * RAM and a bus reset does not touch it - it is the same event as plugging the
 * cable in. It only changes node ids and the generation counter, which every
 * later fw_read run re-reads for itself.
 *
 * @param fw  the open device interface (already opened by main)
 * @return    the IOReturn from BusReset: 0x00000000 means the reset was queued
 */
static IOReturn do_bus_reset(IOFireWireLibDeviceRef fw)
{
    printf("\n=== BUS RESET: restarts the 1394 bus (does not touch the desk's config) ===\n");

    // Ask the FireWire controller to reset the bus. The call returns as soon as
    // the reset is scheduled; the reset itself completes asynchronously.
    IOReturn r = (*fw)->BusReset(fw);
    printf("BusReset() -> 0x%08x\n", r);

    // Wait two seconds for the bus to settle: every node has to re-pick a node
    // id and the desk has to bring its link layer back up, and reading before
    // that finishes gives timeouts that look like a failure.
    for (int i = 0; i < 20; i++) {
        usleep(100 * 1000);
    }

    // The generation counter is incremented by every bus reset, so printing it
    // before and after proves whether the reset actually happened.
    UInt32 gen = 0;
    (*fw)->GetBusGeneration(fw, &gen);
    printf("bus generation is now %u\n", gen);
    printf("now re-run the probe: ./fw_read --probe 0xFFFFE00000E0 4\n");
    return r;
}

int main(int argc, char **argv)
{
    printf("fw_read - read the n12's own address space over FireWire\n\n");

    io_service_t dev = find_n12_device();
    if (dev == IO_OBJECT_NULL) {
        printf("no service of class \"%s\" is attached\n", service_class());
        if (!any_service() && strcmp(service_class(), "IOFireWireDevice") != 0) {
            printf("  (that node may have no GUID property - try N12_FW_ANY=1)\n");
        }
        return 1;
    }

    /* Say exactly which node was chosen: with N12_FW_CLASS in use this is the
     * whole point of the run, so it must be visible in the output. */
    io_name_t svc_name = {0};
    if (IORegistryEntryGetName(dev, svc_name) == KERN_SUCCESS) {
        printf("service: %s (matched class \"%s\")\n",
               svc_name, service_class());
    }

    CFTypeRef nodeRef = IORegistryEntryCreateCFProperty(
        dev, CFSTR("FireWire Node ID"), kCFAllocatorDefault, 0);
    uint64_t node_id = 0;
    if (nodeRef != NULL) {
        if (CFGetTypeID(nodeRef) == CFNumberGetTypeID()) {
            CFNumberGetValue((CFNumberRef)nodeRef, kCFNumberSInt64Type,
                             &node_id);
        }
        CFRelease(nodeRef);
    }
    printf("n12: GUID 0x%016llx, FireWire node id 0x%llx\n",
           (unsigned long long)N12_GUID, (unsigned long long)node_id);

    IOCFPlugInInterface **plugin = NULL;
    IOFireWireLibDeviceRef fw = open_firewire_lib(dev, &plugin);
    if (fw == NULL) {
        IOObjectRelease(dev);
        return 1;
    }

    // Read calls return kIOReturnNotOpen (0xe00002cd) until the interface is
    // opened. Open() takes exclusive access; Close() releases it.
    IOReturn or = (*fw)->Open(fw);
    printf("Open() -> 0x%08x\n", or);
    if (or != kIOReturnSuccess) {
        printf("  (this needs read/write access to the FireWire device; try sudo)\n");
    }

    UInt32 generation = 0;
    (*fw)->GetBusGeneration(fw, &generation);
    printf("bus generation %u\n", generation);

    // The device handle the read calls want. The documentation says to pass the
    // service the interface was built on, which is what GetDevice returns.
    io_object_t device = (*fw)->GetDevice(fw);
    printf("device handle: 0x%x (service was 0x%x)\n",
           (unsigned)device, (unsigned)dev);
    if (device == 0) {
        device = dev;
    }

    // `--no-bswap` anywhere on the command line turns off the default
    // byte-swap-before-write, so the raw quadlet is sent. Reads are unaffected.
    int do_bswap = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-bswap") == 0) {
            do_bswap = 0;
        }
    }

    int count = 64;
    if (argc > 2) {
        count = (int)strtol(argv[2], NULL, 0);   // allow 0x… as well as decimal
    }

    if (argc > 1 && strcmp(argv[1], "--plug") == 0) {
        // --plug   (read-only: decode the core-space / plug-layout tree)
        parse_core(fw, device);
    } else if (argc > 1 && strcmp(argv[1], "--isp") == 0) {
        // --isp    (read-only: decode the 48-byte ISP data records)
        isp_decode(fw, device);
    } else if (argc > 1 && strcmp(argv[1], "--running") == 0) {
        // --running (read-only: each ISP's run-status bit)
        running_decode(fw, device);
    } else if (argc > 1 && strcmp(argv[1], "--busreset") == 0) {
        // --busreset  (restart the 1394 bus, then run the probe again)
        do_bus_reset(fw);
    } else if (argc > 1 && strcmp(argv[1], "--irm") == 0) {
        // --irm CHANNEL [UNITS]  (allocate isoch resources in the bus IRM)
        int ch = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : 0;
        uint32_t units = (argc > 3 && argv[3][0] != '-')
                             ? (uint32_t)strtoul(argv[3], NULL, 0) : 100;
        irm_alloc(fw, ch, units, (argc > 4) ? (int)strtol(argv[4], NULL, 0) : 5);
    } else if (argc > 1 && strcmp(argv[1], "--probe") == 0) {
        // --probe BASE COUNT  (write-back probe: finds writable quadlets)
        unsigned long long base = (argc > 2) ? strtoull(argv[2], NULL, 0)
                                             : 0xffffe0000000ULL;
        int n = (argc > 3) ? (int)strtol(argv[3], NULL, 0) : 256;
        printf("\n=== WRITE MODE (probe): writes each register its own value ===\n");
        probe_writes(fw, device, (uint32_t)((base >> 32) & 0xffff),
                     (uint32_t)(base & 0xffffffffULL), n);
    } else if (argc > 1 && strcmp(argv[1], "--lockprobe") == 0) {
        // --lockprobe BASE COUNT  (SAFE: compare-swaps each register onto itself)
        unsigned long long base = (argc > 2) ? strtoull(argv[2], NULL, 0)
                                             : 0xffffe0000000ULL;
        int n = (argc > 3) ? (int)strtol(argv[3], NULL, 0) : 256;
        printf("\n=== LOCK MODE (safe: same-value compare-and-swap) ===\n");
        probe_locks(fw, device, (uint32_t)((base >> 32) & 0xffff),
                    (uint32_t)(base & 0xffffffffULL), n);
    } else if (argc > 1 && strcmp(argv[1], "--lock") == 0) {
        // --lock BASE CMP NEW [--no-bswap]   (a deliberate compare-and-swap)
        unsigned long long base = (argc > 2) ? strtoull(argv[2], NULL, 0)
                                             : 0xffffe0000000ULL;
        uint32_t cmp  = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
        uint32_t newv = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 0) : 0;
        printf("\n=== WRITE MODE (compare-and-swap): this changes the device ===\n");
        lock_quadlet(fw, device, (uint32_t)((base >> 32) & 0xffff),
                     (uint32_t)(base & 0xffffffffULL), cmp, newv, do_bswap);
    } else if (argc > 1 && strcmp(argv[1], "--write") == 0) {
        // --write BASE VALUE [--no-bswap]   (a deliberate device write)
        unsigned long long base = (argc > 2) ? strtoull(argv[2], NULL, 0)
                                             : 0xffffe0000894ULL;
        uint32_t value = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
        printf("\n=== WRITE MODE: this changes the device ===\n");
        write_quadlet(fw, device, (uint32_t)((base >> 32) & 0xffff),
                      (uint32_t)(base & 0xffffffffULL), value, do_bswap);
    } else if (argc > 1 && strcmp(argv[1], "--rmw") == 0) {
        // --rmw BASE MASK VALUE [--no-bswap]   (read-modify-write one quadlet)
        unsigned long long base = (argc > 2) ? strtoull(argv[2], NULL, 0)
                                             : 0xffffe0000894ULL;
        uint32_t mask = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0)
                                   : 0xffffffff;
        uint32_t value = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 0) : 0;
        printf("\n=== WRITE MODE (read-modify-write): this changes the device ===\n");
        rmw_quadlet(fw, device, (uint32_t)((base >> 32) & 0xffff),
                    (uint32_t)(base & 0xffffffffULL), mask, value, do_bswap);
    } else if (argc > 1 && strcmp(argv[1], "--writeblock") == 0) {
        // --writeblock BASE HEXBYTES          (a deliberate device block write)
        unsigned long long base = (argc > 2) ? strtoull(argv[2], NULL, 0)
                                             : 0xffffe0300000ULL;
        uint8_t buf[4096];
        int n = (argc > 3) ? parse_hex_bytes(argv[3], buf, (int)sizeof(buf)) : -1;
        printf("\n=== WRITE MODE (block): this changes the device ===\n");
        if (n < 0) {
            printf("--writeblock: HEXBYTES must be an even run of hex digits\n");
        } else {
            write_block(fw, device, (uint32_t)((base >> 32) & 0xffff),
                        (uint32_t)(base & 0xffffffffULL), buf, (uint32_t)n);
        }
    } else if (argc > 1 && strcmp(argv[1], "--findchan") == 0) {
        // --findchan BASE COUNT
        unsigned long long base = (argc > 2) ? strtoull(argv[2], NULL, 0)
                                             : 0xffffe0000000ULL;
        int n = (argc > 3) ? (int)strtol(argv[3], NULL, 0) : 2048;
        find_chan_regs(fw, device, (uint32_t)((base >> 32) & 0xffff),
                       (uint32_t)(base & 0xffffffffULL), n);
    } else if (argc > 1 && strcmp(argv[1], "--block") == 0) {
        // --block BASE SIZE
        unsigned long long base = (argc > 2) ? strtoull(argv[2], NULL, 0)
                                             : 0xffffe0400000ULL;
        uint32_t size = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0xC4;
        block_read(fw, device, (uint32_t)((base >> 32) & 0xffff),
                   (uint32_t)(base & 0xffffffffULL), size);
    } else if (argc > 1 && strcmp(argv[1], "--find") == 0) {        // --find BASE COUNT
        unsigned long long base = (argc > 2) ? strtoull(argv[2], NULL, 0)
                                             : 0xffffe0000000ULL;
        int n = (argc > 3) ? (int)strtol(argv[3], NULL, 0) : 2048;
        find_signature(fw, device, (uint32_t)((base >> 32) & 0xffff),
                       (uint32_t)(base & 0xffffffffULL), n);
    } else if (argc > 1) {
        // A 48-bit address: split into the 16-bit high half and 32-bit low half.
        unsigned long long base = strtoull(argv[1], NULL, 0);
        dump_region(fw, device, (uint32_t)((base >> 32) & 0xffff),
                    (uint32_t)(base & 0xffffffffULL), count);
    } else {
        // The areas that matter: core CSR, the IRM registers, the config ROM,
        // and the DICE II vendor block.
        dump_region(fw, device, 0xffff, 0xf0000000, 16);   // core CSR
        dump_region(fw, device, 0xffff, 0xf0000900, 8);    // IRM: bus manager,
                                                          // bandwidth, channels
        dump_region(fw, device, 0xffff, 0xf0000400, 32);   // config ROM
        dump_region(fw, device, 0xffff, 0xe0000000, 96);   // DICE vendor block
    }

    (*fw)->Close(fw);
    (*fw)->Release(fw);
    if (plugin != NULL) {
        (*plugin)->Release(plugin);
    }
    IOObjectRelease(dev);
    return 0;
}
