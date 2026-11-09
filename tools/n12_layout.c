/*
 * n12_layout.c - point the n12 device's stereo pair at channels 1-2.
 *
 * WHY THIS EXISTS
 *   The Yamaha driver publishes the n12 with a channel *layout* that marks
 *   channels 7 and 8 as Left/Right. CoreAudio has two places to learn "where is
 *   stereo":
 *
 *     * kAudioDevicePropertyPreferredChannelsForStereo - the "preferred stereo
 *       pair". The AudioUnit output path (Logic, VLC, Safari/YouTube, and
 *       tools/sysplay) honours this.
 *     * kAudioDevicePropertyPreferredChannelLayout - the channel layout.
 *       The AudioQueue path (afplay, QuickTime Player, Music) honours *this*.
 *
 *   In n12_agent's Mojave channel order the main bus is device channels 1-2 and
 *   the desk strips start at channel 3. So the AudioUnit path lands on the main
 *   bus, but the AudioQueue path followed the layout to channels 7-8, which are
 *   desk channels 5-6 (strips) - hence "QuickTime/Music are silent".
 *
 *   Re-pointing the layout so Left=1 and Right=2 makes the AudioQueue path land
 *   on the main bus too, matching the AudioUnit path. The change is not
 *   persistent: the layout is rebuilt with the device, so this must run after
 *   n12_agent starts the device (n12_on.sh does that for you).
 *
 * Build:
 *     clang -Wall -O2 -o n12_layout n12_layout.c \
 *           -framework CoreAudio -framework CoreFoundation
 *
 * Run (device up):
 *     ./n12_layout                 # move Left/Right to channels 1-2
 *     ./n12_layout --channels=1,2  # same, explicit
 *     ./n12_layout --device=n12    # pick a different device by name
 *
 * Exit status: 0 if the layout now has Left/Right on the requested channels.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

/* The channel layout is a variable-length structure: a header plus one
 * "description" per channel. kAudioChannelLayoutTag_UseChannelDescriptions
 * (0) means "read the per-channel labels that follow". */
typedef struct {
    AudioChannelLayout hdr;
    AudioChannelDescription desc[64];
} layout_buf_t;

/* Find a CoreAudio device whose name contains `want`; 0 if none. */
static AudioDeviceID find_device(const char *want)
{
    AudioObjectPropertyAddress a = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size);
    int n = (int)(size / sizeof(AudioDeviceID));
    AudioDeviceID *ids = calloc(n ? n : 1, sizeof(AudioDeviceID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);

    AudioDeviceID found = 0;
    for (int i = 0; i < n && !found; i++) {
        CFStringRef name = NULL;
        UInt32 ns = sizeof(name);
        AudioObjectPropertyAddress na = {
            kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &ns, &name) != noErr || !name) {
            continue;
        }
        char buf[256] = "";
        CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(name);
        if (strcasestr(buf, want)) {
            found = ids[i];
        }
    }
    free(ids);
    return found;
}

int main(int argc, char **argv)
{
    const char *want = "n12";
    unsigned want_l = 1, want_r = 2;   // 1-based channels, as CoreAudio numbers them

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--device=", 9) == 0) {
            want = argv[i] + 9;
        } else if (strncmp(argv[i], "--channels=", 11) == 0) {
            unsigned l = 0, r = 0;
            if (sscanf(argv[i] + 11, "%u,%u", &l, &r) == 2 && l && r) {
                want_l = l; want_r = r;
            }
        } else {
            fprintf(stderr, "usage: %s [--device=NAME] [--channels=L,R]\n", argv[0]);
            return 2;
        }
    }

    AudioDeviceID dev = find_device(want);
    if (!dev) {
        fprintf(stderr, "no device matching \"%s\"\n", want);
        return 1;
    }

    AudioObjectPropertyAddress la = {
        kAudioDevicePropertyPreferredChannelLayout,
        kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };

    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &la, 0, NULL, &size) != noErr || size == 0) {
        fprintf(stderr, "device has no channel layout\n");
        return 1;
    }
    layout_buf_t *lay = calloc(1, size < sizeof(*lay) ? sizeof(*lay) : size);
    if (AudioObjectGetPropertyData(dev, &la, 0, NULL, &size, lay) != noErr) {
        fprintf(stderr, "could not read the channel layout\n");
        return 1;
    }
    if (lay->hdr.mNumberChannelDescriptions < 2) {
        fprintf(stderr, "channel layout has too few channels\n");
        return 1;
    }

    /* Find the existing Left and Right (so we can clear their old slots). */
    UInt32 old_l = 0xffffffff, old_r = 0xffffffff;
    for (UInt32 c = 0; c < lay->hdr.mNumberChannelDescriptions; c++) {
        UInt32 lab = lay->hdr.mChannelDescriptions[c].mChannelLabel;
        if (lab == kAudioChannelLabel_Left  && old_l == 0xffffffff) old_l = c;
        if (lab == kAudioChannelLabel_Right && old_r == 0xffffffff) old_r = c;
    }
    if (old_l == 0xffffffff || old_r == 0xffffffff) {
        fprintf(stderr, "layout has no Left/Right pair to move\n");
        return 1;
    }

    /* Remember the descriptions we are about to overwrite, so we can put them
     * back where the old pair was (keeps the layout otherwise intact). */
    AudioChannelDescription keep0 = lay->hdr.mChannelDescriptions[want_l - 1];
    AudioChannelDescription keep1 = lay->hdr.mChannelDescriptions[want_r - 1];

    lay->hdr.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
    lay->hdr.mChannelDescriptions[want_l - 1].mChannelLabel = kAudioChannelLabel_Left;
    lay->hdr.mChannelDescriptions[want_r - 1].mChannelLabel = kAudioChannelLabel_Right;
    if (old_l != want_l - 1) {
        lay->hdr.mChannelDescriptions[old_l] = keep0;
    }
    if (old_r != want_r - 1) {
        lay->hdr.mChannelDescriptions[old_r] = keep1;
    }

    OSStatus st = AudioObjectSetPropertyData(dev, &la, 0, NULL, size, lay);
    if (st != noErr) {
        fprintf(stderr, "could not set the channel layout: %d\n", (int)st);
        return 1;
    }

    printf("n12 stereo pair -> device channels %u and %u (was %u and %u)\n",
           want_l, want_r, old_l + 1, old_r + 1);
    free(lay);
    return 0;
}
