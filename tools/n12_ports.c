/*
 * n12_ports.c - capture harness for the n12's asynchronous (mLAN) control channel.
 *
 * Why this exists: the "1394 wiring" the n12 needs is not reachable by raw
 * register writes (docs/HOW-IT-WORKS.md - the device refuses them), so
 * the remaining candidate is the mLAN connection-management protocol carried
 * over FireWire *asynchronous* writes. That protocol's transport was recovered
 * (section 28) but its payload was never seen. This tool exists to see it.
 *
 * The vendor connection manager receives on a 0x200-byte "pseudo address space"
 * it registers on the bus (CmLANAsyncPort::Connect, section 27). IOFireWireLib
 * exposes exactly that primitive: CreatePseudoAddressSpace() gives a software
 * address space plus a write handler that fires for every write the device sends
 * to it. This tool registers one, logs every packet that arrives (hex, with a
 * timestamp), and can also *send* a packet - raw, or framed as the mACP message
 * of section 28 - so a real or reconstructed command can be replayed and the
 * device's reply recorded.
 *
 * It does not need the vendor bundle and it does not write device registers.
 * The one courtesy it performs is to print the address it registered, because
 * in mLAN the host must tell the device that address before the device will
 * write to it.
 *
 * Build:
 *     clang -Wall -O2 -o n12_ports n12_ports.c \
 *           -framework IOKit -framework CoreFoundation
 *
 * Run:
 *     ./n12_ports                       # register, log writes, until Ctrl-C
 *     ./n12_ports --seconds=30          # ... for 30 s
 *     ./n12_ports --log=/tmp/n12.log    # also append the log to a file
 *     ./n12_ports --size=0x200          # address-space size (default 0x200)
 *     ./n12_ports --send 0xFFFFE0000200 0123456789abcdef
 *     ./n12_ports --macp 0xFFFFE0000200 0123456789abcdef
 *     ./n12_ports --send ... --seconds=5   # send, then listen for replies
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>

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

static volatile sig_atomic_t g_stop;   // set by Ctrl-C so the run loop can end
static FILE *g_log;                    // optional log file (may be NULL)

/*
 * Write one line to stdout and, if a log file is open, to it as well. Keeping
 * both in step means a captured session can be re-read from the file exactly as
 * it appeared on screen.
 */
static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_log != NULL) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

/* Millisecond wall-clock stamp, for interleaving with other logs. */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Find the IOFireWireDevice whose GUID property is the n12's (as fw_read does). */
static io_service_t find_n12_device(void)
{
    CFMutableDictionaryRef matching = IOServiceMatching("IOFireWireDevice");
    if (matching == NULL) {
        return IO_OBJECT_NULL;
    }
    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(YAM_IO_PORT, matching, &iter) != KERN_SUCCESS) {
        return IO_OBJECT_NULL;
    }
    io_service_t found = IO_OBJECT_NULL;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        CFTypeRef guidRef = IORegistryEntryCreateCFProperty(
            svc, CFSTR("GUID"), kCFAllocatorDefault, 0);
        if (guidRef != NULL) {
            uint64_t guid = 0;
            if (CFGetTypeID(guidRef) == CFNumberGetTypeID()) {
                CFNumberGetValue((CFNumberRef)guidRef, kCFNumberSInt64Type, &guid);
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

/* Open IOFireWireLib on a device service; returns the interface or NULL. */
static IOFireWireLibDeviceRef open_firewire_lib(io_service_t service,
                                                IOCFPlugInInterface ***outPlugin)
{
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != KERN_SUCCESS || plugin == NULL) {
        emit("could not load IOFireWireLib: 0x%08x\n", kr);
        return NULL;
    }
    IOFireWireLibDeviceRef fw = NULL;
    HRESULT hr = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID), (LPVOID *)&fw);
    if (hr != S_OK || fw == NULL) {
        emit("QueryInterface for the device interface failed: 0x%08x\n", (unsigned)hr);
        (*plugin)->Release(plugin);
        return NULL;
    }
    *outPlugin = plugin;
    return fw;
}

/* Parse a hex string (separators allowed) into bytes; -1 on error. */
static int parse_hex_bytes(const char *s, uint8_t *out, int max)
{
    int n = 0, hi = 0;
    for (; *s != '\0'; s++) {
        char c = *s;
        if (c == ' ' || c == '\t' || c == ':' || c == '_') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        if ((n & 1) == 0) hi = v;
        else { if (n / 2 >= max) return -1; out[n / 2] = (uint8_t)((hi << 4) | v); }
        n++;
    }
    return (n & 1) ? -1 : n / 2;
}

/* Hex-dump `len` bytes one line, prefixed. */
static void emit_hex(const char *prefix, const uint8_t *b, UInt32 len)
{
    emit("%s", prefix);
    for (UInt32 i = 0; i < len; i++) emit("%02x", b[i]);
    emit("\n");
}

/*
 * The handler IOFireWireLib calls for every asynchronous write the device aims
 * at our address space. This is the device -> host direction of the control
 * protocol, so it is the one that actually needs capturing.
 */
static UInt32 write_handler(IOFireWireLibPseudoAddressSpaceRef as,
                            FWClientCommandID commandID,
                            UInt32 packetLen, void *packet,
                            UInt16 srcNodeID, UInt32 destHi, UInt32 destLo,
                            void *refCon)
{
    (void)refCon;
    char pfx[128];
    snprintf(pfx, sizeof(pfx), "[%10.3f] RX  node=0x%04x dest=0x%04x%08x len=%u  ",
             now_ms(), srcNodeID, destHi, destLo, packetLen);
    emit_hex(pfx, (const uint8_t *)packet, packetLen);

    // Acknowledge the write so the sender's transaction completes; without this
    // the device would see its writes time out.
    (*as)->ClientCommandIsComplete(as, commandID, kIOReturnSuccess);
    return kIOReturnSuccess;
}

/* Report writes IOFireWireLib had to drop, so a capture shows its own gaps. */
static void skipped_handler(IOFireWireLibPseudoAddressSpaceRef as,
                            void *refCon, UInt32 skippedPacketCount)
{
    (void)as;
    (void)refCon;
    emit("[%10.3f] !! dropped %u packet(s) - increase --queue\n",
         now_ms(), skippedPacketCount);
}

/*
 * Frame a payload as the mACP message of docs/HOW-IT-WORKS.md and send it as one
 * asynchronous block write to `addr`. The framing is:
 *     char[4] "mACP" | u8 version | u8 0 | u16 0
 *     u8 chunkCount | u8 sequence | u16 big-endian payload length
 *     payload | zero pad to 4 bytes | char[4] "EndP"
 * Returns the IOReturn.
 */
static IOReturn send_macp(IOFireWireLibDeviceRef fw, io_object_t device,
                          uint64_t addr, const uint8_t *payload, uint32_t len)
{
    uint32_t pad = (4 - (len & 3)) & 3;           // pad payload to a 4-byte boundary
    uint32_t total = 12 + len + pad + 4;          // header + payload + pad + trailer
    uint8_t *buf = calloc(1, total);
    if (buf == NULL) return kIOReturnNoMemory;

    memcpy(buf, "mACP", 4);
    buf[4] = 1;                                   // version
    buf[8] = 1;                                   // chunk count
    buf[9] = 0;                                   // sequence
    buf[10] = (uint8_t)(len >> 8);                // payload length, big-endian
    buf[11] = (uint8_t)(len);
    memcpy(buf + 12, payload, len);
    memcpy(buf + 12 + len + pad, "EndP", 4);

    FWAddress a;
    memset(&a, 0, sizeof(a));
    a.addressHi = (uint32_t)((addr >> 32) & 0xffff);
    a.addressLo = (uint32_t)(addr & 0xffffffffULL);
    UInt32 got = total;
    IOReturn r = (*fw)->Write(fw, device, &a, buf, &got, false, 0);

    char pfx[128];
    snprintf(pfx, sizeof(pfx), "[%10.3f] TX  mACP to 0x%llx len=%u -> 0x%08x  ",
             now_ms(), (unsigned long long)addr, len, r);
    emit_hex(pfx, buf, total);
    free(buf);
    return r;
}

int main(int argc, char **argv)
{
    printf("n12_ports - capture the n12's asynchronous control channel\n\n");

    uint32_t size = 0x200;      // the vendor port's receive address space is 0x200
    uint32_t queue = 0x4000;    // queue buffer for bursts
    int seconds = 0;            // 0 = until Ctrl-C
    uint64_t send_addr = 0;     // --send / --macp target
    uint8_t send_buf[4096];
    int send_len = -1;
    int use_macp = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--size=", 7) == 0) {
            size = (uint32_t)strtoul(argv[i] + 7, NULL, 0);
        } else if (strncmp(argv[i], "--queue=", 8) == 0) {
            queue = (uint32_t)strtoul(argv[i] + 8, NULL, 0);
        } else if (strncmp(argv[i], "--seconds=", 10) == 0) {
            seconds = (int)strtol(argv[i] + 10, NULL, 0);
        } else if (strncmp(argv[i], "--log=", 6) == 0) {
            g_log = fopen(argv[i] + 6, "a");
            if (g_log == NULL) fprintf(stderr, "cannot open log %s\n", argv[i] + 6);
        } else if (strcmp(argv[i], "--send") == 0 && i + 2 < argc) {
            send_addr = strtoull(argv[++i], NULL, 0);
            send_len = parse_hex_bytes(argv[++i], send_buf, (int)sizeof(send_buf));
            if (send_len < 0) { fprintf(stderr, "--send: bad hex\n"); return 1; }
        } else if (strcmp(argv[i], "--macp") == 0 && i + 2 < argc) {
            send_addr = strtoull(argv[++i], NULL, 0);
            send_len = parse_hex_bytes(argv[++i], send_buf, (int)sizeof(send_buf));
            if (send_len < 0) { fprintf(stderr, "--macp: bad hex\n"); return 1; }
            use_macp = 1;
        } else {
            fprintf(stderr,
                "usage: %s [--size=N] [--queue=N] [--seconds=N] [--log=FILE]\n"
                "          [--send ADDR HEX | --macp ADDR HEX]\n", argv[0]);
            return 1;
        }
    }

    io_service_t dev = find_n12_device();
    if (dev == IO_OBJECT_NULL) {
        printf("no IOFireWireDevice with the n12's GUID is attached\n");
        return 1;
    }

    IOCFPlugInInterface **plugin = NULL;
    IOFireWireLibDeviceRef fw = open_firewire_lib(dev, &plugin);
    if (fw == NULL) {
        IOObjectRelease(dev);
        return 1;
    }

    IOReturn or = (*fw)->Open(fw);
    printf("Open() -> 0x%08x\n", or);
    if (or != kIOReturnSuccess) {
        printf("  (needs read/write access to the FireWire device; try sudo)\n");
    }
    io_object_t device = (*fw)->GetDevice(fw);
    printf("device handle: 0x%x (service was 0x%x)\n", (unsigned)device, (unsigned)dev);

    UInt32 generation = 0;
    (*fw)->GetBusGeneration(fw, &generation);
    printf("bus generation %u\n", generation);

    /*
     * Register the receive address space. kFWAddressSpaceAutoCopyOnWrite keeps a
     * backing copy as well as notifying us, which matches what the vendor port
     * does; the handler still sees every packet.
     */
    void *backing = calloc(1, size);
    IOFireWireLibPseudoAddressSpaceRef as = (*fw)->CreatePseudoAddressSpace(
        fw, size, NULL /* refCon */, queue, backing,
        kFWAddressSpaceAutoCopyOnWrite, CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
    if (as == NULL) {
        printf("CreatePseudoAddressSpace failed\n");
        (*fw)->Close(fw);
        (*fw)->Release(fw);
        if (plugin != NULL) (*plugin)->Release(plugin);
        IOObjectRelease(dev);
        free(backing);
        return 1;
    }
    (*as)->SetWriteHandler(as, write_handler);
    (*as)->SetSkippedPacketHandler(as, skipped_handler);
    (*as)->TurnOnNotification(as);

    FWAddress myaddr;
    memset(&myaddr, 0, sizeof(myaddr));
    (*as)->GetFWAddress(as, &myaddr);
    printf("\nregistered receive address space: 0x%04x%08x, %u bytes\n",
           myaddr.addressHi, myaddr.addressLo, size);
    printf("  (in mLAN the host must publish this address to the device before\n"
           "   the device will write to it; this is the address to hand over)\n");

    // Handlers only fire once the callback dispatcher is on a run loop.
    CFRunLoopRef rl = CFRunLoopGetCurrent();
    (*fw)->AddCallbackDispatcherToRunLoop(fw, rl);

    // Optionally transmit first, so a reply can be captured in the same run.
    if (send_len >= 0) {
        if (use_macp) {
            send_macp(fw, device, send_addr, send_buf, (uint32_t)send_len);
        } else {
            FWAddress a;
            memset(&a, 0, sizeof(a));
            a.addressHi = (uint32_t)((send_addr >> 32) & 0xffff);
            a.addressLo = (uint32_t)(send_addr & 0xffffffffULL);
            UInt32 got = (UInt32)send_len;
            IOReturn r = (*fw)->Write(fw, device, &a, send_buf, &got, false, 0);
            char pfx[128];
            snprintf(pfx, sizeof(pfx), "[%10.3f] TX  raw to 0x%llx -> 0x%08x  ",
                     now_ms(), (unsigned long long)send_addr, r);
            emit_hex(pfx, send_buf, (UInt32)send_len);
        }
    }

    signal(SIGINT, on_sigint);
    printf("\nlistening%s - Ctrl-C to stop\n",
           seconds > 0 ? " (bounded)" : "");

    double deadline = (seconds > 0) ? now_ms() + (double)seconds * 1000.0 : 0.0;
    while (!g_stop) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
        if (deadline > 0.0 && now_ms() >= deadline) break;
    }

    printf("\nstopping\n");
    (*fw)->RemoveCallbackDispatcherFromRunLoop(fw);
    (*as)->TurnOffNotification(as);
    (*as)->Release(as);
    (*fw)->Close(fw);
    (*fw)->Release(fw);
    if (plugin != NULL) (*plugin)->Release(plugin);
    IOObjectRelease(dev);
    free(backing);
    if (g_log != NULL) fclose(g_log);
    return 0;
}
