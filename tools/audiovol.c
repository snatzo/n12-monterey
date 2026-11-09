/*
 * audiovol.c - read or set the output volume of an audio device.
 *
 * Why this exists: to prove where a sound is coming from, the cleanest test is
 * to silence every other output. If the Mac's built-in speakers are muted and
 * the n12 is the default output, then anything still audible must be coming out
 * of the desk. Without that, "I heard a beep" and "I heard nothing" are
 * ambiguous - the Mac's own speakers are always a candidate.
 *
 * Usage:
 *     ./audiovol                     # list devices and their output volume
 *     ./audiovol built-in            # read the built-in output's volume
 *     ./audiovol built-in 0.0        # mute it
 *     ./audiovol built-in 0.5        # half volume
 *
 * The second argument is a volume from 0.0 to 1.0. Only output volume is
 * touched; nothing else about the device is changed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>          // strcasestr
#include <ctype.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

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

/* Read the master output volume. Most devices here expose no master element, so
 * fall back to the first channel's volume, which is enough to see the setting. */
static float read_volume(AudioDeviceID dev)
{
    Float32 v = -1;
    UInt32 s = sizeof(v);
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &s, &v) == noErr) {
        return v;
    }
    // No master element: ask channel 1 (elements are 1-based on the device).
    a.mElement = 1;
    s = sizeof(v);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &s, &v) == noErr) {
        return v;
    }
    return -1;
}

/* Set the output volume. Tries the master element first; if the device has
 * none, sets each of the individual channels, which is the common case for
 * multichannel devices. Returns the number of elements that took the value. */
static int set_volume(AudioDeviceID dev, Float32 nv)
{
    UInt32 s = sizeof(nv);
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain };
    if (AudioObjectSetPropertyData(dev, &a, 0, NULL, s, &nv) == noErr) {
        return 1;
    }
    int ok = 0;
    for (UInt32 ch = 1; ch <= 8; ch++) {
        a.mElement = ch;
        if (AudioObjectSetPropertyData(dev, &a, 0, NULL, s, &nv) == noErr) {
            ok++;
        }
    }
    return ok;
}

int main(int argc, char **argv)
{
    // List every device and its volume.
    if (argc == 1) {
        AudioObjectPropertyAddress a = {
            kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        UInt32 size = 0;
        AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
        int n = size / sizeof(AudioDeviceID);
        AudioDeviceID *ids = calloc(n, sizeof(AudioDeviceID));
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);
        for (int i = 0; i < n; i++) {
            char name[256] = "(?)";
            device_name(ids[i], name, sizeof(name));
            float v = read_volume(ids[i]);
            if (v < 0) printf("  id %-4u  %s   (no master volume)\n", (unsigned)ids[i], name);
            else       printf("  id %-4u  %s   volume %.3f\n", (unsigned)ids[i], name, v);
        }
        free(ids);
        return 0;
    }

    const char *want = argv[1];
    AudioObjectPropertyAddress a = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
    int n = size / sizeof(AudioDeviceID);
    AudioDeviceID *ids = calloc(n, sizeof(AudioDeviceID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);

    AudioDeviceID found = kAudioObjectUnknown;
    for (int i = 0; i < n; i++) {
        char name[256] = "";
        device_name(ids[i], name, sizeof(name));
        if (strcasestr(name, want) != NULL) { found = ids[i]; break; }
    }
    free(ids);
    if (found == kAudioObjectUnknown) {
        printf("no output device matching \"%s\"\n", want);
        return 1;
    }

    char name[256] = "";
    device_name(found, name, sizeof(name));

    if (argc == 2) {
        float v = read_volume(found);
        if (v < 0) printf("%s: no master volume control\n", name);
        else       printf("%s: volume %.3f\n", name, v);
        return 0;
    }

    float old = read_volume(found);
    Float32 nv = (Float32)atof(argv[2]);
    int nset = set_volume(found, nv);
    if (nset == 0) {
        printf("could not set %s volume (no settable element)\n", name);
        return 1;
    }
    printf("%s: volume %.3f -> %.3f (%d element(s) set)\n", name, old, nv, nset);
    return 0;
}
