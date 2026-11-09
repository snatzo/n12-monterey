/*
 * n12_fmt.c - print the n12 CoreAudio device's real stream layout.
 *
 * Why this exists: n12_play assumes each AudioBuffer in its output callback is
 * ONE channel of float samples, and writes a continuous sine into each. If the
 * device is actually interleaved - one buffer holding all 16 channels
 * back to back - then writing a single continuous sine across that buffer puts
 * every 16th sample into any one channel. A 1 kHz sine sampled at 44100 Hz,
 * decimated by 16, becomes 62.5 Hz, and the waveform is a staircase. That is
 * heard as a buzzy, pulsating tone rather than a clean beep - which is exactly
 * what the desk's monitor produced.
 *
 * So before changing the tone generator, measure: how many buffers does the
 * device hand out, how many channels are in each, what is the sample format,
 * and is the interleaved flag set. This tool prints all of it and nothing else.
 *
 * Build:
 *     clang -Wall -O2 -o n12_fmt n12_fmt.c \
 *           -framework CoreAudio -framework CoreFoundation
 *
 * Run (the device only exists while n12_agent is running):
 *     ./n12_fmt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

/* Find the first audio device whose name contains "n12".
 * Returns kAudioObjectUnknown when the n12 driver is not publishing a device -
 * which happens whenever n12_agent is not running, because the Yamaha driver
 * tears the device down when its last user client closes. */
static AudioDeviceID find_device_named(const char *want)
{
    // The list of every audio device the system knows about.
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr) {
        return kAudioObjectUnknown;
    }
    int n = size / sizeof(AudioDeviceID);
    AudioDeviceID *ids = calloc(n, sizeof(AudioDeviceID));
    if (ids == NULL) return kAudioObjectUnknown;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, ids);

    AudioDeviceID found = kAudioObjectUnknown;
    for (int i = 0; i < n; i++) {
        CFStringRef name = NULL;
        UInt32 ns = sizeof(name);
        AudioObjectPropertyAddress na = {
            kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &ns, &name) != noErr || name == NULL) {
            continue;
        }
        // Copy the CFString into a plain C buffer so strstr() can look at it.
        char buf[256] = {0};
        CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(name);
        if (strstr(buf, want) != NULL) {
            found = ids[i];
            printf("device: \"%s\"  id %u\n", buf, (unsigned)found);
            break;
        }
    }
    free(ids);
    return found;
}

/* Print the channel layout for one scope (input or output).
 * CoreAudio reports this as a list of buffers; each buffer says how many
 * channels it holds. One buffer with 16 channels means interleaved; sixteen
 * buffers with 1 channel each means non-interleaved. */
static void show_layout(AudioDeviceID dev, AudioObjectPropertyScope scope, const char *label)
{
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamConfiguration, scope,
        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr || size == 0) {
        printf("  %s: no stream configuration\n", label);
        return;
    }
    AudioBufferList *list = malloc(size);
    if (list == NULL) return;
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, list) != noErr) {
        printf("  %s: could not read stream configuration\n", label);
        free(list);
        return;
    }
    printf("  %s: %u buffer(s)\n", label, (unsigned)list->mNumberBuffers);
    UInt32 total = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
        printf("      buffer %u: %u channel(s)\n",
               (unsigned)i, (unsigned)list->mBuffers[i].mNumberChannels);
        total += list->mBuffers[i].mNumberChannels;
    }
    printf("      total %u channel(s) -> %s\n", (unsigned)total,
           list->mNumberBuffers > 1 ? "NON-INTERLEAVED (one buffer per channel)"
                                    : "INTERLEAVED (one buffer, many channels)");
    free(list);
}

/* Print the format of every stream in a scope. This is the part that decides
 * whether the callback's data is float, how big a frame is, and whether the
 * samples of different channels are adjacent (interleaved) or separated. */
static void show_formats(AudioDeviceID dev, AudioObjectPropertyScope scope, const char *label)
{
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr || size == 0) {
        printf("  %s: no streams\n", label);
        return;
    }
    int n = size / sizeof(AudioStreamID);
    AudioStreamID *streams = calloc(n, sizeof(AudioStreamID));
    if (streams == NULL) return;
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, streams) != noErr) {
        free(streams);
        return;
    }
    for (int i = 0; i < n; i++) {
        AudioStreamBasicDescription asbd = {0};
        UInt32 s = sizeof(asbd);
        AudioObjectPropertyAddress fa = {
            kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(streams[i], &fa, 0, NULL, &s, &asbd) != noErr) {
            printf("  %s stream %d: no format\n", label, i);
            continue;
        }
        printf("  %s stream %d: %.0f Hz, %u ch, %u bits, %u bytes/frame, %u frames/packet\n",
               label, i, asbd.mSampleRate, (unsigned)asbd.mChannelsPerFrame,
               (unsigned)asbd.mBitsPerChannel, (unsigned)asbd.mBytesPerFrame,
               (unsigned)asbd.mFramesPerPacket);
        // Decode the format flags into the two facts that matter here.
        printf("      virtual : id 0x%08x  %s  %s\n",
               (unsigned)asbd.mFormatID,
               (asbd.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "integer",
               (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved)
                   ? "NON-interleaved (one buffer per channel)" : "interleaved");

        // The physical format is what the hardware runs at; the virtual format
        // is what clients see. They can differ, and the AudioQueue path uses
        // them differently - which is the point of printing both.
        AudioStreamBasicDescription ph = {0};
        UInt32 ps = sizeof(ph);
        AudioObjectPropertyAddress pa = {
            kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(streams[i], &pa, 0, NULL, &ps, &ph) == noErr) {
            printf("      physical: %.0f Hz, %u ch, %u bits, %u bytes/frame  %s  %s\n",
                   ph.mSampleRate, (unsigned)ph.mChannelsPerFrame,
                   (unsigned)ph.mBitsPerChannel, (unsigned)ph.mBytesPerFrame,
                   (ph.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "integer",
                   (ph.mFormatFlags & kAudioFormatFlagIsNonInterleaved)
                       ? "NON-interleaved" : "interleaved");
        }
    }
    free(streams);
}

int main(int argc, char **argv)
{
    // An optional argument names the device (default "n12"), so the same dump
    // can be produced for a known-good device such as "Built-in Output" and the
    // two compared directly.
    const char *want = (argc > 1) ? argv[1] : "n12";

    AudioDeviceID dev = find_device_named(want);
    if (dev == kAudioObjectUnknown) {
        printf("no CoreAudio device with \"n12\" in its name\n");
        printf("(start tools/n12_agent first - the device only exists while it runs)\n");
        return 1;
    }

    // The frame size the device wants per callback. A mismatch with what the
    // driver's isochronous packet layout expects shows up as periodic glitches.
    UInt32 frames = 0;
    UInt32 fs = sizeof(frames);
    AudioObjectPropertyAddress ba = {
        kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &ba, 0, NULL, &fs, &frames) == noErr) {
        printf("buffer frame size: %u\n", (unsigned)frames);
    }

    // Nominal rate - compared against the desk's 44100 in its ISP records.
    Float64 sr = 0;
    UInt32 ss = sizeof(sr);
    AudioObjectPropertyAddress sa = {
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &sa, 0, NULL, &ss, &sr) == noErr) {
        printf("nominal sample rate: %.0f Hz\n", sr);
    }

    printf("\noutput side (what n12_play writes into):\n");
    show_layout(dev, kAudioDevicePropertyScopeOutput, "output");
    show_formats(dev, kAudioDevicePropertyScopeOutput, "output");

    printf("\ninput side (what n12_capture reads):\n");
    show_layout(dev, kAudioDevicePropertyScopeInput, "input");
    show_formats(dev, kAudioDevicePropertyScopeInput, "input");

    return 0;
}
