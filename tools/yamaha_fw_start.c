/*
 * yamaha_fw_start.c - drive the Yamaha n12 driver from user space.
 *
 * Why this exists: the Yamaha kernel extensions load and start correctly on
 * macOS 12 Monterey, but they do not build the CoreAudio device on their own.
 * Their symbols show why - YamahaFWStreamDevice has
 *
 *     checkRequestFromConnectionManager
 *     requestAddDevices / requestSetDevices / requestStartStreams
 *     createAudioDevice / createMIDIDevice
 *
 * i.e. the audio device is created in response to a request from a user-space
 * "connection manager". That is the mLAN model: the host configures the 1394
 * wiring. Nothing on this Mac performs that role, because the shipped control
 * software is 32-bit.
 *
 * The kext nominates the client library to use, in its IOKitPersonalities:
 *
 *     IOCFPlugInTypes: FFC1AF3E-D305-4457-8150-2DFA2A684261
 *                      -> YamahaFWAudioDriver.kext/Contents/PlugIns/
 *                         YamahaFWUserLib.bundle
 *
 * and that bundle implements an IOCFPlugInInterface - it exports deviceProbe /
 * deviceStart / deviceStop with exactly the Probe / Start / Stop signatures.
 * So the standard IOKit plug-in mechanism is the intended way in, and this
 * program is the smallest thing that uses it.
 *
 * Build:
 *     clang -o yamaha_fw_start yamaha_fw_start.c \
 *           -framework IOKit -framework CoreFoundation
 *
 * Run:
 *     ./yamaha_fw_start
 *
 * Root is not needed: IOServiceOpen on this kext succeeds from a normal user
 * (verified 2026-11-01), even though the user client is created against the
 * caller's task. Use sudo only if a future selector turns out to want it.
 *
 * Requires the kexts to be installed in /Library/Extensions, not merely loaded:
 * IOCFPlugInTypes names a *relative* path, which IOKit resolves against the
 * standard extension directories.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach/mach.h>          // mach ports, mach_msg - the async notification path

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>

/* The CFPlugin *type* UUID from the kext's IOCFPlugInTypes dictionary. This is
 * not the interface UUID - it selects which plug-in to load for the service. */
#define YAMAHA_PLUGIN_TYPE "FFC1AF3E-D305-4457-8150-2DFA2A684261"

/* The service the audio driver publishes. It attaches to the FireWire local
 * node, so it exists as soon as the kext is loaded - it does not need the n12
 * to be present for us to open it. */
#define YAMAHA_SERVICE "YamahaFWStreamDevice"

/* kIOMasterPortDefault is deprecated as of macOS 12 and renamed
 * kIOMainPortDefault. The two are literally the same value (MACH_PORT_NULL) and
 * behave identically, so pick whichever the SDK in use provides, and keep the
 * build warning-free on both. */
#if defined(MAC_OS_VERSION_12_0) && MAC_OS_VERSION_12_0
#define YAM_IO_PORT kIOMainPortDefault
#else
#define YAM_IO_PORT kIOMasterPortDefault
#endif

/* Classes that should appear once the driver has been asked to build devices. */
static const char *kAudioDeviceClass = "YamahaFWAudioDevice";
static const char *kMidiDeviceClass  = "YamahaFWMIDIDevice";

/*
 * Count the live instances of an IOKit class. Used to tell whether the driver
 * actually built the audio device after we asked it to.
 *
 * IOServiceGetMatchingServices returns an iterator over every matching service;
 * we walk it and release as we go.
 */
static int count_services(const char *className)
{
    CFMutableDictionaryRef matching = IOServiceMatching(className);
    if (matching == NULL) {
        return -1;
    }
    io_iterator_t iter = IO_OBJECT_NULL;
    // This *consumes* a reference to matching, so we must not release it too.
    kern_return_t kr = IOServiceGetMatchingServices(YAM_IO_PORT,
                                                    matching, &iter);
    if (kr != KERN_SUCCESS) {
        return -1;
    }
    int n = 0;
    io_service_t service;
    while ((service = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        n++;
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return n;
}

/*
 * Find the first service of a class. Caller owns the returned object.
 */
static io_service_t find_service(const char *className)
{
    CFMutableDictionaryRef matching = IOServiceMatching(className);
    if (matching == NULL) {
        return IO_OBJECT_NULL;
    }
    return IOServiceGetMatchingService(YAM_IO_PORT, matching);
}

/*
 * Load the Yamaha plug-in for a service and Start it.
 *
 * IOCreatePlugInInterfaceForService does the work: it reads the service's
 * IOCFPlugInTypes, finds the matching bundle, loads it, and calls its factory.
 * The resulting IOCFPlugInInterface lets us Probe and Start it, which is what
 * reaches the driver's user client.
 */
static int start_plugin(io_service_t service)
{
    CFUUIDRef typeID = CFUUIDCreateFromString(NULL, CFSTR(YAMAHA_PLUGIN_TYPE));
    if (typeID == NULL) {
        fprintf(stderr, "could not parse the plug-in type UUID\n");
        return 1;
    }

    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;

    // kIOCFPlugInInterfaceID asks for the generic plug-in interface, which is
    // the one exposing Probe/Start/Stop.
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service, typeID, kIOCFPlugInInterfaceID, &plugin, &score);
    CFRelease(typeID);

    if (kr != KERN_SUCCESS || plugin == NULL) {
        fprintf(stderr,
                "IOCreatePlugInInterfaceForService failed: 0x%08x\n"
                "  - is the kext installed in /Library/Extensions?\n"
                "  - does that path still contain the PlugIns/"
                "YamahaFWUserLib.bundle?\n", kr);
        return 1;
    }
    printf("loaded the Yamaha plug-in (probe score %d)\n", (int)score);

    // Probe is optional here but harmless, and it is what the plug-in expects
    // to be called before Start.
    SInt32 order = 0;
    IOReturn pr = (*plugin)->Probe(plugin, NULL, service, &order);
    printf("Probe  -> 0x%08x (order %d)\n", pr, (int)order);

    // Start is the one that matters: in the Yamaha plug-in this is
    // IYamaha1394StreamDeviceClass::deviceStart, which opens the driver's user
    // client and tells it to begin.
    IOReturn sr = (*plugin)->Start(plugin, NULL, service);
    printf("Start  -> 0x%08x\n", sr);

    // Give the driver a moment to act on the request. The driver processes
    // requests on its work loop and posts completions asynchronously.
    printf("waiting for the driver to build devices...\n");
    for (int i = 0; i < 10; i++) {
        sleep(1);
        int audio = count_services(kAudioDeviceClass);
        int midi  = count_services(kMidiDeviceClass);
        printf("  t+%2ds  %s=%d  %s=%d\n",
               i + 1, kAudioDeviceClass, audio, kMidiDeviceClass, midi);
        if (audio > 0) {
            printf("\nthe audio device exists\n");
            break;
        }
    }

    // Do NOT call Stop or destroy the interface: the driver's streams should
    // outlive this process. Leaving the plug-in loaded is intentional for this
    // diagnostic. (A real connection manager would stop it on the way out.)
    return 0;
}

/* ---------------------------------------------------------------------------
 * Direct user-client interface
 *
 * The selectors below were recovered from the kext's own dispatch table:
 * YamahaFWStreamUserClient::sMethods is an IOExternalMethodDispatch array in
 * the binary's __DATA,__const, and the kext is not stripped, so every entry has
 * a symbol name. Dumping it gives the complete API, with the argument sizes the
 * driver enforces:
 *
 *   [ 0] sOpen                             0 in, 0 out
 *   [ 1] sClose                            0 in, 0 out
 *   [ 3] sCreateAudioDevice               48B struct in, 4B out
 *   [ 5] sGetNumAudioDevices               0 in, 1 out
 *   [ 9] sStartAudioDevice                 1 scalar in
 *   [61] sRequestStartStreams              0 in, 0 out
 *   [71] sGetNumberOfDevices               0 in, 1 out
 *   [72] sGetDeviceInfoByIndex             3 scalar in
 *   [83] sRequestSetDevices                3 scalar in
 *   [84] sRequestAddDevices                3 scalar in      <- the one we want
 *   [98] sReportStatus                     0 in, 0 out
 *
 * sRequestAddDevices is the call that should make the driver enumerate what is
 * on the FireWire bus and build the audio device. Note the driver enforces the
 * input sizes in this table, so calling a selector with the wrong counts is
 * rejected before any handler runs - which makes probing them safe.
 * ------------------------------------------------------------------------ */

#define SEL_OPEN                 0
#define SEL_CLOSE                1
#define SEL_CREATE_AUDIO_DEVICE  3
#define SEL_GET_NUM_AUDIO_DEV   5
#define SEL_START_AUDIO_DEVICE   9
#define SEL_REQUEST_START_STREAMS 61
#define SEL_GET_NUM_DEVICES     71
#define SEL_GET_DEVINFO_BY_INDEX 72
#define SEL_REQUEST_SET_DEVICES 83
#define SEL_REQUEST_ADD_DEVICES 84
#define SEL_REPORT_STATUS       98

/*
 * As probe_selector, but also sweeps structure input and output sizes.
 *
 * This matters because the plug-in's own wrapper for the call we care about is
 *   requestAddDevices(unsigned int, PJ_DEVICE_INFO*)
 * i.e. it passes a structure. IOConnectCallScalarMethod cannot express that, so
 * a scalar-only probe cannot find the accepted combination. IOConnectCallMethod
 * takes both scalars and structures, so this sweeps both.
 *
 * Still safe: IOUserClient::externalMethod validates every count against the
 * dispatch entry and rejects mismatches before the handler runs.
 */
static void probe_selector_full(io_connect_t conn, uint32_t sel, const char *name)
{
    static const size_t kInStruct[]  = { 0, 4, 8, 16, 24, 32, 48, 64, 96, 128, 256 };
    static const size_t kOutStruct[] = { 0, 4, 16, 24, 64, 96 };
    uint64_t inSc[4] = { 0, 0, 0, 0 };
    uint64_t outSc[4] = { 0, 0, 0, 0 };
    uint8_t  inStruct[256] = { 0 };
    uint8_t  outStruct[256] = { 0 };
    int accepted = 0;

    printf("  [%2d] %-24s (full sweep)\n", sel, name);
    for (uint32_t inCnt = 0; inCnt <= 3; inCnt++) {
        for (size_t k = 0; k < sizeof(kInStruct) / sizeof(kInStruct[0]); k++) {
            for (uint32_t outCnt = 0; outCnt <= 1; outCnt++) {
                for (size_t m = 0; m < sizeof(kOutStruct) / sizeof(kOutStruct[0]); m++) {
                    size_t inSz = kInStruct[k];
                    size_t outSz = kOutStruct[m];
                    uint32_t nOut = outCnt;
                    size_t nOutStruct = outSz;
                    kern_return_t kr = IOConnectCallMethod(
                        conn, sel,
                        inCnt ? inSc : NULL, inCnt,
                        inSz ? inStruct : NULL, inSz,
                        outCnt ? outSc : NULL, outCnt ? &nOut : NULL,
                        outSz ? outStruct : NULL, outSz ? &nOutStruct : NULL);
                    if (kr == KERN_SUCCESS) {
                        printf("       in %u scalar / %3zuB struct, "
                               "out %u scalar / %3zuB struct  -> OK",
                               inCnt, inSz, outCnt, outSz);
                        if (outCnt) {
                            printf("  out[0]=%llu", (unsigned long long)outSc[0]);
                        }
                        printf("\n");
                        accepted++;
                    } else if (kr != kIOReturnBadArgument &&
                               kr != kIOReturnUnsupported) {
                        // The handler ran and returned something else - useful.
                        printf("       in %u/%3zuB out %u/%3zuB -> 0x%08x\n",
                               inCnt, inSz, outCnt, outSz, kr);
                    }
                }
            }
        }
    }
    if (accepted == 0) {
        printf("       no combination accepted\n");
    }
}
/*
 * Scalar-only probe: try every small (input, output) scalar count.
 *
 * This is safe to do comprehensively because of how IOKit dispatches external
 * methods: IOUserClient::externalMethod compares the counts in the request
 * against the limits recorded in the IOExternalMethodDispatch entry and returns
 * kIOReturnBadArgument on a mismatch *before* calling the handler. So only the
 * correct combination ever reaches driver code.
 */
static void probe_selector(io_connect_t conn, uint32_t sel, const char *name)
{
    printf("  [%2d] %-24s\n", sel, name);
    for (uint32_t inCnt = 0; inCnt <= 3; inCnt++) {
        for (uint32_t outCnt = 0; outCnt <= 2; outCnt++) {
            uint64_t in[4] = { 0, 0, 0, 0 };
            uint64_t out[4] = { 0, 0, 0, 0 };
            uint32_t nOut = outCnt;
            kern_return_t kr = IOConnectCallScalarMethod(
                conn, sel, in, inCnt,
                outCnt ? out : NULL, outCnt ? &nOut : NULL);
            // Only print the interesting outcomes: an accepted call, or a
            // failure that is not the expected argument-count rejection.
            if (kr == KERN_SUCCESS) {
                printf("       in=%u out=%u -> OK        out[0]=%llu\n",
                       inCnt, outCnt, (unsigned long long)out[0]);
            } else if (kr != kIOReturnBadArgument) {
                printf("       in=%u out=%u -> 0x%08x (not BadArgument)\n",
                       inCnt, outCnt, kr);
            }
        }
    }
}
static void direct_user_client(io_service_t service)
{
    printf("\n--- direct user-client interface ---\n");

    io_connect_t conn = IO_OBJECT_NULL;
    // Type 0 is the client's default. The driver has exactly one user client
    // class, so there is nothing else it could be.
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        printf("IOServiceOpen failed: 0x%08x\n", kr);
        return;
    }
    printf("IOServiceOpen ok (connect 0x%x)\n", (unsigned)conn);

    // Rather than assume the argument counts, probe each selector. The driver
    // declares them in its dispatch table, and this discovers them for real.
    probe_selector(conn, SEL_OPEN, "sOpen");
    probe_selector(conn, SEL_GET_NUM_DEVICES, "sGetNumberOfDevices");
    probe_selector(conn, SEL_GET_NUM_AUDIO_DEV, "sGetNumAudioDevices");
    probe_selector(conn, SEL_GET_DEVINFO_BY_INDEX, "sGetDeviceInfoByIndex");
    probe_selector_full(conn, SEL_REQUEST_ADD_DEVICES, "sRequestAddDevices");
    probe_selector_full(conn, SEL_CREATE_AUDIO_DEVICE, "sCreateAudioDevice");
    probe_selector(conn, SEL_REPORT_STATUS, "sReportStatus");

    printf("  (report any line marked OK above - that is the working combination)\n");

    // Deliberately leave the connection open: the point is to leave the driver
    // in the "devices added" state so a separate step can create the audio
    // device. Closing here would undo the request.
    printf("  (leaving the user client open on purpose)\n");
}

/* ---------------------------------------------------------------------------
 * Async transport user client: YamahaFWAsync
 *
 * This is the layer underneath the audio driver. Both Yamaha kexts attach to
 * IOFireWireLocalNode, but only this one exposes the async transaction engine:
 * the audio driver has no async-receiver code of its own (it imports only
 * IOUserClient's setAsyncReference64 / sendAsyncResult64 helpers, not any
 * YamahaFWAsync symbol), so anything that receives asynchronous writes FROM
 * the n12 has to come through YFWAsyncUserClient.
 *
 * The selector table was recovered with tools/vtable_dump.py and confirmed:
 *
 *   [0] sOpen                           0 in, 0 struct  -> 0 out, 0 struct
 *   [1] sClose                          0 in             -> 0 out
 *   [2] sCreateReceiver                 0 scalar, 24B    -> 0 scalar, 12B
 *   [3] sDisposeReceiver                1 scalar         -> -
 *   [4] sDisposeReceiverAll             0                -> -
 *   [5] sActivateReceiver               1 scalar         -> -
 *   [6] sDeactivateReceiver             1 scalar         -> -
 *   [7] sGetReceivedData                2 scalars        -> 1048B struct
 *   [8] sResetReceiver                  1 scalar         -> -
 *
 * The struct layouts below were read out of the handler disassembly rather
 * than guessed. Notes that matter:
 *
 *  - `sCreateReceiver` dereferences IOExternalMethodArguments.asyncReference
 *    (it passes it straight to setAsyncReference64). A plain synchronous
 *    IOConnectCallMethod leaves that pointer NULL, so calling selector 2 that
 *    way would fault inside the kernel. It MUST be called with the async
 *    variant, IOConnectCallAsyncStructMethod.
 *
 *  - The driver reads the wake port from asyncReference[0], so the reference
 *    array's first element is set to our port as well as the call's wake_port.
 *
 *  - PBI_CreateReceiver.max_size is bounds-checked `<= 0x400` by
 *    CAsyncReceiver::init and each buffer entry is 0x418 bytes (0x14 header +
 *    0x400 of data), so 0x400 is the natural value.
 *
 *  - CAsyncReceiver numbers its entries with a counter that starts at
 *    0x88880000 and increments per received transaction; sGetReceivedData
 *    takes (receiver index, that sequence number) and fails for any other
 *    value. The same number is what the notification callback delivers, so a
 *    client either tracks the counter itself or waits on the port.
 * ------------------------------------------------------------------------ */

/* The service the async kext publishes. It exists as soon as the kext is
 * loaded; it does not need the n12 to be present. */
#define ASYNC_SERVICE "YamahaFWAsync"

/* Selector numbers, from the table above. */
#define ASEL_OPEN                 0
#define ASEL_CLOSE                1
#define ASEL_CREATE_RECEIVER      2
#define ASEL_DISPOSE_RECEIVER     3
#define ASEL_DISPOSE_RECEIVER_ALL 4
#define ASEL_ACTIVATE_RECEIVER    5
#define ASEL_DEACTIVATE_RECEIVER  6
#define ASEL_GET_RECEIVED_DATA    7
#define ASEL_RESET_RECEIVER       8

/* The sequence number CAsyncReceiver starts handing out. */
#define YFW_FIRST_SEQ 0x88880000u

/* kIOReturnNotFound: what sGetReceivedData reports for "nothing with that
 * sequence number is waiting". Not an error worth printing every poll. */
#define YFW_NO_DATA 0xe00002f0u

/* Input of sCreateReceiver: 24 bytes, called YFWAsyncPBI_CreateReceiver in the
 * kext. YFWAsyncUserClient::CreateReceiver reads it as four fields. */
typedef struct {
    uint32_t max_size;    // +0x00  largest async transaction, <= 0x400
    uint32_t num_entries; // +0x04  how many 0x418-byte buffer entries to make
    uint64_t ref1;        // +0x08  async reference the callback reports on
    uint64_t ref2;        // +0x10  second async reference value
} yfw_create_in_t;

/* Output of sCreateReceiver: 12 bytes, YFWAsyncPBO_CreateReceiver. The first
 * eight bytes are the FireWire address the receiver was registered at (the
 * structure FWAddressStruct the kext uses everywhere); the last four are the
 * receiver index every later call must quote. */
typedef struct {
    uint16_t node_id;   // +0x00
    uint16_t addr_hi;   // +0x02
    uint32_t addr_lo;   // +0x04
    uint32_t index;     // +0x08
} yfw_create_out_t;

/* One received transaction: 1048 bytes (0x418), YFWAsyncBufferEntry. The kext
 * fills the 0x14-byte header in CAsyncReceiver::CallbackWrite and copies the
 * payload in after it. */
typedef struct {
    uint32_t seq;     // +0x00  sequence number (starts at 0x88880000)
    uint32_t tcode;   // +0x04  FireWire transaction code
    uint32_t speed;   // +0x08  IOFWSpeed the packet arrived at
    uint32_t offset;  // +0x0c  offset within the receiver's address range
    uint32_t length;  // +0x10  valid bytes in `data`
    uint8_t  data[1028]; // +0x14  payload (0x400 max, plus slack)
} yfw_buffer_entry_t;

// These asserts are the whole point of writing the structs down: if the ABI
// assumptions are ever wrong, this stops compiling instead of misbehaving.
_Static_assert(sizeof(yfw_create_in_t)  == 24,   "PBI_CreateReceiver is 24B");
_Static_assert(sizeof(yfw_create_out_t) == 12,   "PBO_CreateReceiver is 12B");
_Static_assert(sizeof(yfw_buffer_entry_t) == 1048, "YFWAsyncBufferEntry is 1048B");

/*
 * Make a Mach port to receive the driver's asynchronous notifications on.
 *
 * A receive right is all we need: the driver only ever sends to us. Allocating
 * it here means the async call always has a valid port, even before we are
 * ready to look at what arrives.
 */
static mach_port_t make_wake_port(void)
{
    mach_port_t port = MACH_PORT_NULL;
    // MACH_PORT_RIGHT_RECEIVE gives us the ability to receive messages; the
    // kernel implicitly grants anyone who knows the port name the send right
    // needed to deliver them.
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                           &port) != KERN_SUCCESS) {
        return MACH_PORT_NULL;
    }
    return port;
}

/*
 * Take whatever is already waiting on the notification port, without blocking.
 *
 * The kext sends one async result per received transaction (the payload is the
 * sequence number). We do not commit to the message layout here - we print the
 * header and the first few words so the next session can pin it down, and
 * return any plausible sequence numbers we spot.
 *
 * The receive uses a 1 ms timeout: mach_msg's timeout argument counts in
 * milliseconds and 0 means "wait forever" (MACH_MSG_TIMEOUT_NONE), which is not
 * what a polling loop wants.
 */
static int drain_wake_port(mach_port_t port, uint32_t *seqs, int max_seqs)
{
    int found = 0;
    // Loop until the port is empty. mach_msg with a zero timeout is a poll.
    for (;;) {
        uint8_t buf[512];
        memset(buf, 0, sizeof(buf));
        mach_msg_header_t *hdr = (mach_msg_header_t *)buf;

        mach_msg_return_t mr = mach_msg(hdr,
                                        MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                                        0,                 // send size
                                        (mach_msg_size_t)sizeof(buf), // rcv size
                                        port,
                                        1,                 // timeout, ms
                                        MACH_PORT_NULL);
        if (mr != MACH_MSG_SUCCESS) {
            break;                     // MACH_RCV_TIMED_OUT = nothing waiting
        }

        printf("       [wake port] msg id=0x%x size=%u",
               hdr->msgh_id, (unsigned)hdr->msgh_size);
        // The result body follows the 24-byte header; dump it as words so the
        // shape is visible rather than assumed.
        size_t body = (size_t)hdr->msgh_size > sizeof(mach_msg_header_t)
                        ? (size_t)hdr->msgh_size - sizeof(mach_msg_header_t) : 0;
        for (size_t off = 0; off + 4 <= body && off < 32; off += 4) {
            uint32_t w;
            memcpy(&w, buf + sizeof(mach_msg_header_t) + off, sizeof(w));
            printf(" %08x", w);
            // The sequence number sits somewhere in the result; collect any
            // word that looks like one.
            if (w >= YFW_FIRST_SEQ && found < max_seqs) {
                seqs[found++] = w;
            }
        }
        printf("\n");
    }
    return found;
}

/*
 * Create one receiver through selector 2.
 *
 * Returns the IOReturn from the driver, and fills `*out` on success.
 *
 * This is the one call that must go out asynchronously: the handler feeds
 * IOExternalMethodArguments.asyncReference to setAsyncReference64 without
 * checking it, so a synchronous call (which leaves it NULL) would fault in the
 * kernel. IOConnectCallAsyncStructMethod supplies a real reference array.
 */
static kern_return_t async_create_receiver(io_connect_t conn, mach_port_t wake,
                                           uint32_t max_size, uint32_t entries,
                                           yfw_create_out_t *out)
{
    yfw_create_in_t in;
    memset(&in, 0, sizeof(in));
    in.max_size    = max_size;      // <= 0x400, enforced by the kext
    in.num_entries = entries;       // ring depth
    in.ref1        = (uint64_t)wake; // callback reports to our port
    in.ref2        = 0;

    // The kext takes the wake port from reference[0], so put it there. The
    // array is the standard 4-element async callback reference.
    uint64_t reference[4];
    memset(reference, 0, sizeof(reference));
    reference[0] = (uint64_t)wake;

    memset(out, 0, sizeof(*out));
    size_t out_size = sizeof(*out);

    return IOConnectCallAsyncStructMethod(conn, ASEL_CREATE_RECEIVER,
                                          wake,
                                          reference, 4,
                                          &in, sizeof(in),
                                          out, &out_size);
}

/*
 * Play the whole received-transaction line end to end: open, create, activate,
 * poll for data, then put everything back.
 *
 * Nothing here is destructive - it registers a local receiver address and reads
 * what the device sends to it - but it is a real kernel operation, so it is the
 * part that needs the machine's cooperation.
 */
static int drive_async(io_service_t service)
{
    printf("\n--- async transport user client (%s) ---\n", ASYNC_SERVICE);

    io_connect_t conn = IO_OBJECT_NULL;
    // Type 0 is the client class's default; the async kext has one user client.
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        printf("IOServiceOpen failed: 0x%08x\n", kr);
        return 1;
    }
    printf("IOServiceOpen ok (connect 0x%x)\n", (unsigned)conn);

    // sOpen: 0 scalars in, 0 out. Every other selector checks that we did this.
    kr = IOConnectCallScalarMethod(conn, ASEL_OPEN, NULL, 0, NULL, NULL);
    printf("sOpen -> 0x%08x\n", kr);

    mach_port_t wake = make_wake_port();
    if (wake == MACH_PORT_NULL) {
        printf("could not allocate a notification port\n");
        IOServiceClose(conn);
        return 1;
    }

    // Which (max_size, num_entries) shapes the driver accepts is not documented
    // anywhere, so try plausible ones in order and stop at the first success.
    static const uint32_t sizes[]  = { 0x400, 0x100, 0x40, 0x20, 1 };
    static const uint32_t counts[] = { 8, 4, 2, 1 };

    yfw_create_out_t created;
    int have_receiver = 0;
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]) && !have_receiver; si++) {
        for (size_t ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
            yfw_create_out_t out;
            kr = async_create_receiver(conn, wake, sizes[si], counts[ci], &out);
            printf("sCreateReceiver(max_size=0x%x, entries=%u) -> 0x%08x",
                   sizes[si], counts[ci], kr);
            if (kr == KERN_SUCCESS) {
                // The kext fills the output only on success; on failure it
                // leaves our zeroed struct alone.
                printf("  nodeID=0x%04x addr=0x%04x%08x index=%u\n",
                       out.node_id, out.addr_hi, out.addr_lo, out.index);
                created = out;
                have_receiver = 1;
                break;
            }
            printf("\n");
        }
    }
    if (!have_receiver) {
        printf("  no receiver could be created\n");
        IOServiceClose(conn);
        return 1;
    }

    // sActivateReceiver takes the receiver index as a single scalar.
    uint64_t arg[2];
    arg[0] = created.index;
    kr = IOConnectCallScalarMethod(conn, ASEL_ACTIVATE_RECEIVER, arg, 1,
                                   NULL, NULL);
    printf("sActivateReceiver(%u) -> 0x%08x\n", created.index, kr);
    if (kr != KERN_SUCCESS) {
        printf("  could not activate; nothing will arrive\n");
    }

    // Poll for transactions. The kext's entry numbers climb from 0x88880000 by
    // one for each received packet, so track that counter and ask for the next
    // expected number each time. Any notification the driver posts to our port
    // carries the same number; drain it too, and use whatever it says.
    printf("\n-- receiving (up to 10s) --\n");
    uint32_t expect = YFW_FIRST_SEQ;
    int received = 0;
    for (int i = 0; i < 100; i++) {
        // First, see whether the driver has pushed a sequence number at us.
        uint32_t notified[8];
        int nn = drain_wake_port(wake, notified, 8);
        for (int k = 0; k < nn; k++) {
            if (notified[k] > expect) {
                expect = notified[k];
            }
        }

        uint64_t in[2];
        in[0] = created.index;   // which receiver
        in[1] = expect;          // which entry, by sequence number
        yfw_buffer_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        size_t entry_size = sizeof(entry);

        // Two scalars in and one 1048-byte struct out: IOConnectCallMethod is
        // the only wrapper that carries both.
        kr = IOConnectCallMethod(conn, ASEL_GET_RECEIVED_DATA,
                                 in, 2,
                                 NULL, 0,
                                 NULL, NULL,
                                 &entry, &entry_size);
        if (kr == KERN_SUCCESS) {
            received++;
            printf("  seq=0x%08x tcode=0x%x speed=0x%x offset=0x%x len=%u\n",
                   entry.seq, entry.tcode, entry.speed,
                   entry.offset, entry.length);
            // Dump the payload. Cap the width so a big packet stays readable.
            size_t show = entry.length;
            if (show > sizeof(entry.data)) {
                show = sizeof(entry.data);
            }
            for (size_t o = 0; o < show; o += 16) {
                printf("   %04zx ", o);
                size_t row = show - o > 16 ? 16 : show - o;
                for (size_t b = 0; b < row; b++) {
                    printf("%02x ", entry.data[o + b]);
                }
                printf("\n");
            }
            expect = entry.seq + 1;
        } else if (kr != YFW_NO_DATA) {
            // Anything other than "not yet" is worth knowing about.
            printf("  sGetReceivedData(seq=0x%08x) -> 0x%08x\n", expect, kr);
        }
        usleep(100 * 1000);      // 100 ms between polls
    }
    printf("-- done: %d transaction(s) received --\n", received);

    // Put the kernel back the way we found it. clientDied would do this for us
    // when the process exits, but doing it explicitly leaves no doubt.
    arg[0] = created.index;
    IOConnectCallScalarMethod(conn, ASEL_DEACTIVATE_RECEIVER, arg, 1, NULL, NULL);
    IOConnectCallScalarMethod(conn, ASEL_DISPOSE_RECEIVER,    arg, 1, NULL, NULL);
    IOConnectCallScalarMethod(conn, ASEL_DISPOSE_RECEIVER_ALL, NULL, 0, NULL, NULL);
    IOConnectCallScalarMethod(conn, ASEL_CLOSE, NULL, 0, NULL, NULL);
    IOServiceClose(conn);
    return 0;
}

/* ---------------------------------------------------------------------------
 * sCreateAudioDevice: the short route to a YamahaFWAudioDevice
 *
 * The connection manager's full path builds _PJ_DEVICE_INFO (4808 bytes per
 * device) and hands it over with sRequestAddDevices. But that call is
 * bookkeeping *after the fact*: the audio device itself is made by
 * sCreateAudioDevice, whose input is small (see docs/HOW-IT-WORKS.md).
 *
 * YamahaFWStreamDevice::createAudioDeviceFunction validates the four
 * pointer/length pairs, constructs a YamahaFWAudioDevice (0x170 bytes), and
 * only walks its list of already-created audio devices to reject duplicates.
 * It never consults the FireWire device table that sRequestAddDevices fills, so
 * an empty table is fine.
 *
 * The 48-byte input, Yamaha1394StreamIF::CreateAudioDeviceParam, as filled by
 * IYamaha1394StreamDeviceClass::createAudioDevice and read by the driver:
 *
 *   +0x00  uint64  guid
 *   +0x08  uint32  field08        (setter 0xae8; meaning not yet pinned down)
 *   +0x0c  uint32  index          (must be <= 1; duplicate key and setter 0xad8)
 *   +0x10  uint64  name length    } a NUL-terminated UTF-8 C string in the
 *   +0x18  uint64  name pointer   } caller's address space, copied in by
 *   +0x20  uint64  vendor length  } IYamaha1394StreamDeviceClass::SetStringParam
 *   +0x28  uint64  vendor pointer } and turned into an OSString
 *
 * The string pair mirrors SetStringParam: it allocates
 * CFStringGetMaximumSizeForEncoding(len, UTF-8) + 1 bytes, writes the C string
 * with CFStringGetCString, and stores {length, pointer}. The driver's
 * createOSStringFromUserData allocates the same size, copyUserData's it in, and
 * calls OSString::withCString - so length must include the terminating NUL.
 * ------------------------------------------------------------------------ */

/* The remaining selector the earlier sections did not name. */
#define SEL_GET_AUDIO_DEVICE 6

typedef struct {
    uint64_t guid;         // +0x00
    uint32_t field08;      // +0x08
    uint32_t index;        // +0x0c  <= 1
    uint64_t name_size;    // +0x10  bytes, including the NUL
    uint64_t name_ptr;     // +0x18  valid in *our* address space
    uint64_t vendor_size;  // +0x20
    uint64_t vendor_ptr;   // +0x28
} yfw_create_audio_dev_t;

_Static_assert(sizeof(yfw_create_audio_dev_t) == 48,
               "CreateAudioDeviceParam is 48 bytes");

/* Shorthand so a failed call can be reported with its name and code. */
static void report(const char *what, kern_return_t kr)
{
    printf("  %-34s -> 0x%08x\n", what, kr);
}

/*
 * Ask the driver to build a YamahaFWAudioDevice for the n12, then look for it.
 *
 * The name buffers must stay alive for the duration of the call only, but they
 * are file-scope statics so the addresses are obvious in the output.
 */
static int create_audio_device(io_service_t service, uint32_t field08)
{
    printf("\n--- sCreateAudioDevice ---\n");

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        printf("IOServiceOpen failed: 0x%08x\n", kr);
        return 1;
    }
    printf("IOServiceOpen ok (connect 0x%x)\n", (unsigned)conn);

    // sOpen first: every other selector checks that it was called.
    kr = IOConnectCallScalarMethod(conn, SEL_OPEN, NULL, 0, NULL, NULL);
    report("sOpen", kr);

    // Baseline. Both should be zero before we ask for anything.
    uint64_t n = 0;
    uint32_t ncnt = 1;
    kr = IOConnectCallScalarMethod(conn, SEL_GET_NUM_DEVICES, NULL, 0, &n, &ncnt);
    printf("  sGetNumberOfDevices               -> 0x%08x", kr);
    if (kr == KERN_SUCCESS) printf("  (%llu)", (unsigned long long)n);
    printf("\n");

    uint64_t na = 0;
    uint32_t nacnt = 1;
    kr = IOConnectCallScalarMethod(conn, SEL_GET_NUM_AUDIO_DEV, NULL, 0,
                                   &na, &nacnt);
    printf("  sGetNumAudioDevices               -> 0x%08x", kr);
    if (kr == KERN_SUCCESS) printf("  (%llu)", (unsigned long long)na);
    printf("\n");

    // The two names. SetStringParam uses the CFString's maximum UTF-8 size + 1,
    // and the driver copies exactly `size` bytes, so include the NUL.
    static char name1[]   = "n12";
    static char name2[]   = "Yamaha";

    yfw_create_audio_dev_t param;
    memset(&param, 0, sizeof(param));
    param.guid        = 0x00A0DE0000011013ULL;  // the n12, from its config ROM
    param.field08     = field08;
    param.index       = 0;                     // <= 1; duplicate key
    param.name_size   = sizeof(name1);
    param.name_ptr    = (uint64_t)(uintptr_t)name1;
    param.vendor_size = sizeof(name2);
    param.vendor_ptr  = (uint64_t)(uintptr_t)name2;

    printf("  param: guid=0x%016llx field08=0x%x index=%u"
           " name=\"%s\"(%llu) vendor=\"%s\"(%llu)\n",
           (unsigned long long)param.guid, param.field08, param.index,
           name1, (unsigned long long)param.name_size,
           name2, (unsigned long long)param.vendor_size);

    uint32_t out_index = 0xffffffff;
    size_t out_size = sizeof(out_index);
    // 48-byte struct in, 4-byte struct out: IOConnectCallStructMethod.
    kr = IOConnectCallStructMethod(conn, SEL_CREATE_AUDIO_DEVICE,
                                   &param, sizeof(param),
                                   &out_index, &out_size);
    printf("  sCreateAudioDevice                -> 0x%08x", kr);
    if (kr == KERN_SUCCESS) printf("  index=%u", out_index);
    printf("\n");

    // Look for it.
    na = 0;
    nacnt = 1;
    kr = IOConnectCallScalarMethod(conn, SEL_GET_NUM_AUDIO_DEV, NULL, 0,
                                   &na, &nacnt);
    printf("  sGetNumAudioDevices               -> 0x%08x", kr);
    if (kr == KERN_SUCCESS) printf("  (%llu)", (unsigned long long)na);
    printf("\n");

    // sGetAudioDevice(index) writes a 24-byte struct out.
    if (na > 0) {
        uint64_t idx = 0;
        uint8_t detail[24];
        memset(detail, 0, sizeof(detail));
        size_t detail_size = sizeof(detail);
        kr = IOConnectCallMethod(conn, SEL_GET_AUDIO_DEVICE,
                                 &idx, 1,
                                 NULL, 0,
                                 NULL, NULL,
                                 detail, &detail_size);
        printf("  sGetAudioDevice(0)                -> 0x%08x  %zu bytes:",
               kr, detail_size);
        for (size_t i = 0; i < detail_size && i < sizeof(detail); i++) {
            printf(" %02x", detail[i]);
        }
        printf("\n");
    }

    printf("  %s matches %d\n", kAudioDeviceClass,
           count_services(kAudioDeviceClass));

    // Leave no half-built device behind: sDisposeAudioDevice is selector 4.
    if (na > 0) {
        uint64_t idx = 0;
        kr = IOConnectCallScalarMethod(conn, 4, &idx, 1, NULL, NULL);
        report("sDisposeAudioDevice(0)", kr);
    }

    IOServiceClose(conn);
    return 0;
}

/*
 * Find the audio driver's service and run just the create-device experiment.
 */
static int run_create_device_layer(uint32_t field08)
{
    io_service_t service = find_service(YAMAHA_SERVICE);
    if (service == IO_OBJECT_NULL) {
        fprintf(stderr, "service %s not found - kexts not loaded\n",
                YAMAHA_SERVICE);
        return 1;
    }
    printf("\nfound service %s (io_service_t 0x%x)\n",
           YAMAHA_SERVICE, (unsigned)service);
    int rc = create_audio_device(service, field08);
    IOObjectRelease(service);
    return rc;
}

/* ---------------------------------------------------------------------------
 * The isochronous and audio stream steps
 *
 * With an audio device in hand, the driver exposes the isochronous layer
 * directly - selectors 34-48 - so no Enabler is needed to set up a stream:
 *
 *   [34] sCreateIsochStreamParam  4 scalars in -> 1 scalar out
 *   [36] sGetNumIsochStreams      1 scalar in  -> 1 scalar out
 *   [37] sGetIsochStream          2 scalars in -> 1 scalar out
 *   [38] sGetIsochStreamInfo      1 scalar in  -> 3 scalars out
 *   [11] sCreateAudioStream       40B struct in -> 4B struct out
 *   [ 9] sStartAudioDevice        1 scalar in
 *
 * CStreamDevice::isochStreamingCreateAudioOut/In, reached from
 * YamahaFWStreamDevice::createIsochStreaming, reads the CIsochStreamParam the
 * driver allocated at selector 34 as:
 *
 *   +0x08  uint32  isoch channel
 *   +0x0c  uint8   direction - if set the driver calls ...CreateAudioIn,
 *                             otherwise ...CreateAudioOut
 *   +0x10  uint32  audio sequences (used by CreateAudioOut)
 *   +0x14  uint32  MIDI ports      (used by CreateMIDIOut)
 *
 * and then calls CStreamDevice::isochStreamingCreateAudioIn/Out, which needs
 * the CStreamDevice at device+0x3a48 - i.e. the audio device must exist first,
 * which is why this runs after sCreateAudioDevice.
 *
 * The 40-byte audio-stream input, read from
 * IYamaha1394StreamDeviceClass::createAudioStream:
 *
 *   +0x00  uint32  audio-device reference
 *   +0x04  uint32  isoch-stream reference
 *   +0x08  uint32  (unknown)
 *   +0x0c  uint32  (unknown)
 *   +0x10  uint8   flag
 *   +0x18  uint64  name size   } same NUL-terminated UTF-8 pair as the device
 *   +0x20  uint64  name pointer}
 * ------------------------------------------------------------------------ */

#define SEL_START_AUDIO_DEVICE      9
#define SEL_STOP_AUDIO_DEVICE       10
#define SEL_CREATE_AUDIO_STREAM     11
#define SEL_DISPOSE_AUDIO_STREAM    12
#define SEL_GET_NUM_AUDIO_STREAMS   13
#define SEL_CREATE_ISOCH_PARAM      34
#define SEL_DISPOSE_ISOCH_PARAM     35
#define SEL_GET_NUM_ISOCH_STREAMS   36
#define SEL_GET_ISOCH_STREAM        37
#define SEL_GET_ISOCH_STREAM_INFO   38

typedef struct {
    uint32_t audio_dev;     // +0x00
    uint32_t isoch_stream;  // +0x04
    uint32_t field08;       // +0x08
    uint32_t field0c;       // +0x0c
    uint8_t  flag;          // +0x10
    uint8_t  pad[7];
    uint64_t name_size;     // +0x18
    uint64_t name_ptr;      // +0x20
} yfw_create_audio_stream_t;

_Static_assert(sizeof(yfw_create_audio_stream_t) == 40,
               "CreateAudioStreamParam is 40 bytes");

/* Seconds to keep a started device alive so it can be observed from outside. */
static int g_hold = 0;

/*
 * Build the device, then try to build one isochronous stream and one audio
 * stream and start the device.
 *
 * The isoch channel and sequence counts the device actually wants are not known
 * yet - they are what the mLAN transport reports - so the isoch creation is
 * swept over a few plausible shapes and the first accepted one is used.
 */
static int start_device_experiment(io_service_t service, uint32_t field08)
{
    printf("\n--- build device, isoch stream, audio stream, start ---\n");

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        printf("IOServiceOpen failed: 0x%08x\n", kr);
        return 1;
    }
    kr = IOConnectCallScalarMethod(conn, SEL_OPEN, NULL, 0, NULL, NULL);
    report("sOpen", kr);

    /* --- 1. the audio device --------------------------------------------- */
    static char dev_name[]    = "n12";
    static char vendor_name[] = "Yamaha";

    yfw_create_audio_dev_t dp;
    memset(&dp, 0, sizeof(dp));
    dp.guid        = 0x00A0DE0000011013ULL;
    dp.field08     = field08;
    dp.index       = 0;
    dp.name_size   = sizeof(dev_name);
    dp.name_ptr    = (uint64_t)(uintptr_t)dev_name;
    dp.vendor_size = sizeof(vendor_name);
    dp.vendor_ptr  = (uint64_t)(uintptr_t)vendor_name;

    uint32_t dev_index = 0xffffffff;
    size_t dev_out = sizeof(dev_index);
    kr = IOConnectCallStructMethod(conn, SEL_CREATE_AUDIO_DEVICE,
                                   &dp, sizeof(dp), &dev_index, &dev_out);
    printf("  sCreateAudioDevice                -> 0x%08x", kr);
    if (kr == KERN_SUCCESS) printf("  index=%u", dev_index);
    printf("\n");
    if (kr != KERN_SUCCESS) {
        IOServiceClose(conn);
        return 1;
    }

    /* --- 2. an isochronous stream ---------------------------------------- */
    // A candidate is (channel, is_input, audio_sequences, midi_ports). The
    // n12 moves 16 channels each way, so 16 sequences is the natural guess;
    // the channel is swept because it is negotiated with the device.
    static const uint32_t channels[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    uint32_t isoch_channel = 0;
    uint32_t isoch_index = 0;
    int have_isoch = 0;

    for (int is_in = 0; is_in <= 1 && !have_isoch; is_in++) {
        for (size_t c = 0; c < sizeof(channels) / sizeof(channels[0]); c++) {
            uint64_t in[4];
            in[0] = channels[c];        // isoch channel
            in[1] = (uint64_t)is_in;    // direction: 1 = CreateAudioIn
            in[2] = 16;                 // audio sequences
            in[3] = 1;                  // MIDI ports

            uint64_t idx = 0;
            uint32_t icnt = 1;
            kr = IOConnectCallScalarMethod(conn, SEL_CREATE_ISOCH_PARAM,
                                           in, 4, &idx, &icnt);
            if (kr == KERN_SUCCESS) {
                printf("  sCreateIsochStreamParam(ch=%u in=%d seq=16 midi=1)"
                       " -> 0x00000000  index=%llu\n",
                       channels[c], is_in, (unsigned long long)idx);
                isoch_channel = channels[c];
                isoch_index = (uint32_t)idx;
                have_isoch = 1;
                break;
            }
            if (kr != kIOReturnBadArgument && kr != kIOReturnUnsupported) {
                printf("  sCreateIsochStreamParam(ch=%u in=%d) -> 0x%08x\n",
                       channels[c], is_in, kr);
            }
        }
    }
    if (!have_isoch) {
        printf("  no isoch stream could be created\n");
        printf("  -> the channel/sequence values must come from the device\n");
        uint64_t d = 0;
        IOConnectCallScalarMethod(conn, 4, &d, 1, NULL, NULL);   // dispose device
        IOServiceClose(conn);
        return 1;
    }

    /* --- 3. the audio stream that references it -------------------------- */
    static char stream_name[] = "n12 out";
    int have_stream = 0;

    // createAudioStreamFunction rejects the param with kIOReturnBadArgument
    // when +0x0c is zero but +0x04 (the isoch reference) is not, so +0x0c is
    // only meaningful alongside an isoch stream. Its exact meaning is not yet
    // pinned down - it is handed to the audio device as the stream's second
    // argument - so sweep a few plausible (field08, field0c, flag) shapes.
    struct { uint32_t f08, f0c; uint8_t flag; } combos[] = {
        { 0, 16, 0 }, { 0, 1, 0 }, { 16, 1, 0 }, { 1, 1, 0 },
        { 0, 8, 0 },  { 0, 16, 1 }, { 0, 1, 1 },
    };

    uint32_t str_index = 0xffffffff;
    for (size_t i = 0; i < sizeof(combos) / sizeof(combos[0]); i++) {
        yfw_create_audio_stream_t sp;
        memset(&sp, 0, sizeof(sp));
        sp.audio_dev    = dev_index;
        sp.isoch_stream = isoch_index;
        sp.field08      = combos[i].f08;
        sp.field0c      = combos[i].f0c;
        sp.flag         = combos[i].flag;
        sp.name_size    = sizeof(stream_name);
        sp.name_ptr     = (uint64_t)(uintptr_t)stream_name;

        str_index = 0xffffffff;
        size_t str_out = sizeof(str_index);
        kr = IOConnectCallStructMethod(conn, SEL_CREATE_AUDIO_STREAM,
                                       &sp, sizeof(sp), &str_index, &str_out);
        if (kr == KERN_SUCCESS) {
            printf("  sCreateAudioStream(+0x08=%u +0x0c=%u flag=%u)"
                   " -> 0x00000000  index=%u\n",
                   combos[i].f08, combos[i].f0c, combos[i].flag, str_index);
            have_stream = 1;
            break;
        }
        printf("  sCreateAudioStream(+0x08=%u +0x0c=%u flag=%u) -> 0x%08x\n",
               combos[i].f08, combos[i].f0c, combos[i].flag, kr);
    }

    if (have_stream) {
        uint64_t ne = dev_index;
        uint64_t nstr = 0;
        // sGetNumAudioStreams takes two scalars: the audio device and a kind.
        uint64_t qin[2] = { dev_index, 0 };
        uint32_t ncnt = 1;
        kr = IOConnectCallScalarMethod(conn, SEL_GET_NUM_AUDIO_STREAMS,
                                       qin, 2, &nstr, &ncnt);
        printf("  sGetNumAudioStreams               -> 0x%08x  (%llu)\n",
               kr, (unsigned long long)nstr);

        /* --- 4. start it ------------------------------------------------- */
        kr = IOConnectCallScalarMethod(conn, SEL_START_AUDIO_DEVICE,
                                       &ne, 1, NULL, NULL);
        report("sStartAudioDevice(0)", kr);

        printf("  %s matches %d\n", kAudioDeviceClass,
               count_services(kAudioDeviceClass));

        // Optionally sit here with the device running, so it can be seen from
        // outside (CoreAudio, ioreg) while this process holds the client open.
        if (kr == KERN_SUCCESS && g_hold > 0) {
            printf("  holding the started device for %d s (client stays open)...\n",
                   g_hold);
            for (int t = 0; t < g_hold; t++) {
                sleep(1);
                int n = count_services(kAudioDeviceClass);
                printf("    t+%2ds  %s=%d\n", t + 1, kAudioDeviceClass, n);
            }
        }
    }

    /* --- clean up -------------------------------------------------------- */
    // The user client is closed on exit, which the driver treats like a client
    // death, but tidy up explicitly so repeated runs start from zero.
    uint64_t d = dev_index;
    uint64_t iso = isoch_index;
    IOConnectCallScalarMethod(conn, SEL_STOP_AUDIO_DEVICE, &d, 1, NULL, NULL);
    printf("  (isoch channel used: %u; disposing the device and isoch stream)\n",
           isoch_channel);
    IOConnectCallScalarMethod(conn, SEL_DISPOSE_ISOCH_PARAM, &iso, 1, NULL, NULL);
    IOConnectCallScalarMethod(conn, 4, &d, 1, NULL, NULL);   // sDisposeAudioDevice

    IOServiceClose(conn);
    return 0;
}

static int run_start_device_layer(uint32_t field08)
{
    io_service_t service = find_service(YAMAHA_SERVICE);
    if (service == IO_OBJECT_NULL) {
        fprintf(stderr, "service %s not found - kexts not loaded\n",
                YAMAHA_SERVICE);
        return 1;
    }
    int rc = start_device_experiment(service, field08);
    IOObjectRelease(service);
    return rc;
}

/* Which layer(s) to run. */
static int g_do_audio = 1;
static int g_do_async = 0;
static int g_do_create = 0;
static int g_do_start = 0;
static uint32_t g_field08 = 0;

/*
 * Find a service and drive the audio driver through it. Kept as its own
 * function so the async layer can be run independently of it.
 */
static int run_audio_layer(void)
{
    printf("before:\n");
    printf("  %-20s matches %d\n", kAudioDeviceClass,
           count_services(kAudioDeviceClass));
    printf("  %-20s matches %d\n", kMidiDeviceClass,
           count_services(kMidiDeviceClass));
    printf("  %-20s matches %d\n", YAMAHA_SERVICE,
           count_services(YAMAHA_SERVICE));

    io_service_t service = find_service(YAMAHA_SERVICE);
    if (service == IO_OBJECT_NULL) {
        fprintf(stderr,
                "\nservice %s not found.\n"
                "The Yamaha kexts are not loaded. Either reboot (if they are\n"
                "installed in /Library/Extensions) or run\n"
                "  sudo tools/try_kextload.sh\n", YAMAHA_SERVICE);
        return 1;
    }
    printf("\nfound service %s (io_service_t 0x%x)\n\n",
           YAMAHA_SERVICE, (unsigned)service);

    int rc = start_plugin(service);

    // The plug-in path gets us a connection but does not build the device, so
    // also drive the kernel interface directly. This is the part that should
    // make the driver enumerate the bus and create the audio device.
    direct_user_client(service);

    printf("\nafter: %s matches %d\n", kAudioDeviceClass,
           count_services(kAudioDeviceClass));

    // Give the driver a moment for the audio device to register with CoreAudio.
    for (int i = 0; i < 5; i++) {
        sleep(1);
        int n = count_services(kAudioDeviceClass);
        printf("  t+%ds  %s=%d\n", i + 1, kAudioDeviceClass, n);
        if (n > 0) {
            break;
        }
    }

    IOObjectRelease(service);
    return rc;
}

/*
 * Find and drive the async transport kext. Split out from run_audio_layer
 * because it is the layer under investigation: a panic here is the result we
 * are after, and we would rather it not happen with the audio path half-done.
 */
static int run_async_layer(void)
{
    io_service_t service = find_service(ASYNC_SERVICE);
    if (service == IO_OBJECT_NULL) {
        fprintf(stderr,
                "\nservice %s not found - the async kext is not loaded.\n",
                ASYNC_SERVICE);
        return 1;
    }
    printf("\nfound service %s (io_service_t 0x%x)\n",
           ASYNC_SERVICE, (unsigned)service);

    int rc = drive_async(service);
    IOObjectRelease(service);
    return rc;
}

int main(int argc, char **argv)
{
    // Default stays what it always was (the audio driver); the async layer is
    // opt-in so an existing habit does not silently start poking the transport.
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--async") == 0) {
            g_do_audio = 0; g_do_async = 1;
        } else if (strcmp(argv[i], "--audio") == 0) {
            g_do_audio = 1; g_do_async = 0;
        } else if (strcmp(argv[i], "--all") == 0) {
            g_do_audio = 1; g_do_async = 1;
        } else if (strcmp(argv[i], "--create-device") == 0) {
            g_do_audio = 0; g_do_async = 0; g_do_create = 1;
        } else if (strncmp(argv[i], "--create-device=", 16) == 0) {
            // Optional: which value to put in the device param's unknown u32.
            g_do_audio = 0; g_do_async = 0; g_do_create = 1;
            g_field08 = (uint32_t)strtoul(argv[i] + 16, NULL, 0);
        } else if (strcmp(argv[i], "--start-device") == 0) {
            g_do_audio = 0; g_do_async = 0; g_do_start = 1;
        } else if (strncmp(argv[i], "--start-device=", 15) == 0) {
            g_do_audio = 0; g_do_async = 0; g_do_start = 1;
            g_field08 = (uint32_t)strtoul(argv[i] + 15, NULL, 0);
        } else if (strncmp(argv[i], "--hold=", 7) == 0) {
            // Keep a started device alive so CoreAudio/ioreg can see it.
            g_hold = (int)strtol(argv[i] + 7, NULL, 0);
        } else {
            fprintf(stderr,
                    "usage: %s [--audio|--async|--all|--create-device[=N]|"
                    "--start-device[=N]]\n"
                    "  --audio          drive the audio driver's user client (default)\n"
                    "  --async          create an async receiver and watch for data\n"
                    "  --all            the async layer first, then the audio layer\n"
                    "  --create-device  call sCreateAudioDevice and look for the\n"
                    "                   YamahaFWAudioDevice it builds\n"
                    "  --start-device   the above, then an isoch stream, an audio\n"
                    "                   stream and sStartAudioDevice\n"
                    "  --hold=N         with --start-device, keep it running N seconds\n"
                    "  (N on --create-device/--start-device sets the device\n"
                    "   parameter's unknown u32 at +0x08)\n",
                    argv[0]);
            return 2;
        }
    }

    printf("yamaha_fw_start - drive the Yamaha n12 driver from user space\n\n");

    int rc = 0;
    // Async first when both are requested: it is the layer whose behaviour is
    // unknown, and it should not run against a driver the audio path has
    // already rearranged.
    if (g_do_async) {
        rc = run_async_layer();
    }
    if (g_do_create) {
        rc = run_create_device_layer(g_field08) || rc;
    }
    if (g_do_start) {
        rc = run_start_device_layer(g_field08) || rc;
    }
    if (g_do_audio) {
        rc = run_audio_layer() || rc;
    }
    return rc;
}
