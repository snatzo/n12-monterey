/*
 * setout.c - set the system default output device by name.
 *
 * Why: to test the audio path itself, we need to move the Mac's output between
 * the n12 and the built-in without going through System Settings. This is the
 * same call the Sound pane makes.
 *
 * Usage:
 *     ./setout                 # list the output-capable devices
 *     ./setout n12             # make the n12 the default output
 *     ./setout built-in        # make the built-in output the default
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

static int device_name(AudioDeviceID dev, char *buf, size_t n)
{
    CFStringRef name = NULL;
    UInt32 ns = sizeof(name);
    AudioObjectPropertyAddress a = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &ns, &name) != noErr || name == NULL) {
        return -1;
    }
    CFStringGetCString(name, buf, n, kCFStringEncodingUTF8);
    CFRelease(name);
    return 0;
}

int main(int argc, char **argv)
{
    AudioObjectPropertyAddress la = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &la, 0, NULL, &sz);
    int n = sz / sizeof(AudioDeviceID);
    AudioDeviceID *ids = calloc(n ? n : 1, sizeof(AudioDeviceID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &la, 0, NULL, &sz, ids);

    if (argc < 2) {
        for (int i = 0; i < n; i++) {
            char nm[256] = ""; device_name(ids[i], nm, sizeof(nm));
            printf("  id %-4u %s\n", (unsigned)ids[i], nm);
        }
        free(ids);
        return 0;
    }

    AudioDeviceID found = kAudioObjectUnknown;
    for (int i = 0; i < n; i++) {
        char nm[256] = ""; device_name(ids[i], nm, sizeof(nm));
        if (strcasestr(nm, argv[1]) != NULL) { found = ids[i]; break; }
    }
    free(ids);
    if (found == kAudioObjectUnknown) { printf("no device matching \"%s\"\n", argv[1]); return 1; }

    AudioObjectPropertyAddress da = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    OSStatus st = AudioObjectSetPropertyData(kAudioObjectSystemObject, &da, 0, NULL,
                                             sizeof(found), &found);
    char nm[256] = ""; device_name(found, nm, sizeof(nm));
    printf("default output -> %s (id %u), status %d\n", nm, (unsigned)found, (int)st);
    return 0;
}
