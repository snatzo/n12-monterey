/*
 * n12_agent.c - keep the Yamaha n12 alive as a CoreAudio device.
 *
 * Why this exists: the driver builds the whole audio device on request, but the
 * request lives on an open user client. When that client goes away the driver
 * treats it as a client death and tears the device down, so CoreAudio only sees
 * the n12 while some process is holding the connection open. The shipped
 * component that used to do this (YamahaFWCM.app) is 32-bit and cannot run.
 *
 * This is the smallest 64-bit replacement: find the audio driver, run the four
 * calls that make the device, then sit in the background until told to stop.
 *
 * The four calls, and what each parameter means, are in docs/HOW-IT-WORKS.md
 * sections 16-18. In short:
 *
 *   sCreateAudioDevice      48B: guid + two NUL-terminated UTF-8 name buffers
 *   sCreateIsochStreamParam 4 scalars: (channel, isInput, numAudioSequences,
 *                            numMIDI). The bool chooses CreateAudioIn vs
 *                            CreateAudioOut inside createIsochStreaming.
 *   sCreateAudioStream      40B: references the audio device and an isoch
 *                            stream. +0x0c is the stream's channel count and
 *                            must be non-zero whenever +0x04 (the isoch
 *                            reference) is; +0x10 is a direction flag.
 *   sStartAudioDevice       1 scalar: the audio device index
 *
 * Build:
 *     clang -Wall -O2 -o n12_agent n12_agent.c \
 *           -framework IOKit -framework CoreFoundation
 *
 * Run (no root needed):
 *     ./n12_agent                 # hold the device until killed
 *     ./n12_agent --out-channel N # override the guessed isoch channel
 *
 * Then CoreAudio should list an "n12" device with 16 outputs while this runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <signal.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

/* The service the audio driver publishes. */
#define YAMAHA_SERVICE "YamahaFWStreamDevice"

/* Selectors, from the driver's IOExternalMethodDispatch table. */
#define SEL_OPEN                   0
#define SEL_CLOSE                  1
#define SEL_CREATE_AUDIO_DEVICE    3
#define SEL_DISPOSE_AUDIO_DEVICE   4
#define SEL_GET_NUM_AUDIO_DEV      5
#define SEL_START_AUDIO_DEVICE     9
#define SEL_STOP_AUDIO_DEVICE      10
#define SEL_CREATE_AUDIO_STREAM    11
#define SEL_DISPOSE_AUDIO_STREAM   12
#define SEL_GET_NUM_AUDIO_STREAMS  13
#define SEL_CREATE_ISOCH_PARAM     34
#define SEL_DISPOSE_ISOCH_PARAM    35
#define SEL_SET_ISOCH_CHANNEL      45   // sSetIsochStreamChannel(isoIndex, channel)
#define SEL_SET_ISOCH_RATE         47
#define SEL_SET_SYT_SYNC           43   // sSetSYTSynchChannel(channel)
#define SEL_REQUEST_START_STREAMS  61
#define SEL_REQUEST_STOP_STREAMS   62
#define SEL_SET_AVAILABLE_RATES    76   // sSetAvailableSampleRates(count, rates[])
#define SEL_REQUEST_CHANGE_RATE    86

/* Set by --cm-style: build the device the way the vendor connection manager
 * does, with one audio stream per channel. See step_create_stream_cm. */
static int g_cm_style = 0;

/* --out-offset=N puts the value N in the stream structure's +0x08 field (the
 * one the manager varies per channel and we always send as 0). If that field is
 * a starting-channel offset, a small stream can be made to present exactly the
 * desk inputs that CoreAudio will use for stereo.
 * --out-chans=N overrides the output stream's channel count (default 16). */
static int g_out_off = 0;
static int g_out_chans = 0;

/* Upper bound on how many per-channel streams --out-map may build. The n12 has
 * at most 16 channels in a direction, so 16 is generous; the extra headroom just
 * lets an experiment list a few more without a bounds check firing. */
#define N12_MAX_STREAMS 32

/* --out-map=6,7,8,...  builds ONE OUTPUT AUDIO STREAM PER LISTED CHANNEL.
 *
 * The numbers are 0-based isochronous subchannels, i.e. the value that goes in
 * the stream structure's +0x08 field. The driver calls that the stream's
 * "sequence number"; the desk calls the matching input "DAW IN n+1". For the
 * n12's own map, desk channel k (1-based) is fed from isoch subchannel 5+k, so
 * desk 1..8 need --out-map=6,7,8,9,10,11,12,13.
 *
 * Each stream is given channel number 1,2,3... in list order, so CoreAudio's
 * output 1 is the first listed value. See docs/HOW-IT-WORKS.md. */
static int g_out_map[N12_MAX_STREAMS];
static int g_out_map_n = 0;

/* --iso-per-stream: give every per-channel output stream its own isochronous
 * parameter, each created at that channel, instead of sharing the one output
 * isoch parameter. The vendor used a single shared parameter, so this is off by
 * default; it exists to test the alternative if the shared form is refused. */
static int g_iso_per_stream = 0;

/* The channel the per-channel output isochronous parameter(s) are programmed on
 * with sSetIsochStreamChannel. -1 means "same as --out-channel". */
static int g_iso_channel = -1;

/* --in-map=0,1,2,... builds ONE INPUT AUDIO STREAM PER LISTED CHANNEL, the
 * mirror of --out-map for the desk -> Mac direction. The numbers are 0-based
 * isochronous subchannels (the desk's DAW OUT n is subchannel n-1), and device
 * input k is given channel number k. The desk's stereo bus is DAW OUT 15/16, so
 * --in-map=0,...,15 puts it on Mac inputs 15 and 16. */
static int g_in_map[N12_MAX_STREAMS];
static int g_in_map_n = 0;

/* kIOMasterPortDefault is kIOMainPortDefault on macOS 12; same value. */
#if defined(MAC_OS_VERSION_12_0) && MAC_OS_VERSION_12_0
#define YAM_IO_PORT kIOMainPortDefault
#else
#define YAM_IO_PORT kIOMasterPortDefault
#endif

/* The n12's GUID, from its configuration ROM. */
#define N12_GUID 0x00A0DE0000011013ULL

/* How many audio channels the n12 moves in each direction. */
#define N12_CHANNELS 16

/* ------------------------------------------------------------------ layout */

/* 48-byte input of sCreateAudioDevice (Yamaha1394StreamIF::CreateAudioDeviceParam). */
typedef struct {
    uint64_t guid;         // +0x00
    uint32_t field08;      // +0x08  unknown; 0 works
    uint32_t index;        // +0x0c  <= 1, duplicate key
    uint64_t name_size;    // +0x10  bytes, including the NUL
    uint64_t name_ptr;     // +0x18  valid in this process
    uint64_t vendor_size;  // +0x20
    uint64_t vendor_ptr;   // +0x28
} yfw_create_audio_dev_t;
_Static_assert(sizeof(yfw_create_audio_dev_t) == 48, "device param is 48B");

/* 40-byte input of sCreateAudioStream (Yamaha1394StreamIF::CreateAudioStreamParam). */
typedef struct {
    uint32_t audio_dev;    // +0x00  index returned by sCreateAudioDevice
    uint32_t isoch_stream; // +0x04  index returned by sCreateIsochStreamParam
    uint32_t field08;      // +0x08  unknown; 0 works
    uint32_t channels;     // +0x0c  channel count; must be non-zero with +0x04
    uint8_t  is_input;     // +0x10  direction flag (0 = output)
    uint8_t  pad[7];
    uint64_t name_size;    // +0x18
    uint64_t name_ptr;     // +0x20
} yfw_create_audio_stream_t;
_Static_assert(sizeof(yfw_create_audio_stream_t) == 40, "stream param is 40B");

/* ------------------------------------------------------------------ state */

/*
 * Everything the clean-up path needs, gathered in one place so the signal
 * handler and the normal exit can share it. A signal handler may only touch
 * async-signal-safe things, so the actual unwind happens in the main loop.
 */
static struct {
    io_connect_t conn;
    io_service_t service;
    uint32_t dev_index;
    uint32_t out_isoch, in_isoch;
    uint32_t out_stream, in_stream;
    int have_dev, have_out_isoch, have_in_isoch;
    int have_out_stream, have_in_stream;
    /* The per-channel build (--out-map): every output channel is its own audio
     * stream. map_streams holds each stream's index; map_isochs holds the extra
     * isochronous parameters created for --iso-per-stream (empty otherwise). */
    uint32_t map_streams[N12_MAX_STREAMS];
    int      map_stream_n;
    uint32_t map_isochs[N12_MAX_STREAMS];
    int      map_isoch_n;
    /* The per-channel input build (--in-map): mirror of the output arrays. */
    uint32_t in_map_streams[N12_MAX_STREAMS];
    int      in_map_stream_n;
} g_state;   /* zero-initialised below; IO_OBJECT_NULL is 0 */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;   // the main loop notices and unwinds
}

static io_service_t find_service(const char *className)
{
    CFMutableDictionaryRef matching = IOServiceMatching(className);
    if (matching == NULL) {
        return IO_OBJECT_NULL;
    }
    return IOServiceGetMatchingService(YAM_IO_PORT, matching);
}

/* ------------------------------------------------------------------- steps */

/*
 * Ask the driver for the audio device. Returns the index to quote later, or -1.
 */
static int step_create_device(io_connect_t conn)
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
    if (kr == KERN_SUCCESS) {
        printf("  index=%u", index);
    }
    printf("\n");
    return kr == KERN_SUCCESS ? (int)index : -1;
}

/*
 * Create one isochronous stream. `is_input` selects the direction; the driver
 * calls CStreamDevice::isochStreamingCreateAudioIn or ...CreateAudioOut
 * depending on it. Returns the index, or -1.
 */
static int step_create_isoch(io_connect_t conn, uint32_t channel, int is_input,
                             uint32_t sequences, uint32_t midi)
{
    uint64_t in[4];
    in[0] = channel;
    in[1] = (uint64_t)(is_input ? 1 : 0);
    in[2] = sequences;
    in[3] = midi;

    uint64_t index = 0;
    uint32_t cnt = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_CREATE_ISOCH_PARAM,
                                                 in, 4, &index, &cnt);
    printf("  sCreateIsochStreamParam(ch=%u %s seq=%u) -> 0x%08x",
           channel, is_input ? "in " : "out", sequences, kr);
    if (kr == KERN_SUCCESS) {
        printf("  index=%llu", (unsigned long long)index);
    }
    printf("\n");
    return kr == KERN_SUCCESS ? (int)index : -1;
}

/*
 * Set an existing isochronous stream's channel (selector 45,
 * sSetIsochStreamChannel). The channel also arrives as the first argument of
 * sCreateIsochStreamParam, but that only seeds the parameter; this is the call
 * that tells the driver's isoch client to program the stream on the channel,
 * and it is the one the vendor connection manager makes. Two scalars:
 * (isoch index, channel).
 */
static void step_set_channel(io_connect_t conn, int isoch_index, uint32_t channel)
{
    uint64_t a[2] = { (uint64_t)isoch_index, channel };
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_SET_ISOCH_CHANNEL,
                                                 a, 2, NULL, NULL);
    printf("  sSetIsochStreamChannel(%d,ch=%u)   -> 0x%08x\n",
           isoch_index, channel, kr);
}

/*
 * Create one audio stream referencing an isoch stream. `is_input` becomes the
 * parameter's direction flag, and `channels` the value the driver requires to
 * be non-zero whenever an isoch reference is present.
 */
static int step_create_stream(io_connect_t conn, uint32_t dev_index,
                              uint32_t isoch_index, uint32_t channels,
                              int is_input, const char *name)
{
    yfw_create_audio_stream_t p;
    memset(&p, 0, sizeof(p));
    p.audio_dev    = dev_index;
    p.isoch_stream = isoch_index;
    // The manager varies this field per channel; we used to always send 0.
    // --out-offset exposes it so we can test whether it is a channel offset.
    p.field08      = is_input ? 0 : (uint32_t)g_out_off;
    p.channels     = channels;
    p.is_input     = (uint8_t)(is_input ? 1 : 0);
    // The name must be non-empty and NUL-terminated; the driver copies
    // name_size bytes and calls OSString::withCString on them.
    p.name_size    = strlen(name) + 1;
    p.name_ptr     = (uint64_t)(uintptr_t)name;

    uint32_t index = 0xffffffff;
    size_t out = sizeof(index);
    kern_return_t kr = IOConnectCallStructMethod(conn, SEL_CREATE_AUDIO_STREAM,
                                                 &p, sizeof(p), &index, &out);
    printf("  sCreateAudioStream(%s chans=%u)      -> 0x%08x",
           is_input ? "in " : "out", channels, kr);
    if (kr == KERN_SUCCESS) {
        printf("  index=%u", index);
    }
    printf("\n");
    return kr == KERN_SUCCESS ? (int)index : -1;
}

/*
 * Vendor-style stream creation: ONE STREAM PER CHANNEL.
 *
 * Why this exists: the Yamaha connection manager on Mojave builds the device
 * completely differently from the code above. The user-client capture (see
 * docs/HOW-IT-WORKS.md) shows it calling sCreateAudioStream 16 times for the input isoch
 * stream and 16 times for the output one, with the *channel index* in the
 * structure rather than a channel count. Since the same driver is byte-identical
 * on both systems, and Mojave plays stereo correctly, this is the prime suspect
 * for our channel-mapping problem.
 *
 * The captured structure (40 bytes) is:
 *
 *   +0x00  audio_dev            the device index
 *   +0x04  isoch_stream         the isoch index
 *   +0x08  field08 = CHANNEL    the channel index (0..15) - we used to send 0
 *   +0x0c  channels = 1 / 0     one channel (input set) / zero (output set)
 *   +0x10  is_input = 1 / 0
 *   +0x14  0xffffffff
 *   +0x18  name_size
 *   +0x20  name_ptr
 *
 * @param conn        the open user client
 * @param dev_index   index from sCreateAudioDevice
 * @param isoch_index index from sCreateIsochStreamParam
 * @param channel     which channel of that isoch stream this stream carries
 * @param is_input    non-zero for the input direction
 * @param name        stream name (NUL-terminated, in this process)
 * @return            the stream index, or -1
 */
static int step_create_stream_cm(io_connect_t conn, uint32_t dev_index,
                                 uint32_t isoch_index, uint32_t channel,
                                 int is_input, const char *name)
{
    yfw_create_audio_stream_t p;
    memset(&p, 0, sizeof(p));
    p.audio_dev    = dev_index;
    p.isoch_stream = isoch_index;
    p.field08      = channel;                 // the channel index, not 0
    p.channels     = is_input ? 1 : 0;        // exactly as captured
    p.is_input     = (uint8_t)(is_input ? 1 : 0);
    // The capture shows 0xffffffff at +0x14, which falls in the padding after
    // is_input (+0x10); pad[3..6] are bytes +0x14..+0x17.
    p.pad[3] = 0xff; p.pad[4] = 0xff; p.pad[5] = 0xff; p.pad[6] = 0xff;
    p.name_size    = strlen(name) + 1;
    p.name_ptr     = (uint64_t)(uintptr_t)name;

    uint32_t index = 0xffffffff;
    size_t out = sizeof(index);
    kern_return_t kr = IOConnectCallStructMethod(conn, SEL_CREATE_AUDIO_STREAM,
                                                 &p, sizeof(p), &index, &out);
    printf("  sCreateAudioStream(cm %s ch=%2u)  -> 0x%08x\n",
           is_input ? "in " : "out", channel, kr);
    return kr == KERN_SUCCESS ? (int)index : -1;
}

/*
 * Create one *single-channel* audio stream with an explicit sequence number and
 * channel number. This is the per-desk-channel builder behind --out-map.
 *
 * The driver stores the first value as CAudioStreamParam::SequenceNumber and the
 * second as CAudioStreamParam::ChannelNumber; the latter is also the 1-based
 * index (and, via its maximum, the count) of the device's channels - see
 * docs/HOW-IT-WORKS.md. Keeping a single channel per stream is what lets the desk
 * see each channel separately: a stream's own isochronous placement comes from
 * its sequence number, so device channel N can be pointed at any subchannel.
 *
 * @param conn           the open user client
 * @param dev_index      index from sCreateAudioDevice
 * @param isoch_index    the isochronous parameter the stream transmits on
 * @param channel_number 1-based device channel this stream is (struct +0x0c)
 * @param sequence       the isochronous subchannel it feeds (struct +0x08)
 * @param is_input       non-zero for the input direction
 * @param name           stream name, NUL-terminated and living in this process
 * @return               the stream index, or -1 if the driver refused it
 */
static int step_create_stream_chan(io_connect_t conn, uint32_t dev_index,
                                   uint32_t isoch_index, uint32_t channel_number,
                                   uint32_t sequence, int is_input,
                                   const char *name)
{
    yfw_create_audio_stream_t p;
    memset(&p, 0, sizeof(p));
    p.audio_dev    = dev_index;
    p.isoch_stream = isoch_index;
    p.field08      = sequence;        // +0x08  CAudioStreamParam::SequenceNumber
    p.channels     = channel_number;  // +0x0c  CAudioStreamParam::ChannelNumber
    p.is_input     = (uint8_t)(is_input ? 1 : 0);
    p.name_size    = strlen(name) + 1;
    p.name_ptr     = (uint64_t)(uintptr_t)name;

    uint32_t index = 0xffffffff;
    size_t out = sizeof(index);
    kern_return_t kr = IOConnectCallStructMethod(conn, SEL_CREATE_AUDIO_STREAM,
                                                 &p, sizeof(p), &index, &out);
    printf("  sCreateAudioStream(%s ch#%u seq=%u)  -> 0x%08x",
           is_input ? "in " : "out", channel_number, sequence, kr);
    if (kr == KERN_SUCCESS) {
        printf("  index=%u", index);
    }
    printf("\n");
    return kr == KERN_SUCCESS ? (int)index : -1;
}

/*
 * Undo everything, in the order the driver expects.
 *
 * This ordering matters and getting it wrong crashes the machine. The audio
 * streams hold references to the isochronous stream parameters, so the streams
 * must be disposed before the isoch parameters, and both before the audio
 * device. Disposing the isoch parameter first leaves the audio streams pointing
 * at freed memory, and the driver's isoch worker then dereferences a stale
 * field - which is what a panic on a past run showed: a fault at the value
 * 0xAC44 (44100), the sample rate the driver stores in CIsochStreamParam+0x20.
 *
 * Safe to call more than once and after a partial setup.
 */
static void teardown(void)
{
    io_connect_t conn = g_state.conn;
    if (conn == IO_OBJECT_NULL) {
        return;
    }

    // 1. tell the isochronous engine to stop, then let its worker drain.
    IOConnectCallScalarMethod(conn, SEL_REQUEST_STOP_STREAMS, NULL, 0, NULL, NULL);
    if (g_state.have_dev) {
        uint64_t d = g_state.dev_index;
        IOConnectCallScalarMethod(conn, SEL_STOP_AUDIO_DEVICE, &d, 1, NULL, NULL);
    }
    usleep(100 * 1000);

    // 2. dispose the audio streams, which reference the isoch parameters.
    //    The per-channel streams (--out-map) first, then the legacy pair.
    for (int i = 0; i < g_state.map_stream_n; i++) {
        uint64_t s = g_state.map_streams[i];
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_AUDIO_STREAM, &s, 1, NULL, NULL);
    }
    g_state.map_stream_n = 0;
    for (int i = 0; i < g_state.in_map_stream_n; i++) {
        uint64_t s = g_state.in_map_streams[i];
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_AUDIO_STREAM, &s, 1, NULL, NULL);
    }
    g_state.in_map_stream_n = 0;
    if (g_state.have_out_stream) {
        uint64_t s = g_state.out_stream;
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_AUDIO_STREAM, &s, 1, NULL, NULL);
    }
    if (g_state.have_in_stream) {
        uint64_t s = g_state.in_stream;
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_AUDIO_STREAM, &s, 1, NULL, NULL);
    }

    // 3. only now the isochronous stream parameters: the extras created for
    //    --iso-per-stream, then the shared output one, then the input one.
    for (int i = 0; i < g_state.map_isoch_n; i++) {
        uint64_t iso = g_state.map_isochs[i];
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_ISOCH_PARAM, &iso, 1, NULL, NULL);
    }
    g_state.map_isoch_n = 0;
    if (g_state.have_in_isoch) {
        uint64_t iso = g_state.in_isoch;
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_ISOCH_PARAM, &iso, 1, NULL, NULL);
    }
    if (g_state.have_out_isoch) {
        uint64_t iso = g_state.out_isoch;
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_ISOCH_PARAM, &iso, 1, NULL, NULL);
    }

    // 4. and finally the audio device.
    if (g_state.have_dev) {
        uint64_t d = g_state.dev_index;
        IOConnectCallScalarMethod(conn, SEL_DISPOSE_AUDIO_DEVICE, &d, 1, NULL, NULL);
    }
    IOConnectCallScalarMethod(conn, SEL_CLOSE, NULL, 0, NULL, NULL);
    IOServiceClose(conn);
    g_state.conn = IO_OBJECT_NULL;
}

/*
 * Ask the driver what it thinks the streams are. This is the cheapest way to
 * see whether the values we passed are the values in force, before trying to
 * make audio actually move.
 */
static void report_driver_state(io_connect_t conn, uint32_t dev_index,
                                uint32_t out_iso, uint32_t in_iso)
{
    struct { uint32_t sel; const char *name; } no_arg[] = {
        { 42, "sGetSYTSynchChannel" },   // 0 in -> 1 out
        { 44, "sGetMaxSpeed" },          // 0 in -> 1 out
        { 77, "sGetSampleRate" },        // 0 in -> 1 out
    };
    for (size_t i = 0; i < sizeof(no_arg) / sizeof(no_arg[0]); i++) {
        uint64_t v = 0;
        uint32_t n = 1;
        kern_return_t kr = IOConnectCallScalarMethod(conn, no_arg[i].sel,
                                                     NULL, 0, &v, &n);
        printf("  %-26s -> 0x%08x  %llu\n", no_arg[i].name, kr,
               (unsigned long long)v);
    }

    // These take an isoch stream index and report the channel / sample rate the
    // driver is actually using for it.
    for (int i = 0; i < 2; i++) {
        uint32_t idx = i ? in_iso : out_iso;
        uint64_t arg = idx;

        uint64_t v = 0;
        uint32_t n = 1;
        kern_return_t kr = IOConnectCallScalarMethod(conn, 46, &arg, 1, &v, &n);
        printf("  sGetIsochStreamChannel(%u,%s) -> 0x%08x  %llu\n",
               idx, i ? "in " : "out", kr, (unsigned long long)v);

        v = 0;
        n = 1;
        kr = IOConnectCallScalarMethod(conn, 48, &arg, 1, &v, &n);
        printf("  sGetIsochStreamSampleRate(%u,%s) -> 0x%08x  %llu\n",
               idx, i ? "in " : "out", kr, (unsigned long long)v);

        // sGetIsochStreamInfo(1 in -> 3 out)
        uint64_t three[3] = { 0, 0, 0 };
        uint32_t tn = 3;
        kr = IOConnectCallScalarMethod(conn, 38, &arg, 1, three, &tn);
        printf("  sGetIsochStreamInfo(%u,%s) -> 0x%08x  %llu %llu %llu\n",
               idx, i ? "in " : "out", kr,
               (unsigned long long)three[0], (unsigned long long)three[1],
               (unsigned long long)three[2]);
    }

    // How many audio streams the driver counts for the device.
    uint64_t qin[2] = { dev_index, 0 };
    uint64_t cnt = 0;
    uint32_t cn = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, 13, qin, 2, &cnt, &cn);
    printf("  sGetNumAudioStreams(%u)     -> 0x%08x  %llu\n",
           dev_index, kr, (unsigned long long)cnt);
}

int main(int argc, char **argv)
{
    uint32_t out_channel = 0;   // the value that worked in --start-device
    uint32_t in_channel = 1;    // a different channel, so the two do not collide
    uint32_t rate = 0;          // 0 = leave the defaults alone
    int run_for = 0;            // 0 = until signalled
    int devstart = 1;           // 0 = do NOT call sStartAudioDevice (let CoreAudio start it)
    int64_t syt_sync = -1;      // -1 = not given: default to the receive channel

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--out-channel=", 14) == 0) {
            out_channel = (uint32_t)strtoul(argv[i] + 14, NULL, 0);
        } else if (strncmp(argv[i], "--in-channel=", 13) == 0) {
            in_channel = (uint32_t)strtoul(argv[i] + 13, NULL, 0);
        } else if (strncmp(argv[i], "--rate=", 7) == 0) {
            rate = (uint32_t)strtoul(argv[i] + 7, NULL, 0);
        } else if (strncmp(argv[i], "--syt=", 6) == 0) {
            // --syt=CHANNEL forces the isochronous channel the driver derives its
            // presentation-time clock from. 0xffffffff means "this node is the
            // clock master" (the driver's current, default answer). A real
            // channel number makes the node a slave to whoever transmits on it -
            // which is the experiment for a tone that pulses at a slow,
            // countable rate (that is a clock beat, not a per-buffer glitch).
            syt_sync = (int64_t)strtoul(argv[i] + 6, NULL, 0);
        } else if (strncmp(argv[i], "--run-for=", 10) == 0) {
            // Bounded run, for scripted experiments.
            run_for = atoi(argv[i] + 10);
        } else if (strcmp(argv[i], "--no-devstart") == 0) {
            // Do not start the IOAudioEngine ourselves. Used to test whether the
            // system's own output can start it (and thus play through it), which
            // our pre-starting the engine may be preventing.
            devstart = 0;
        } else if (strcmp(argv[i], "--cm-style") == 0) {
            // Build the device the way the vendor connection manager does: one
            // audio stream per channel, channel index in the structure, and no
            // sSetIsochStreamChannel. See step_create_stream_cm.
            g_cm_style = 1;
        } else if (strncmp(argv[i], "--out-offset=", 13) == 0) {
            g_out_off = atoi(argv[i] + 13);
        } else if (strncmp(argv[i], "--out-chans=", 12) == 0) {
            g_out_chans = atoi(argv[i] + 12);
        } else if (strncmp(argv[i], "--out-map=", 10) == 0) {
            // A comma-separated list of 0-based isochronous subchannels. One
            // output audio stream is built per entry, in this order, so CoreAudio
            // output 1 is the first value. See docs/HOW-IT-WORKS.md.
            const char *s = argv[i] + 10;
            g_out_map_n = 0;
            while (*s && g_out_map_n < N12_MAX_STREAMS) {
                g_out_map[g_out_map_n++] = atoi(s);
                while (*s && *s != ',') {
                    s++;
                }
                if (*s == ',') {
                    s++;
                }
            }
        } else if (strcmp(argv[i], "--iso-per-stream") == 0) {
            // Give each per-channel stream its own isochronous parameter, created
            // at that channel, instead of sharing the single output parameter.
            g_iso_per_stream = 1;
        } else if (strncmp(argv[i], "--iso-channel=", 14) == 0) {
            // The channel sSetIsochStreamChannel programs on the per-channel
            // output isochronous parameter(s).
            g_iso_channel = atoi(argv[i] + 14);
        } else if (strncmp(argv[i], "--in-map=", 9) == 0) {
            // Comma-separated 0-based isochronous subchannels for the input
            // direction, one input stream per entry. See g_in_map above.
            const char *s = argv[i] + 9;
            g_in_map_n = 0;
            while (*s && g_in_map_n < N12_MAX_STREAMS) {
                g_in_map[g_in_map_n++] = atoi(s);
                while (*s && *s != ',') {
                    s++;
                }
                if (*s == ',') {
                    s++;
                }
            }
        } else {
            fprintf(stderr,
                    "usage: %s [--out-channel=N] [--in-channel=N] [--rate=N] [--syt=N] [--run-for=N]\n"
                    "          [--out-offset=N] [--out-chans=N] [--out-map=6,7,...]\n"
                    "          [--in-map=0,1,...] [--iso-per-stream] [--iso-channel=N]\n"
                    "  makes the Yamaha n12 appear in CoreAudio and keeps it\n"
                    "  there until stopped (SIGINT/SIGTERM), or N seconds\n"
                    "  --syt defaults to the receive channel (the desk's transmit\n"
                    "  channel), i.e. the desk's clock; --syt=0xffffffff makes\n"
                    "  this node the master instead\n"
                    "  --out-map builds one output stream per listed 0-based\n"
                    "  isochronous subchannel, so each desk channel is a separate\n"
                    "  CoreAudio output (desk 1-8 = 6,7,8,9,10,11,12,13;\n"
                    "  desk 9-12 = 2,3,4,5)\n"
                    "  --in-map is the same for the input direction (desk DAW OUT\n"
                    "  n = subchannel n-1; the stereo bus is 15/16 = 14,15)\n",
                    argv[0]);
            return 2;
        }
    }

    printf("n12_agent - keep the n12 alive as a CoreAudio device\n\n");

    // The agent runs until stopped, so its output is often watched through a
    // pipe or a file. Unbuffered output means each line appears as it happens
    // instead of only when the process exits.
    setvbuf(stdout, NULL, _IONBF, 0);

    g_state.service = find_service(YAMAHA_SERVICE);
    if (g_state.service == IO_OBJECT_NULL) {
        fprintf(stderr,
                "service %s not found.\n"
                "Load the kexts first: sudo tools/try_kextload.sh\n",
                YAMAHA_SERVICE);
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

    // 1. the audio device.
    int dev = step_create_device(g_state.conn);
    if (dev < 0) {
        fprintf(stderr, "  could not create the audio device\n");
        teardown();
        IOObjectRelease(g_state.service);
        return 1;
    }
    g_state.dev_index = (uint32_t)dev;
    g_state.have_dev = 1;

    // Tell the driver which sample rates a CoreAudio client may select.
    //
    // Why this matters: the engine's IOAudioStream only accepts a format change
    // if the requested rate is in the device's "available rates" list
    // (YamahaFWAudioEngine::setAvailableSampleRate -> performFormatChange). We
    // never populated that list, so it was empty and *every* format-change
    // request failed. The AudioUnit output path never sets the format (it reads
    // the device's and converts), which is why Logic/VLC/browsers worked - but
    // the AudioQueue path (afplay, QuickTime, Music) does set it, so it failed
    // silently. A count of 0 selects the driver's own default list:
    // 44100, 48000, 88200, 96000.
    {
        uint64_t nrates = 0;   // 0 => driver default list
        kr = IOConnectCallScalarMethod(g_state.conn, SEL_SET_AVAILABLE_RATES,
                                       &nrates, 1, NULL, NULL);
        printf("  sSetAvailableSampleRates(defaults)  -> 0x%08x\n", kr);
    }

    // 2. the isochronous parameters - one per direction. The two build styles
    //    below differ only in how the audio streams are then created.
    int out_iso = step_create_isoch(g_state.conn, out_channel, 0,
                                    N12_CHANNELS, 1);
    int in_iso = step_create_isoch(g_state.conn, in_channel, 1,
                                   N12_CHANNELS, 1);
    if (out_iso >= 0) { g_state.out_isoch = (uint32_t)out_iso; g_state.have_out_isoch = 1; }
    if (in_iso  >= 0) { g_state.in_isoch  = (uint32_t)in_iso;  g_state.have_in_isoch  = 1; }

    // ----------------------------------------------------------------- output
    if (g_out_map_n > 0) {
        // --- per-desk-channel output (docs/HOW-IT-WORKS.md) ---
        // One output audio stream per mapped channel. Entry k (0-based) becomes
        // device channel k+1 and transmits on isochronous subchannel
        // g_out_map[k]. By default every stream shares the one output isoch
        // parameter - which is exactly the vendor's shape, sixteen streams on one
        // parameter - because the driver only objects to two streams claiming the
        // same channel number, and these all differ.
        printf("  building %d output stream(s), one per desk channel:\n",
               g_out_map_n);
        char nm[32];
        if (!g_state.have_out_isoch) {
            fprintf(stderr, "  no output isoch parameter - cannot build channels\n");
        } else {
            // The vendor path also programs the isoch client's channel with
            // sSetIsochStreamChannel; the legacy single-stream build does this and
            // it matters - without it the client has no channel and nothing is
            // transmitted. Do it for the shared parameter here (and per parameter
            // in --iso-per-stream below).
            int iso_ch = (g_iso_channel >= 0) ? g_iso_channel : (int)out_channel;
            if (!g_iso_per_stream) {
                step_set_channel(g_state.conn, out_iso, (uint32_t)iso_ch);
            }
            for (int k = 0; k < g_out_map_n; k++) {
                uint32_t iso = g_state.out_isoch;
                if (g_iso_per_stream) {
                    // The alternative shape: a fresh isochronous parameter (hence
                    // its own CIsochClient, and its own per-channel buffer) for
                    // each stream.
                    int ni = step_create_isoch(g_state.conn, (uint32_t)iso_ch,
                                               0, N12_CHANNELS, 1);
                    if (ni < 0) {
                        continue;   // skip this channel, keep the others
                    }
                    iso = (uint32_t)ni;
                    g_state.map_isochs[g_state.map_isoch_n++] = iso;
                    step_set_channel(g_state.conn, ni, (uint32_t)iso_ch);
                }
                // Device channel k+1 is named "Desk k+1" so the DAW shows the desk
                // channel, not a bare number.
                snprintf(nm, sizeof(nm), "Desk %d", k + 1);
                int s = step_create_stream_chan(g_state.conn, g_state.dev_index,
                                                iso, (uint32_t)(k + 1),
                                                (uint32_t)g_out_map[k], 0, nm);
                if (s >= 0) {
                    g_state.map_streams[g_state.map_stream_n++] = (uint32_t)s;
                }
            }
        }
    } else if (g_cm_style) {
        // --- vendor style, output half (docs/HOW-IT-WORKS.md) ---
        if (g_state.have_out_isoch) {
            char nm[32];
            for (uint32_t c = 0; c < N12_CHANNELS; c++) {
                snprintf(nm, sizeof(nm), "n12 out %u", c);
                step_create_stream_cm(g_state.conn, g_state.dev_index,
                                      g_state.out_isoch, c, 0, nm);
            }
        }
    } else if (g_state.have_out_isoch) {
        // --- our original style: one output stream, many channels ---
        // --out-chans can shrink it so the device presents fewer channels, which
        // is the experiment for making CoreAudio's stereo land on the desk's
        // inputs.
        uint32_t oc = g_out_chans ? (uint32_t)g_out_chans : N12_CHANNELS;
        step_set_channel(g_state.conn, out_iso, out_channel);
        int s = step_create_stream(g_state.conn, g_state.dev_index,
                                   g_state.out_isoch, oc, 0, "n12 out");
        if (s >= 0) { g_state.out_stream = (uint32_t)s; g_state.have_out_stream = 1; }
    } else {
        fprintf(stderr, "  no output isoch stream - the device will be silent\n");
    }

    // ------------------------------------------------------------------ input
    if (g_in_map_n > 0) {
        // --- per-desk-channel input (the mirror of --out-map) ---
        // One input stream per listed subchannel, so the desk's DAW OUT n lands
        // on Mac input n. The desk's stereo bus is DAW OUT 15/16, which is why
        // recording it wants the full --in-map=0,...,15.
        printf("  building %d input stream(s), one per desk transmit channel:\n",
               g_in_map_n);
        char nm[32];
        if (!g_state.have_in_isoch) {
            fprintf(stderr, "  no input isoch parameter - cannot build channels\n");
        } else {
            step_set_channel(g_state.conn, in_iso, in_channel);
            for (int k = 0; k < g_in_map_n; k++) {
                snprintf(nm, sizeof(nm), "In %d", k + 1);
                int s = step_create_stream_chan(g_state.conn, g_state.dev_index,
                                                g_state.in_isoch, (uint32_t)(k + 1),
                                                (uint32_t)g_in_map[k], 1, nm);
                if (s >= 0) {
                    g_state.in_map_streams[g_state.in_map_stream_n++] = (uint32_t)s;
                }
            }
        }
    } else if (g_cm_style) {
        // --- vendor style, input half ---
        if (g_state.have_in_isoch) {
            char nm[32];
            for (uint32_t c = 0; c < N12_CHANNELS; c++) {
                snprintf(nm, sizeof(nm), "n12 in %u", c);
                step_create_stream_cm(g_state.conn, g_state.dev_index,
                                      g_state.in_isoch, c, 1, nm);
            }
        }
    } else if (g_state.have_in_isoch) {
        // --- our original style: one input stream, many channels ---
        step_set_channel(g_state.conn, in_iso, in_channel);
        int s = step_create_stream(g_state.conn, g_state.dev_index,
                                   g_state.in_isoch, N12_CHANNELS, 1, "n12 in");
        if (s >= 0) { g_state.in_stream = (uint32_t)s; g_state.have_in_stream = 1; }
    } else {
        printf("  (no input isoch stream; output-only)\n");
    }

    // Optional: force a sample rate, for devices that default to one we did not
    // guess. sRequestChangeSampleRate is the device-level request; the per-stream
    // setter makes the isoch parameters agree with it.
    if (rate > 0) {
        uint64_t r = rate;
        kr = IOConnectCallScalarMethod(g_state.conn, SEL_REQUEST_CHANGE_RATE,
                                       &r, 1, NULL, NULL);
        printf("  sRequestChangeSampleRate(%u)       -> 0x%08x\n", rate, kr);
        if (g_state.have_out_isoch) {
            uint64_t a[2] = { g_state.out_isoch, rate };
            IOConnectCallScalarMethod(g_state.conn, SEL_SET_ISOCH_RATE,
                                      a, 2, NULL, NULL);
        }
        if (g_state.have_in_isoch) {
            uint64_t a[2] = { g_state.in_isoch, rate };
            IOConnectCallScalarMethod(g_state.conn, SEL_SET_ISOCH_RATE,
                                      a, 2, NULL, NULL);
        }
    }

    // Optional: force the clock source the driver derives presentation times
    // from. The default (below) slaved the node to the channel it RECEIVES on,
    // which is the desk's transmit channel - that is the desk's own clock, and
    // it is what makes the tone steady. The driver's own boot default is "this
    // node is the clock master", which free-runs and beats against the desk at a
    // slow, audible rate.
    //
    // -1 means "not given on the command line": fall back to the receive
    // channel, because whoever transmits to us is the clock we should follow.
    if (syt_sync < 0) {
        syt_sync = (int64_t)in_channel;
    }
    if (syt_sync >= 0) {
        uint64_t s = (uint64_t)syt_sync;
        kr = IOConnectCallScalarMethod(g_state.conn, SEL_SET_SYT_SYNC,
                                       &s, 1, NULL, NULL);
        printf("  sSetSYTSynchChannel(slave to ch %d) -> 0x%08x\n",
               (int)syt_sync, kr);
    }

    // 4. start it. Two separate steps, as CPCVars does it: requestStartStreams
    //    starts the isochronous DMA, and sStartAudioDevice starts the
    //    IOAudioEngine that CoreAudio talks to. Doing only the second leaves an
    //    engine with nothing feeding it.
    kr = IOConnectCallScalarMethod(g_state.conn, SEL_REQUEST_START_STREAMS,
                                   NULL, 0, NULL, NULL);
    printf("  sRequestStartStreams               -> 0x%08x\n", kr);

    uint64_t d = g_state.dev_index;
    if (devstart) {
        kr = IOConnectCallScalarMethod(g_state.conn, SEL_START_AUDIO_DEVICE,
                                       &d, 1, NULL, NULL);
        printf("  sStartAudioDevice(%u)              -> 0x%08x\n",
               g_state.dev_index, kr);
        if (kr != KERN_SUCCESS) {
            teardown();
            IOObjectRelease(g_state.service);
            return 1;
        }
    } else {
        printf("  sStartAudioDevice                   (skipped - letting CoreAudio start it)\n");
    }

    uint64_t nstreams = 0;
    uint64_t qin[2] = { g_state.dev_index, 0 };
    uint32_t ncnt = 1;
    IOConnectCallScalarMethod(g_state.conn, SEL_GET_NUM_AUDIO_STREAMS,
                              qin, 2, &nstreams, &ncnt);
    // In per-channel mode the device presents as many channels as we built
    // streams; otherwise it is the legacy single-stream width.
    uint32_t shown_out = (g_out_map_n > 0)
                             ? (uint32_t)g_state.map_stream_n
                             : N12_CHANNELS;
    uint32_t shown_in = (g_in_map_n > 0)
                            ? (uint32_t)g_state.in_map_stream_n
                            : N12_CHANNELS;
    printf("\nthe n12 should now be in CoreAudio with %u output and %u input channels\n",
           shown_out, shown_in);
    printf("(%llu audio stream(s) registered; %s)\n",
           (unsigned long long)nstreams,
           g_state.have_in_isoch ? "input and output" : "output only");
    if (g_out_map_n > 0) {
        // Spell out what each CoreAudio output should feed, so the mapping can be
        // checked against the desk by ear and eye rather than remembered.
        printf("output mapping: CoreAudio output -> isochronous subchannel "
               "(desk DAW IN = subchannel+1)\n");
        for (int k = 0; k < g_out_map_n; k++) {
            printf("  output %d -> subchannel %d (desk DAW IN %d)\n",
                   k + 1, g_out_map[k], g_out_map[k] + 1);
        }
    }
    if (g_in_map_n > 0) {
        printf("input mapping: CoreAudio input -> isochronous subchannel "
               "(desk DAW OUT = subchannel+1)\n");
        for (int k = 0; k < g_in_map_n; k++) {
            printf("  input %d -> subchannel %d (desk DAW OUT %d)\n",
                   k + 1, g_in_map[k], g_in_map[k] + 1);
        }
    }

    // What does the driver think it is doing? If these differ from the values
    // we passed, the guesses were wrong and audio cannot move.
    printf("\ndriver's own view:\n");
    report_driver_state(g_state.conn, g_state.dev_index,
                        g_state.out_isoch, g_state.in_isoch);

    printf("holding the user client open - Ctrl-C to stop\n\n");

    // Signal handlers cannot do the teardown themselves; they only set a flag.
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (run_for > 0) {
        printf("running for %d s\n\n", run_for);
        for (int t = 0; t < run_for && !g_stop; t++) {
            sleep(1);
        }
    } else {
        while (!g_stop) {
            // One second is short enough to notice a signal promptly and long
            // enough to stay idle. The device does not need polling to stay
            // alive; it is the open client that keeps it.
            sleep(1);
        }
    }

    printf("\nstopping\n");
    teardown();
    IOObjectRelease(g_state.service);
    return 0;
}
