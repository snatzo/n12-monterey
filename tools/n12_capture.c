/*
 * n12_capture.c - record from a CoreAudio device and report per-channel levels.
 *
 * Why this exists: n12_agent makes the n12 appear in CoreAudio with 16 inputs
 * and 16 outputs, but "appears" is not "works". The only way to know whether
 * the isochronous path is real is to ask CoreAudio for samples and look at
 * them.
 *
 * It can also record from any other device by name, which is the point of the
 * `--name` option: a control run against a device known to work proves the
 * measuring program itself is sound, so a silent n12 means something real.
 *
 * If the levels are non-zero (even a noise floor) the receive path is live. If
 * the channel count comes back as zero frames, CoreAudio called the callback
 * zero times - the device produced nothing to record.
 *
 * Build:
 *     clang -Wall -O2 -o n12_capture n12_capture.c \
 *           -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
 *
 * Run (with n12_agent already running):
 *     ./n12_capture                        # the n12, 3 seconds
 *     ./n12_capture --name=USB --seconds=2 # a control device
 *
 * Note on macOS 12: recording from an audio device can be gated by the
 * microphone privacy setting. A plain command-line binary has no usage
 * description, so a refusal is possible; it is reported, not hidden.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>

#define MAX_CHANNELS 64u

/* Set once, before the audio unit starts. */
static unsigned g_channels;
static float g_peak[MAX_CHANNELS];
static double g_sumsq[MAX_CHANNELS];
static uint64_t g_frames;
static OSStatus g_render_err;

/* Locate a CoreAudio device whose name contains `want`. */
static AudioDeviceID find_device(const char *want)
{
    CFStringRef needle = CFStringCreateWithCString(NULL, want,
                                                   kCFStringEncodingUTF8);
    if (needle == NULL) {
        return kAudioObjectUnknown;
    }

    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr,
                                       0, NULL, &size) != noErr) {
        CFRelease(needle);
        return kAudioObjectUnknown;
    }
    UInt32 count = size / sizeof(AudioDeviceID);
    AudioDeviceID *ids = calloc(count, sizeof(AudioDeviceID));
    if (ids == NULL) {
        CFRelease(needle);
        return kAudioObjectUnknown;
    }
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                   0, NULL, &size, ids) != noErr) {
        free(ids);
        CFRelease(needle);
        return kAudioObjectUnknown;
    }

    AudioDeviceID found = kAudioObjectUnknown;
    for (UInt32 i = 0; i < count; i++) {
        CFStringRef name = NULL;
        UInt32 nameSize = sizeof(name);
        AudioObjectPropertyAddress na = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain,
        };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &nameSize,
                                       &name) == noErr && name != NULL) {
            Boolean match = CFStringFind(name, needle, 0).location != kCFNotFound;
            CFRelease(name);
            if (match) {
                found = ids[i];
                break;
            }
        }
    }
    free(ids);
    CFRelease(needle);
    return found;
}

/* Number of channels the device offers on its input side. */
static unsigned input_channel_count(AudioDeviceID dev)
{
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeInput,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr) {
        return 0;
    }
    AudioBufferList *list = malloc(size);
    if (list == NULL) {
        return 0;
    }
    unsigned channels = 0;
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, list) == noErr) {
        for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
            channels += list->mBuffers[i].mNumberChannels;
        }
    }
    free(list);
    return channels;
}

static double device_sample_rate(AudioDeviceID dev)
{
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    Float64 rate = 0;
    UInt32 size = sizeof(rate);
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &rate) != noErr) {
        return 0;
    }
    return rate;
}

/*
 * The input callback. A HAL input unit does not hand us the samples; we ask for
 * them with AudioUnitRender on bus 1, then accumulate.
 */
static OSStatus input_proc(void *refCon, AudioUnitRenderActionFlags *flags,
                           const AudioTimeStamp *ts, UInt32 bus,
                           UInt32 frames, AudioBufferList *ioData)
{
    (void)ioData; (void)bus;
    static float scratch[4096 * MAX_CHANNELS];
    if (frames > 4096) frames = 4096;
    if (g_channels == 0 || g_channels > MAX_CHANNELS) return noErr;

    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = g_channels;
    abl.mBuffers[0].mDataByteSize = frames * g_channels * sizeof(float);
    abl.mBuffers[0].mData = scratch;

    OSStatus st = AudioUnitRender((AudioUnit)refCon, flags, ts, 1, frames, &abl);
    if (st != noErr) {
        g_render_err = st;
        return st;
    }

    for (UInt32 f = 0; f < frames; f++) {
        const float *row = scratch + (size_t)f * g_channels;
        for (unsigned ch = 0; ch < g_channels; ch++) {
            float v = row[ch];
            float a = fabsf(v);
            if (a > g_peak[ch]) {
                g_peak[ch] = a;
            }
            g_sumsq[ch] += (double)v * (double)v;
        }
    }
    g_frames += frames;
    return noErr;
}

int main(int argc, char **argv)
{
    const char *want = "n12";
    int seconds = 3;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--name=", 7) == 0) {
            want = argv[i] + 7;
        } else if (strncmp(argv[i], "--seconds=", 10) == 0) {
            seconds = atoi(argv[i] + 10);
        } else if (argv[i][0] != '-') {
            seconds = atoi(argv[i]);
        } else {
            fprintf(stderr, "usage: %s [--name=SUBSTR] [--seconds=N]\n",
                    argv[0]);
            return 2;
        }
    }
    if (seconds < 1) seconds = 1;

    printf("n12_capture - record from a CoreAudio device and report levels\n\n");

    AudioDeviceID dev = find_device(want);
    if (dev == kAudioObjectUnknown) {
        printf("no CoreAudio device with \"%s\" in its name.\n", want);
        if (strstr(want, "n12")) {
            printf("Start tools/n12_agent first - the device only exists while\n"
                   "something holds the driver's user client open.\n");
        }
        return 1;
    }
    unsigned channels = input_channel_count(dev);
    double rate = device_sample_rate(dev);
    printf("found \"%s\": device id %u, %u input channel(s), %.0f Hz\n",
           want, (unsigned)dev, channels, rate);
    if (channels == 0) {
        printf("the device reports no input channels\n");
        return 1;
    }
    if (channels > MAX_CHANNELS) {
        channels = MAX_CHANNELS;
    }
    g_channels = channels;

    AudioComponentDescription desc;
    memset(&desc, 0, sizeof(desc));
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (comp == NULL) {
        printf("no HAL output audio unit\n");
        return 1;
    }
    AudioUnit unit = NULL;
    if (AudioComponentInstanceNew(comp, &unit) != noErr) {
        printf("could not instantiate the audio unit\n");
        return 1;
    }

    // Turn the "output" unit into an input: enable bus 1's input, disable bus
    // 0's output, and point it at the device we chose.
    UInt32 one = 1, zero = 0;
    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Input, 1, &one, sizeof(one));
    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Output, 0, &zero, sizeof(zero));
    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global, 0, &dev, sizeof(dev));

    // Interleaved Float32 at the device's own rate.
    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate       = rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerFrame    = channels * sizeof(float);
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;
    fmt.mFramesPerPacket  = 1;

    OSStatus st = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                       kAudioUnitScope_Output, 1,
                                       &fmt, sizeof(fmt));
    if (st != noErr) {
        printf("could not set the input format (0x%08x)\n", (unsigned)st);
        AudioComponentInstanceDispose(unit);
        return 1;
    }

    AURenderCallbackStruct cb;
    cb.inputProc = input_proc;
    cb.inputProcRefCon = unit;
    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback,
                         kAudioUnitScope_Global, 0, &cb, sizeof(cb));

    if (AudioUnitInitialize(unit) != noErr) {
        printf("could not initialise the audio unit\n");
        AudioComponentInstanceDispose(unit);
        return 1;
    }

    printf("recording %d s...\n", seconds);
    st = AudioOutputUnitStart(unit);
    if (st != noErr) {
        printf("AudioOutputUnitStart failed: 0x%08x\n", (unsigned)st);
        printf("  (check System Settings -> Privacy & Security -> Microphone)\n");
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        return 1;
    }

    for (int i = 0; i < seconds * 10; i++) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);

    printf("\n%llu frames", (unsigned long long)g_frames);
    if (g_render_err != noErr) {
        printf("  (AudioUnitRender error 0x%08x)", (unsigned)g_render_err);
    }
    printf("\n ch    peak      rms\n");
    for (unsigned ch = 0; ch < channels; ch++) {
        double rms = g_frames ? sqrt(g_sumsq[ch] / (double)g_frames) : 0.0;
        printf(" %2u  %8.6f  %8.6f\n", ch, g_peak[ch], rms);
    }

    int live = 0;
    for (unsigned ch = 0; ch < channels; ch++) {
        if (g_peak[ch] > 0.0f) live++;
    }
    printf("\n%d of %u channels carry any non-zero sample\n", live, channels);
    if (g_frames == 0) {
        printf("the callback never ran: the device produced no samples at all\n");
    } else if (live == 0) {
        printf("samples arrived but were all zero (digital silence)\n");
    }
    return 0;
}
