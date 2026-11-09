/*
 * sysplay.c - play a tone through the DEFAULT output unit, like afplay and
 * every app do, and report which device that unit actually uses.
 *
 * Why this exists: our own tools drive the n12 device directly and the desk
 * hears them. The system's normal output path does not seem to reach the desk,
 * even though the "default output device" reads back as the n12. This opens the
 * very same AudioUnit the system uses for default output, asks it which device
 * it has chosen, and plays through it - so we can see whether the HAL agrees
 * that the default output is the n12, or whether it quietly keeps another
 * device.
 *
 * It changes nothing.
 *
 * Build:
 *     clang -Wall -O2 -o sysplay sysplay.c \
 *           -framework AudioUnit -framework CoreAudio -framework CoreFoundation
 *
 * Run:
 *     ./sysplay            # report the device, then play a 3 s tone
 *     ./sysplay --report   # report only, do not play
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

static void device_name(AudioDeviceID dev, char *buf, size_t n)
{
    CFStringRef name = NULL;
    UInt32 ns = sizeof(name);
    AudioObjectPropertyAddress a = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &ns, &name) == noErr && name) {
        CFStringGetCString(name, buf, n, kCFStringEncodingUTF8);
        CFRelease(name);
    } else {
        snprintf(buf, n, "(unknown)");
    }
}

/* The tone generator state, fixed so the callback just advances phase.
 * g_all: put the tone in every channel (like n12_play does). Otherwise it goes
 * into channels 7 and 8 only - which are the desk's input channels 1-2. */
static double g_phase = 0.0;
static double g_freq  = 1000.0;
static int    g_all   = 0;
static int    g_ch    = 16;    // client channel count: 16 for --all, 2 for --stereo

/* --- optional file playback (--file=PATH) --------------------------------
 * A 16-bit PCM WAV, decoded to interleaved float stereo and fed through the
 * same default output unit. This exists because the AudioUnit path reaches the
 * desk while the AudioQueue path (afplay, QuickTime) does not, so this is the
 * working way to play a file "the way an app would". */
static float   *g_pcm;
static unsigned g_pcm_frames, g_pcm_pos;
static int      g_have_file;

/* Minimal WAV reader: RIFF/WAVE, fmt, data; 16-bit PCM, mono or stereo. */
static int load_wav(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("cannot open %s\n", path); return -1; }
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, fp) != 12 ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        printf("not a RIFF/WAVE file\n"); fclose(fp); return -1;
    }
    int channels = 0, bits = 0;
    unsigned data_len = 0, rate = 0;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, fp) != 8) break;
        unsigned len = (unsigned)ck[4] | ((unsigned)ck[5] << 8)
                     | ((unsigned)ck[6] << 16) | ((unsigned)ck[7] << 24);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char f[16] = {0};
            unsigned take = len < 16 ? len : 16;
            fread(f, 1, take, fp);
            channels = f[2] | (f[3] << 8);
            rate = (unsigned)f[4] | ((unsigned)f[5] << 8)
                 | ((unsigned)f[6] << 16) | ((unsigned)f[7] << 24);
            bits = f[14] | (f[15] << 8);
            if (len > take) fseek(fp, (long)(len - take), SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            data_len = len;
            break;
        } else {
            fseek(fp, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    if ((channels != 1 && channels != 2) || bits != 16) {
        printf("only 16-bit PCM mono/stereo WAV is supported\n");
        fclose(fp); return -1;
    }
    unsigned frames = data_len / (2u * (unsigned)channels);
    short *buf = malloc((size_t)data_len + 2);
    unsigned got = buf ? (unsigned)fread(buf, 2, (size_t)frames * channels, fp) : 0;
    fclose(fp);
    frames = channels ? got / (unsigned)channels : 0;
    g_pcm = malloc(sizeof(float) * (size_t)frames * 2);
    if (g_pcm == NULL) { free(buf); return -1; }
    for (unsigned i = 0; i < frames; i++) {
        float l = buf[(size_t)i * channels] / 32768.0f;
        float r = (channels == 2) ? buf[(size_t)i * 2 + 1] / 32768.0f : l;
        g_pcm[(size_t)i * 2]     = l;
        g_pcm[(size_t)i * 2 + 1] = r;
    }
    free(buf);
    g_pcm_frames = frames;
    g_pcm_pos = 0;
    g_have_file = 1;
    printf("loaded %s: %u Hz, %d ch, %u frames (%.1f s)\n",
           path, rate, channels, frames, rate ? (double)frames / rate : 0.0);
    return 0;
}

static OSStatus render_cb(void *inRefCon, AudioUnitRenderActionFlags *flags,
                          const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                          AudioBufferList *io)
{
    (void)inRefCon; (void)flags; (void)ts; (void)bus; (void)frames;
    // One interleaved buffer of g_ch-channel float frames.
    float *f = io->mBuffers[0].mData;

    // File mode: play the loaded WAV. Left goes on even channels, right on odd,
    // so --stereo (g_ch == 2) is a plain stereo signal.
    if (g_have_file) {
        UInt32 n = io->mBuffers[0].mDataByteSize / (g_ch * sizeof(float));
        for (UInt32 i = 0; i < n; i++) {
            unsigned idx = g_pcm_pos + i;
            float l = 0.0f, r = 0.0f;
            if (idx < g_pcm_frames) {
                l = g_pcm[(size_t)idx * 2];
                r = g_pcm[(size_t)idx * 2 + 1];
            }
            for (int c = 0; c < g_ch; c++) {
                f[i * g_ch + c] = (c & 1) ? r : l;
            }
        }
        g_pcm_pos += n;
        return noErr;
    }
    UInt32 n = io->mBuffers[0].mDataByteSize / (g_ch * sizeof(float));
    for (UInt32 i = 0; i < n; i++) {
        float s = (float)(0.3 * sin(g_phase));
        g_phase += 2.0 * M_PI * g_freq / 44100.0;
        if (g_phase > 2.0 * M_PI) g_phase -= 2.0 * M_PI;
        for (int c = 0; c < g_ch; c++) {
            // 16-channel mode: defaults to channels 7-8 unless --all.
            // 2-channel mode: both channels (a normal stereo signal).
            int on = (g_ch == 2) ? 1 : (g_all || c == 6 || c == 7);
            f[i * g_ch + c] = on ? s : 0.0f;
        }
    }
    return noErr;
}

int main(int argc, char **argv)
{
    int report_only = (argc > 1 && strcmp(argv[1], "--report") == 0);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) g_all = 1;
        if (strcmp(argv[i], "--stereo") == 0) g_ch = 2;
        if (strncmp(argv[i], "--file=", 7) == 0) {
            if (load_wav(argv[i] + 7) != 0) return 1;
            g_ch = 2;               // a file is a plain stereo signal
        }
    }

    // What the system says the default output is.
    AudioDeviceID def = kAudioObjectUnknown;
    UInt32 ds = sizeof(def);
    AudioObjectPropertyAddress da = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &ds, &def);
    char dn[256] = "";
    device_name(def, dn, sizeof(dn));
    printf("kAudioHardwarePropertyDefaultOutputDevice -> id %u \"%s\"\n",
           (unsigned)def, dn);

    // Now open the default output unit, which is what apps use.
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (comp == NULL) { printf("no default output unit\n"); return 1; }

    AudioUnit unit = NULL;
    OSStatus st = AudioComponentInstanceNew(comp, &unit);
    if (st != noErr) { printf("could not create the unit: %d\n", (int)st); return 1; }

    // Which device did the unit pick?
    AudioDeviceID used = kAudioObjectUnknown;
    UInt32 us = sizeof(used);
    if (AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global, 0, &used, &us) == noErr) {
        char un[256] = "";
        device_name(used, un, sizeof(un));
        printf("default output unit is using        -> id %u \"%s\"\n",
               (unsigned)used, un);
        printf("%s\n", (used == def) ? "  (they agree)"
                                     : "  *** THEY DISAGREE - the unit is NOT on the reported default ***");
    }

    if (report_only) {
        AudioComponentInstanceDispose(unit);
        return 0;
    }

    // 16-channel float, 44100, matching the device. The tone goes into channels
    // 7 and 8 unless --all is given.
    AudioStreamBasicDescription fmt = {
        .mSampleRate = 44100, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = (UInt32)g_ch * 4, .mFramesPerPacket = 1,
        .mBytesPerFrame = (UInt32)g_ch * 4,
        .mChannelsPerFrame = (UInt32)g_ch, .mBitsPerChannel = 32,
    };
    AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));

    AURenderCallbackStruct cb = { .inputProc = render_cb, .inputProcRefCon = NULL };
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &cb, sizeof(cb));

    st = AudioUnitInitialize(unit);
    if (st != noErr) { printf("AudioUnitInitialize failed: %d\n", (int)st); return 1; }
    st = AudioOutputUnitStart(unit);
    if (st != noErr) { printf("AudioOutputUnitStart failed: %d\n", (int)st); return 1; }

    int secs = 3;
    if (g_have_file) {
        secs = (int)(g_pcm_frames / 44100u) + 1;    // play the file's length
        printf("playing the file through the default output unit (stereo)...\n");
    } else {
        printf("playing a 3 s tone through the default output unit (%s)...\n",
               g_ch == 2 ? "stereo (2 channels)"
                         : (g_all ? "all 16 channels" : "channels 7-8 only"));
    }
    usleep((useconds_t)secs * 1000 * 1000);

    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    printf("done\n");
    return 0;
}
