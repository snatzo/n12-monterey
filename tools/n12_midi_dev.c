/*
 * n12_midi_dev.c - bring the n12's CoreMIDI ports up on a modern Mac.
 *
 * Why this exists (see docs/HOW-IT-WORKS.md):
 *
 *   The desk's MIDI is the one host-writable channel this project has. On a
 *   working Mac the ports "n12 MIDI IN / n12 MIDI OUT / n12 Remote" exist, but
 *   they are created by the connection manager through the *audio driver* -
 *   not by anything in the FireWire register space. Our own agent
 *   (tools/n12_agent) never makes them, so on macOS 12 CoreMIDI shows no
 *   Yamaha ports at all.
 *
 *   Unlike the Enabler/HAL/Control Panel (i386 only), the CoreMIDI driver
 *   `YamahaFWMIDI.plugin` ships an x86_64 slice, so it *can* load here. What is
 *   missing is only the kernel object it attaches to - a `YamahaFWMIDIDevice`
 *   nub, which the audio driver creates when asked.
 *
 * This tool is the small 64-bit caller that asks. It opens the same user client
 * as n12_agent and makes the MIDI half of the vendor sequence:
 *
 *   sCreateMIDIDevice   selector 18, 64B struct -> 4B index
 *   sCreateMIDIPlug     selector 27, 32B struct -> 4B index
 *   sStartMIDIDevice    selector 25, 1 scalar (the device index)
 *
 * The two struct layouts were recovered from the driver's validators
 * (`YamahaFWStreamDevice::createMIDIDeviceFunction` @0x14140 and
 * `createMIDIPlugFunction` @0xed00) - see docs/HOW-IT-WORKS.md.
 *
 * Build:
 *     clang -Wall -O2 -o n12_midi_dev n12_midi_dev.c \
 *           -framework IOKit -framework CoreFoundation -framework CoreMIDI
 *
 * Run (no root needed):
 *     ./n12_midi_dev --run-for=20        # create ports, hold, list CoreMIDI
 *     ./n12_midi_dev --out-plugs=2 --in-plugs=2 --run-for=20
 *     ./n12_midi_dev --with-audio --out-channel=1 --in-channel=16 --run-for=20
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <signal.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <IOKit/IOKitLib.h>

/* The service the audio driver publishes (same one n12_agent opens). */
#define YAMAHA_SERVICE "YamahaFWStreamDevice"

/* Selectors, from the driver's IOExternalMethodDispatch table (dumped with
 * tools/vtable_dump.py; see docs/HOW-IT-WORKS.md). */
#define SEL_OPEN                    0
#define SEL_CLOSE                   1
#define SEL_CREATE_AUDIO_DEVICE     3
#define SEL_DISPOSE_AUDIO_DEVICE    4
#define SEL_START_AUDIO_DEVICE      9
#define SEL_CREATE_ISOCH_PARAM      34
#define SEL_DISPOSE_ISOCH_PARAM     35
#define SEL_SET_ISOCH_CHANNEL       45
#define SEL_REQUEST_START_STREAMS   61
#define SEL_REQUEST_STOP_STREAMS    62
#define SEL_CREATE_MIDI_DEVICE      18
#define SEL_DISPOSE_MIDI_DEVICE     19
#define SEL_GET_NUM_MIDI_DEVICES    20
#define SEL_GET_MIDI_DEVICE         21
#define SEL_START_MIDI_DEVICE       25
#define SEL_STOP_MIDI_DEVICE        26
#define SEL_CREATE_MIDI_PLUG        27
#define SEL_DISPOSE_MIDI_PLUG       28
#define SEL_GET_NUM_MIDI_PLUGS      29
#define SEL_GET_MIDI_PLUG           30
#define SEL_GET_MIDI_PLUG_INFO      31

/* kIOMasterPortDefault is kIOMainPortDefault on macOS 12; same value. */
#if defined(MAC_OS_VERSION_12_0) && MAC_OS_VERSION_12_0
#define YAM_IO_PORT kIOMainPortDefault
#else
#define YAM_IO_PORT kIOMasterPortDefault
#endif

/* The n12's GUID, from its configuration ROM. */
#define N12_GUID 0x00A0DE0000011013ULL

/* ------------------------------------------------------------------ layout */

/*
 * 64-byte input of sCreateMIDIDevice (Yamaha1394StreamIF::CreateMIDIDeviceParam).
 * Recovered from createMIDIDeviceFunction: +0x00 guid; +0x08 field08 and +0x0c
 * field0c are two 32-bit words; then three (size, pointer) name-string pairs.
 * The driver requires all three names non-zero (device, vendor, model), unlike
 * the audio device which needs only two.
 */
typedef struct {
    uint64_t guid;         // +0x00
    uint32_t field08;      // +0x08  unknown; 0 works
    uint32_t field0c;      // +0x0c  unknown; 0 works
    uint64_t name_size;    // +0x10  device name, bytes incl. the NUL
    uint64_t name_ptr;     // +0x18  valid in this process
    uint64_t vendor_size;  // +0x20
    uint64_t vendor_ptr;   // +0x28
    uint64_t model_size;   // +0x30
    uint64_t model_ptr;    // +0x38
} yfw_create_midi_dev_t;
_Static_assert(sizeof(yfw_create_midi_dev_t) == 64, "MIDI device param is 64B");

/*
 * 32-byte input of sCreateMIDIPlug (Yamaha1394StreamIF::CreateMIDIPlugParam).
 * Recovered from createMIDIPlugFunction: +0x00 the MIDI device index; +0x04 a
 * secondary reference (0 allowed); +0x08 and +0x0c are small fields; +0x0d is a
 * direction flag (used by addPlugParamCore / setPlugParam); then one name pair.
 */
typedef struct {
    uint32_t midi_dev;     // +0x00  index returned by sCreateMIDIDevice
    uint32_t stream_ref;   // +0x04  optional stream reference; 0 allowed
    uint32_t field08;      // +0x08  unknown; 0 works
    uint8_t  field0c;      // +0x0c  unknown; 0 works
    uint8_t  is_input;     // +0x0d  direction flag (0 = output, 1 = input)
    uint8_t  pad[2];
    uint64_t name_size;    // +0x10  plug name, bytes incl. the NUL
    uint64_t name_ptr;     // +0x18
} yfw_create_midi_plug_t;
_Static_assert(sizeof(yfw_create_midi_plug_t) == 32, "MIDI plug param is 32B");

/* 48-byte input of sCreateAudioDevice, only used with --with-audio. */
typedef struct {
    uint64_t guid;         // +0x00
    uint32_t field08;      // +0x08
    uint32_t index;        // +0x0c  <= 1, duplicate key
    uint64_t name_size;    // +0x10
    uint64_t name_ptr;     // +0x18
    uint64_t vendor_size;  // +0x20
    uint64_t vendor_ptr;   // +0x28
} yfw_create_audio_dev_t;
_Static_assert(sizeof(yfw_create_audio_dev_t) == 48, "audio device param is 48B");

/* ------------------------------------------------------------------ state */

static struct {
    io_connect_t conn;
    io_service_t service;
    uint32_t audio_dev;                 // set only with --with-audio
    uint32_t iso_out, iso_in;           // set only with --with-audio
    int have_audio, have_iso_out, have_iso_in;
    uint32_t midi_dev;                  // index from sCreateMIDIDevice
    int have_midi_dev;
    uint32_t plugs[64];                 // indices from sCreateMIDIPlug
    int num_plugs;
} g_state;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static io_service_t find_service(const char *className)
{
    CFMutableDictionaryRef matching = IOServiceMatching(className);
    if (matching == NULL) {
        return IO_OBJECT_NULL;
    }
    return IOServiceGetMatchingService(YAM_IO_PORT, matching);
}

/* ------------------------------------------------------------- CoreMIDI view */

/*
 * Print the CoreMIDI sources and destinations that look like the desk. Called
 * once at start (before we create anything) and once after, so the difference is
 * visible in one run. This only reads; it never sends.
 */
static int count_yamaha_ports(const char *which)
{
    int n = 0;
    ItemCount total = (which[0] == 's') ? MIDIGetNumberOfSources()
                                        : MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < total; i++) {
        MIDIEndpointRef ep = (which[0] == 's') ? MIDIGetSource(i) : MIDIGetDestination(i);
        CFStringRef name = NULL;
        // kMIDIPropertyDisplayName is the user-visible port name.
        if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name) == noErr
            && name != NULL) {
            char buf[256] = {0};
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);
            // The desk's ports are named "n12 ..." or contain "mLAN".
            if (strcasestr(buf, "n12") || strcasestr(buf, "mLAN")
                || strcasestr(buf, "yamaha")) {
                printf("    [%s] %s\n", which, buf);
                n++;
            }
        }
    }
    return n;
}

static void show_coremidi(const char *when)
{
    printf("\nCoreMIDI Yamaha/mLAN ports %s:\n", when);
    int s = count_yamaha_ports("sources");
    int d = count_yamaha_ports("destinations");
    if (s == 0 && d == 0) {
        printf("    (none)\n");
    }
}

/* ------------------------------------------------------------------- steps */

static int step_create_audio_device(io_connect_t conn)
{
    static char dev_name[]    = "n12";
    static char vendor_name[] = "Yamaha";

    yfw_create_audio_dev_t p;
    memset(&p, 0, sizeof(p));
    p.guid        = N12_GUID;
    p.index       = 0;
    p.name_size   = sizeof(dev_name);
    p.name_ptr    = (uint64_t)(uintptr_t)dev_name;
    p.vendor_size = sizeof(vendor_name);
    p.vendor_ptr  = (uint64_t)(uintptr_t)vendor_name;

    uint32_t index = 0xffffffff;
    size_t out = sizeof(index);
    kern_return_t kr = IOConnectCallStructMethod(conn, SEL_CREATE_AUDIO_DEVICE,
                                                 &p, sizeof(p), &index, &out);
    printf("  sCreateAudioDevice                 -> 0x%08x", kr);
    if (kr == KERN_SUCCESS) printf("  index=%u", index);
    printf("\n");
    return kr == KERN_SUCCESS ? (int)index : -1;
}

static int step_create_isoch(io_connect_t conn, uint32_t channel, int is_input)
{
    uint64_t in[4] = { channel, (uint64_t)(is_input ? 1 : 0), 16, 1 };
    uint64_t index = 0;
    uint32_t cnt = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_CREATE_ISOCH_PARAM,
                                                 in, 4, &index, &cnt);
    printf("  sCreateIsochStreamParam(ch=%u %s)  -> 0x%08x\n",
           channel, is_input ? "in " : "out", kr);
    return kr == KERN_SUCCESS ? (int)index : -1;
}

static void step_set_channel(io_connect_t conn, int isoch_index, uint32_t channel)
{
    uint64_t a[2] = { (uint64_t)isoch_index, channel };
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_SET_ISOCH_CHANNEL,
                                                 a, 2, NULL, NULL);
    printf("  sSetIsochStreamChannel(%d,ch=%u)   -> 0x%08x\n",
           isoch_index, channel, kr);
}

/*
 * Create a MIDI device. All three name strings are required by the driver, so a
 * missing one returns -0x1ffffd3e (0xe00002c2). Returns the index, or -1.
 */
static int step_create_midi_device(io_connect_t conn)
{
    static char dev_name[]    = "n12";
    static char vendor_name[] = "Yamaha";
    static char model_name[]  = "n12";

    yfw_create_midi_dev_t p;
    memset(&p, 0, sizeof(p));
    p.guid        = N12_GUID;
    p.field08     = 0;
    p.field0c     = 0;
    p.name_size   = sizeof(dev_name);
    p.name_ptr    = (uint64_t)(uintptr_t)dev_name;
    p.vendor_size = sizeof(vendor_name);
    p.vendor_ptr  = (uint64_t)(uintptr_t)vendor_name;
    p.model_size  = sizeof(model_name);
    p.model_ptr   = (uint64_t)(uintptr_t)model_name;

    uint32_t index = 0xffffffff;
    size_t out = sizeof(index);
    kern_return_t kr = IOConnectCallStructMethod(conn, SEL_CREATE_MIDI_DEVICE,
                                                 &p, sizeof(p), &index, &out);
    printf("  sCreateMIDIDevice                  -> 0x%08x", kr);
    if (kr == KERN_SUCCESS) printf("  index=%u", index);
    printf("\n");
    return kr == KERN_SUCCESS ? (int)index : -1;
}

/*
 * Create one MIDI plug (a single-direction port). `midi_dev` is the index from
 * sCreateMIDIDevice; `is_input` selects the direction. Returns the index or -1.
 */
static int step_create_midi_plug(io_connect_t conn, uint32_t midi_dev,
                                 int is_input, const char *name)
{
    yfw_create_midi_plug_t p;
    memset(&p, 0, sizeof(p));
    p.midi_dev   = midi_dev;
    p.stream_ref = 0;
    p.field08    = 0;
    p.field0c    = 0;
    p.is_input   = (uint8_t)(is_input ? 1 : 0);
    p.name_size  = strlen(name) + 1;
    p.name_ptr   = (uint64_t)(uintptr_t)name;

    uint32_t index = 0xffffffff;
    size_t out = sizeof(index);
    kern_return_t kr = IOConnectCallStructMethod(conn, SEL_CREATE_MIDI_PLUG,
                                                 &p, sizeof(p), &index, &out);
    printf("  sCreateMIDIPlug(%s, \"%s\")      -> 0x%08x",
           is_input ? "in " : "out", name, kr);
    if (kr == KERN_SUCCESS) printf("  index=%u", index);
    printf("\n");
    return kr == KERN_SUCCESS ? (int)index : -1;
}

/* Read back what the driver thinks it has, to confirm the creates landed. */
static void report_midi_state(io_connect_t conn)
{
    uint64_t v = 0;
    uint32_t n = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_GET_NUM_MIDI_DEVICES,
                                                 NULL, 0, &v, &n);
    printf("  sGetNumMIDIDevices                 -> 0x%08x  %llu\n",
           kr, (unsigned long long)v);

    for (uint32_t i = 0; i < 2; i++) {
        uint64_t arg = i;
        uint8_t buf[24] = {0};
        size_t out = sizeof(buf);
        // sGetMIDIDevice: 1 scalar in, 24B struct out - the first 8 bytes are the
        // GUID, as with sGetAudioDevice.
        kr = IOConnectCallMethod(conn, SEL_GET_MIDI_DEVICE, &arg, 1,
                                 NULL, 0, NULL, NULL, buf, &out);
        if (kr != KERN_SUCCESS) break;
        uint64_t guid = *(uint64_t *)buf;
        printf("  sGetMIDIDevice(%u)                 -> 0x%08x  guid=0x%016llx\n",
               i, kr, (unsigned long long)guid);
    }

    // sGetNumMIDIPlugs: 2 scalars in (device, isInput), 1 scalar out.
    for (int dir = 0; dir < 2; dir++) {
        uint64_t in[2] = { g_state.midi_dev, (uint64_t)dir };
        v = 0; n = 1;
        kr = IOConnectCallScalarMethod(conn, SEL_GET_NUM_MIDI_PLUGS, in, 2, &v, &n);
        printf("  sGetNumMIDIPlugs(%s)             -> 0x%08x  %llu\n",
               dir ? "in " : "out", kr, (unsigned long long)v);
    }
}

/*
 * Undo the creates. Plugs first, then the device, then (if any) the audio side.
 * Kept deliberately simple - MIDI objects have no cross-references the way the
 * audio streams reference the isoch parameters.
 */
static void teardown(void)
{
    io_connect_t conn = g_state.conn;
    if (conn == IO_OBJECT_NULL) return;

    for (int i = g_state.num_plugs - 1; i >= 0; i--) {
        uint64_t p = g_state.plugs[i];
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_MIDI_PLUG, &p, 1, NULL, NULL);
    }
    if (g_state.have_midi_dev) {
        uint64_t d = g_state.midi_dev;
        IOConnectCallScalarMethod(conn, SEL_STOP_MIDI_DEVICE, &d, 1, NULL, NULL);
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_MIDI_DEVICE, &d, 1, NULL, NULL);
    }
    if (g_state.have_audio) {
        IOConnectCallScalarMethod(conn, SEL_REQUEST_STOP_STREAMS, NULL, 0, NULL, NULL);
    }
    if (g_state.have_iso_out) {
        uint64_t s = g_state.iso_out;
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_ISOCH_PARAM, &s, 1, NULL, NULL);
    }
    if (g_state.have_iso_in) {
        uint64_t s = g_state.iso_in;
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_ISOCH_PARAM, &s, 1, NULL, NULL);
    }
    if (g_state.have_audio) {
        uint64_t d = g_state.audio_dev;
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_AUDIO_DEVICE, &d, 1, NULL, NULL);
    }
    IOConnectCallScalarMethod(conn, SEL_CLOSE, NULL, 0, NULL, NULL);
    IOServiceClose(conn);
    g_state.conn = IO_OBJECT_NULL;
}

int main(int argc, char **argv)
{
    int out_plugs = 1, in_plugs = 1;
    int run_for = 0;
    int with_audio = 0;
    uint32_t out_channel = 1, in_channel = 16;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--out-plugs=", 12) == 0) {
            out_plugs = atoi(argv[i] + 12);
        } else if (strncmp(argv[i], "--in-plugs=", 11) == 0) {
            in_plugs = atoi(argv[i] + 11);
        } else if (strncmp(argv[i], "--run-for=", 10) == 0) {
            run_for = atoi(argv[i] + 10);
        } else if (strcmp(argv[i], "--with-audio") == 0) {
            with_audio = 1;
        } else if (strncmp(argv[i], "--out-channel=", 14) == 0) {
            out_channel = (uint32_t)strtoul(argv[i] + 14, NULL, 0);
        } else if (strncmp(argv[i], "--in-channel=", 13) == 0) {
            in_channel = (uint32_t)strtoul(argv[i] + 13, NULL, 0);
        } else {
            fprintf(stderr,
                    "usage: %s [--out-plugs=N] [--in-plugs=N] [--with-audio]\n"
                    "          [--out-channel=N] [--in-channel=N] [--run-for=N]\n",
                    argv[0]);
            return 2;
        }
    }

    printf("n12_midi_dev - create the n12's CoreMIDI ports through the driver\n\n");
    setvbuf(stdout, NULL, _IONBF, 0);

    show_coremidi("before");

    g_state.service = find_service(YAMAHA_SERVICE);
    if (g_state.service == IO_OBJECT_NULL) {
        fprintf(stderr, "service %s not found - load the kexts first "
                        "(sudo tools/try_kextload.sh)\n", YAMAHA_SERVICE);
        return 1;
    }
    kern_return_t kr = IOServiceOpen(g_state.service, mach_task_self(), 0,
                                     &g_state.conn);
    if (kr != KERN_SUCCESS || g_state.conn == IO_OBJECT_NULL) {
        fprintf(stderr, "IOServiceOpen failed: 0x%08x\n", kr);
        IOObjectRelease(g_state.service);
        return 1;
    }
    kr = IOConnectCallScalarMethod(g_state.conn, SEL_OPEN, NULL, 0, NULL, NULL);
    printf("  sOpen                              -> 0x%08x\n", kr);
    if (kr != KERN_SUCCESS) {
        teardown();
        IOObjectRelease(g_state.service);
        return 1;
    }

    // Optional: the audio device and isochronous streams. The MIDI controller
    // does not obviously need them (the MIDI device is added to its own queue),
    // but the desk's MIDI rides an isochronous stream, so --with-audio also
    // brings up the audio side and starts the DMA.
    if (with_audio) {
        int dev = step_create_audio_device(g_state.conn);
        if (dev < 0) {
            fprintf(stderr, "  could not create the audio device\n");
            teardown(); IOObjectRelease(g_state.service);
            return 1;
        }
        g_state.audio_dev = (uint32_t)dev;
        g_state.have_audio = 1;

        int o = step_create_isoch(g_state.conn, out_channel, 0);
        if (o >= 0) {
            g_state.iso_out = (uint32_t)o; g_state.have_iso_out = 1;
            step_set_channel(g_state.conn, o, out_channel);
        }
        int in = step_create_isoch(g_state.conn, in_channel, 1);
        if (in >= 0) {
            g_state.iso_in = (uint32_t)in; g_state.have_iso_in = 1;
            step_set_channel(g_state.conn, in, in_channel);
        }
    }

    // 1. the MIDI device.
    int md = step_create_midi_device(g_state.conn);
    if (md < 0) {
        fprintf(stderr, "  sCreateMIDIDevice failed - see the error code above\n");
        teardown(); IOObjectRelease(g_state.service);
        return 1;
    }
    g_state.midi_dev = (uint32_t)md;
    g_state.have_midi_dev = 1;

    // 2. the plugs. Default one output and one input, named like the desk's.
    for (int i = 0; i < out_plugs && g_state.num_plugs < 64; i++) {
        char name[64];
        snprintf(name, sizeof(name), "mLAN Network %d", i + 1);
        int p = step_create_midi_plug(g_state.conn, g_state.midi_dev, 0, name);
        if (p >= 0) g_state.plugs[g_state.num_plugs++] = (uint32_t)p;
    }
    for (int i = 0; i < in_plugs && g_state.num_plugs < 64; i++) {
        char name[64];
        snprintf(name, sizeof(name), "mLAN Network %d", i + 1);
        int p = step_create_midi_plug(g_state.conn, g_state.midi_dev, 1, name);
        if (p >= 0) g_state.plugs[g_state.num_plugs++] = (uint32_t)p;
    }

    // 3. start it, so the MIDI plugin's IOKit notifications fire.
    uint64_t d = g_state.midi_dev;
    kr = IOConnectCallScalarMethod(g_state.conn, SEL_START_MIDI_DEVICE, &d, 1,
                                   NULL, NULL);
    printf("  sStartMIDIDevice(%u)               -> 0x%08x\n", g_state.midi_dev, kr);

    // With --with-audio, start the isoch DMA and the audio engine too.
    if (with_audio) {
        IOConnectCallScalarMethod(g_state.conn, SEL_REQUEST_START_STREAMS,
                                  NULL, 0, NULL, NULL);
        uint64_t ad = g_state.audio_dev;
        IOConnectCallScalarMethod(g_state.conn, SEL_START_AUDIO_DEVICE, &ad, 1,
                                  NULL, NULL);
    }

    printf("\ndriver's MIDI view:\n");
    report_midi_state(g_state.conn);

    // The CoreMIDI ports are published by the plugin asynchronously, so give it
    // a moment before looking.
    sleep(2);
    show_coremidi("after");

    if (run_for > 0) {
        printf("\nholding the user client open for %d s\n\n", run_for);
        for (int t = 0; t < run_for && !g_stop; t++) sleep(1);
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("stopping\n");
    teardown();
    IOObjectRelease(g_state.service);
    return 0;
}
