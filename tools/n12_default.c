/*
 * n12_default.c - make the n12 the Mac's default audio output.
 *
 * Why this exists: to test playback with real audio you have to point the Mac's
 * output at the n12, which normally means System Settings > Sound. That is a lot
 * of clicking, and the device only exists while n12_agent is running, so the
 * menu can be empty if you look at the wrong moment. This does the same thing
 * from the command line: find the device called "n12" and set it as the system
 * default output.
 *
 * It changes only which device the system plays to. It does not touch the desk
 * and does not change the n12 itself.
 *
 * Build:
 *     clang -Wall -O2 -o n12_default n12_default.c \
 *           -framework CoreAudio -framework CoreFoundation
 *
 * Run (with n12_agent already running, or the device will not be found):
 *     ./n12_default
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>          /* offsetof, for the channel layout below */

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

/* Return the audio device whose name contains "n12", or kAudioObjectUnknown.
 * Same lookup as the other tools, so all of them agree on which device they
 * mean. */
static AudioDeviceID find_device_named(const char *want)
{
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
        char buf[256] = {0};
        CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(name);
        if (strstr(buf, want) != NULL) {
            printf("found \"%s\" (id %u)\n", buf, (unsigned)ids[i]);
            found = ids[i];
            break;
        }
    }
    free(ids);
    return found;
}

int main(int argc, char **argv)
{
    // Which two output channels the system's stereo pair should use. Default
    // 1-2, which is what the desk's `C-R SOURCE SELECT [DAW]` monitors (the
    // manual: "[DAW] - Enables you to monitor the DAW stereo output (DAW IN
    // 1 & 2)"). Use `--stereo=7,8` if you instead monitor the desk's stereo bus
    // (`C-R [ST]`), because then the audio has to go through desk channels 1-2,
    // which are fed from DAW IN 7-8.
    UInt32 ch_l = 1, ch_r = 2;
    int want_layout = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--stereo=", 9) == 0) {
            unsigned a = 0, b = 0;
            if (sscanf(argv[i] + 9, "%u,%u", &a, &b) == 2) { ch_l = a; ch_r = b; }
        } else if (strcmp(argv[i], "--layout") == 0) {
            // Also declare a full preferred channel layout, labelling the chosen
            // pair Left/Right. Without a layout the HAL keeps putting stereo on
            // the device's first two channels, whatever the stereo pair says.
            want_layout = 1;
        }
    }

    AudioDeviceID dev = find_device_named("n12");
    if (dev == kAudioObjectUnknown) {
        printf("no CoreAudio device with \"n12\" in its name\n");
        printf("(start the device first:  ./n12_on.sh)\n");
        return 1;
    }

    // The system-wide default output device. Setting this is exactly what the
    // Sound pane does when you click a device.
    AudioObjectPropertyAddress out = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    OSStatus st = AudioObjectSetPropertyData(kAudioObjectSystemObject, &out,
                                             0, NULL, sizeof(dev), &dev);
    if (st != noErr) {
        printf("could not set the default output: %d\n", (int)st);
        return 1;
    }

    // Which two of the 16 output channels the system's stereo pair should use.
    //
    // This matters because the n12's DAW IN 1-2 are routed to NO input channel
    // (manual block diagram: DAW IN 7-10 -> input ch 1-4, DAW IN 11-14 -> ch
    // 5-8, DAW IN 3-6 -> ch 9-12; 1 and 2 are unassigned). A normal stereo
    // source plays on the device's first two channels, so out of the box
    // everything the Mac plays lands on DAW IN 1-2 and never reaches a desk
    // channel - which is heard as silence even though the link is fine.
    //
    // Point the system's stereo pair at the chosen channels.
    UInt32 stereo[2] = { ch_l, ch_r };
    AudioObjectPropertyAddress pcs = {
        kAudioDevicePropertyPreferredChannelsForStereo,
        kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
    OSStatus ss = AudioObjectSetPropertyData(dev, &pcs, 0, NULL,
                                             sizeof(stereo), stereo);
    if (ss == noErr) {
        printf("stereo pair routed to output channels %u-%u\n", ch_l, ch_r);
    } else {
        printf("could not set the stereo channel pair: %d\n", (int)ss);
    }

    if (want_layout) {
        // Declare a full 16-channel preferred layout: every channel Discrete
        // except the chosen pair, which are labelled Left and Right. The HAL
        // identifies the stereo pair by label, so this is what should make
        // ordinary stereo use channels ch_l/ch_r instead of the first two.
        UInt8 buf[512];
        memset(buf, 0, sizeof(buf));
        AudioChannelLayout *l = (AudioChannelLayout *)buf;
        l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
        l->mNumberChannelDescriptions = 16;
        for (int i = 0; i < 16; i++) {
            l->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Discrete;
        }
        // Channel numbers here are 1-based, so ch_l is the ch_l'th channel.
        l->mChannelDescriptions[ch_l - 1].mChannelLabel = kAudioChannelLabel_Left;
        l->mChannelDescriptions[ch_r - 1].mChannelLabel = kAudioChannelLabel_Right;
        UInt32 sz = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions)
                             + 16 * sizeof(AudioChannelDescription));
        AudioObjectPropertyAddress la = {
            kAudioDevicePropertyPreferredChannelLayout,
            kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
        OSStatus ls = AudioObjectSetPropertyData(dev, &la, 0, NULL, sz, l);
        printf("preferred channel layout (L=%u R=%u) -> %d\n",
               ch_l, ch_r, (int)ls);
    }

    printf("the n12 is now the Mac's default output - play something and listen\n");

    // Read the stereo pair back too, so the routing is visible rather than
    // assumed.
    UInt32 back[2] = { 0, 0 };
    UInt32 bs = sizeof(back);
    if (AudioObjectGetPropertyData(dev, &pcs, 0, NULL, &bs, back) == noErr) {
        printf("stereo pair in force: channels %u and %u\n",
               (unsigned)back[0], (unsigned)back[1]);
    }

    // Read it back so a silent failure is visible.
    AudioDeviceID check = kAudioObjectUnknown;
    UInt32 cs = sizeof(check);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &out, 0, NULL, &cs, &check) == noErr) {
        printf("default output is now device id %u%s\n", (unsigned)check,
               check == dev ? " (the n12)" : " (NOT the n12 - something overrode it)");
    }
    return 0;
}
