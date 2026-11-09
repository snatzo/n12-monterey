/*
 * n12_diag.c - why is nothing coming out of the desk?
 *
 * Prints the four things that decide whether the system's audio reaches the
 * n12:
 *   - which device the system is currently playing to;
 *   - that device's output volume and mute state (a muted device is silent no
 *     matter how good the routing is);
 *   - whether the device's engine is actually running;
 *   - the stereo channel pair the system is using.
 *
 * It changes nothing. Build and run with the agent already started:
 *     clang -Wall -O2 -o n12_diag n12_diag.c \
 *           -framework CoreAudio -framework CoreFoundation
 *     ./n12_diag
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

/* The device's name, for the report. Returns 0 on success. */
static int device_name(AudioDeviceID dev, char *buf, size_t n)
{
    CFStringRef name = NULL;
    UInt32 ns = sizeof(name);
    AudioObjectPropertyAddress na = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &na, 0, NULL, &ns, &name) != noErr || name == NULL) {
        return -1;
    }
    CFStringGetCString(name, buf, n, kCFStringEncodingUTF8);
    CFRelease(name);
    return 0;
}

int main(void)
{
    // Which device is the system playing to right now?
    AudioDeviceID def = kAudioObjectUnknown;
    UInt32 ds = sizeof(def);
    AudioObjectPropertyAddress da = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &ds, &def);

    char name[256] = "(unknown)";
    if (def != kAudioObjectUnknown) device_name(def, name, sizeof(name));
    printf("system default output : id %u  \"%s\"\n", (unsigned)def, name);

    if (def == kAudioObjectUnknown) {
        return 1;
    }

    // Is the device's engine running? An engine that is stopped produces no
    // sound no matter what is played to it.
    UInt32 running = 0;
    UInt32 rs = sizeof(running);
    AudioObjectPropertyAddress ra = {
        kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(def, &ra, 0, NULL, &rs, &running) == noErr) {
        printf("engine running        : %s\n", running ? "yes" : "NO (stopped)");
    }

    // Master output mute and volume.
    UInt32 mute = 0;
    UInt32 ms = sizeof(mute);
    AudioObjectPropertyAddress ma = {
        kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(def, &ma, 0, NULL, &ms, &mute) == noErr) {
        printf("master output mute    : %s\n", mute ? "MUTED" : "not muted");
    } else {
        printf("master output mute    : (not settable on this device)\n");
    }

    Float32 vol = 0;
    UInt32 vs = sizeof(vol);
    AudioObjectPropertyAddress va = {
        kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(def, &va, 0, NULL, &vs, &vol) == noErr) {
        printf("master output volume  : %.3f\n", vol);
    } else {
        // Some devices have no master element; ask the individual channels.
        printf("master output volume  : (no master element)\n");
    }

    // The per-channel volumes of the stereo pair that matters (7 and 8), since
    // the routing puts stereo there.
    for (UInt32 ch = 7; ch <= 8; ch++) {
        Float32 cv = -1;
        UInt32 cs = sizeof(cv);
        AudioObjectPropertyAddress ca = {
            kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput, ch };
        if (AudioObjectGetPropertyData(def, &ca, 0, NULL, &cs, &cv) == noErr) {
            printf("output channel %u vol  : %.3f\n", (unsigned)ch, cv);
        }
    }

    // Which channels the system is using for stereo.
    UInt32 pair[2] = {0, 0};
    UInt32 ps = sizeof(pair);
    AudioObjectPropertyAddress pa = {
        kAudioDevicePropertyPreferredChannelsForStereo,
        kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(def, &pa, 0, NULL, &ps, pair) == noErr) {
        printf("stereo pair           : channels %u and %u\n",
               (unsigned)pair[0], (unsigned)pair[1]);
    }

    return 0;
}
