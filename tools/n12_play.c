/*
 * n12_play.c - play a test tone straight to the n12's CoreAudio output device.
 *
 * Why this exists: to prove the audio path we need signal going INTO the desk.
 * Routing system audio via Audio MIDI Setup is fiddly (the device shows greyed
 * unless the driver is driven a particular way); this opens the device by name
 * and writes a tone to it directly, so the Mac -> n12 output (DAW IN) -> desk ->
 * n12 input (DAW OUT) -> capture loop can be tested without any UI.
 *
 * Build:
 *     clang -Wall -O2 -o n12_play n12_play.c \
 *           -framework CoreAudio -framework CoreFoundation
 *
 * Run:
 *     ./n12_play --seconds=10              # 1 kHz tone on every output channel
 *     ./n12_play --seconds=10 --freq=440 --channel=0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

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
        UInt32 s = sizeof(name);
        AudioObjectPropertyAddress na = {
            kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &s, &name) == noErr && name) {
            char buf[256] = "";
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);
            if (strcasestr(buf, want) != NULL) { found = ids[i]; break; }
        }
    }
    free(ids);
    return found;
}

static double g_freq = 1000.0;
static double g_phase = 0.0;
static int g_channel = -1;      // -1 = all channels

/* --- optional file playback --------------------------------------------- */
/* With --file=PATH the whole WAV is decoded to interleaved float stereo here,
 * and the callback plays it into two chosen device channels instead of a tone.
 * Those channels are the point of this tool: ordinary stereo from the system
 * lands on the device's first two channels, which on this desk feed no input
 * channel, so the file has to be written to channels 7-8 (which are desk input
 * channels 1-2) to be heard. */
static float   *g_pcm;
static unsigned g_pcm_frames;
static unsigned g_pcm_pos;
static unsigned g_rate;             // sample rate of the loaded file
static int      g_have_file;
static int      g_lo = -1, g_hi = -1;  // -1 = write the mono mix to all channels

/* Decode a 16-bit PCM WAV (mono or stereo) to g_pcm. Returns 0 on success.
 * Only the chunks we need are read; anything else is skipped. */
static int load_wav(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("cannot open %s\n", path); return -1; }
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, fp) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        printf("not a RIFF/WAVE file\n"); fclose(fp); return -1;
    }
    int channels = 0, bits = 0;
    unsigned data_len = 0, rate = 0;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, fp) != 8) break;
        unsigned len = (unsigned)ck[4] | ((unsigned)ck[5] << 8)
                     | ((unsigned)ck[6] << 16) | ((unsigned)ck[7] << 24);
        if (memcmp(ck, "fmt ", 4) == 0) {
            unsigned char f[16] = {0};
            unsigned take = len < 16 ? len : 16;
            if (fread(f, 1, take, fp) != take) break;
            channels = f[2] | (f[3] << 8);
            rate = (unsigned)f[4] | ((unsigned)f[5] << 8)
                 | ((unsigned)f[6] << 16) | ((unsigned)f[7] << 24);
            bits = f[14] | (f[15] << 8);
            if (len > take) fseek(fp, len - take, SEEK_CUR);
        } else if (memcmp(ck, "data", 4) == 0) {
            data_len = len;
            break;                       // samples start right here
        } else {
            fseek(fp, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    if (channels != 1 && channels != 2) {
        printf("only mono or stereo WAV is supported (file has %d)\n", channels);
        fclose(fp); return -1;
    }
    if (bits != 16) {
        printf("only 16-bit PCM WAV is supported (file has %d-bit)\n", bits);
        fclose(fp); return -1;
    }
    unsigned frames = data_len / (2u * (unsigned)channels);
    short *buf = malloc((size_t)data_len + 2);
    if (buf == NULL) { fclose(fp); return -1; }
    unsigned got = (unsigned)fread(buf, 2, (size_t)frames * channels, fp);
    fclose(fp);
    frames = channels ? got / (unsigned)channels : 0;

    g_pcm = malloc(sizeof(float) * (size_t)frames * 2);
    if (g_pcm == NULL) { free(buf); return -1; }
    for (unsigned i = 0; i < frames; i++) {
        float l = buf[(size_t)i * channels] / 32768.0f;
        float r = (channels == 2) ? buf[(size_t)i * 2 + 1] / 32768.0f : l;
        g_pcm[(size_t)i * 2] = l;
        g_pcm[(size_t)i * 2 + 1] = r;
    }
    free(buf);
    g_pcm_frames = frames;
    g_pcm_pos = 0;
    g_rate = rate;
    g_have_file = 1;
    printf("loaded %s: %u Hz, %d ch, %u frames (%.1f s)\n",
           path, rate, channels, frames, rate ? (double)frames / rate : 0.0);
    return 0;
}

/* CoreAudio I/O proc: fill every output buffer with a sine. */
static OSStatus io_proc(AudioDeviceID dev, const AudioTimeStamp *now,
                        const AudioBufferList *inData, const AudioTimeStamp *inTime,
                        AudioBufferList *outData, const AudioTimeStamp *outTime,
                        void *ctx)
{
    (void)dev; (void)now; (void)inData; (void)inTime; (void)outTime; (void)ctx;

    // File mode: play the loaded WAV into the two chosen channels, silence
    // everywhere else.
    if (g_have_file) {
        UInt32 frames = outData->mBuffers[0].mDataByteSize / sizeof(float);
        for (UInt32 i = 0; i < frames; i++) {
            unsigned idx = g_pcm_pos + i;
            float l = 0.0f, r = 0.0f;
            if (idx < g_pcm_frames) { l = g_pcm[(size_t)idx * 2]; r = g_pcm[(size_t)idx * 2 + 1]; }
            for (UInt32 b = 0; b < outData->mNumberBuffers; b++) {
                float *f = outData->mBuffers[b].mData;
                UInt32 nb = outData->mBuffers[b].mDataByteSize / sizeof(float);
                // g_lo < 0 means "play the mono mix everywhere", which is the
                // safe default: it reaches whatever desk channels are set up,
                // even though the Mac -> desk channel numbering has not been
                // pinned down. --to=L,R writes a real stereo pair instead.
                float v = (g_lo < 0) ? 0.5f * (l + r)
                                     : (((int)b == g_lo) ? l : ((int)b == g_hi) ? r : 0.0f);
                if (i < nb) f[i] = v;
            }
        }
        g_pcm_pos += frames;
        return noErr;
    }

    double sr = 44100.0;
    AudioObjectPropertyAddress sa = {
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 ss = sizeof(sr);
    AudioObjectGetPropertyData(dev, &sa, 0, NULL, &ss, &sr);

    for (UInt32 b = 0; b < outData->mNumberBuffers; b++) {
        // Each buffer is one channel for a non-interleaved device.
        if (g_channel >= 0 && (int)b != g_channel) {
            memset(outData->mBuffers[b].mData, 0, outData->mBuffers[b].mDataByteSize);
            continue;
        }
        float *f = outData->mBuffers[b].mData;
        UInt32 frames = outData->mBuffers[b].mDataByteSize / sizeof(float);
        for (UInt32 i = 0; i < frames; i++) {
            f[i] = (float)(0.2 * sin(g_phase));
            g_phase += 2.0 * M_PI * g_freq / sr;
            if (g_phase > 2.0 * M_PI) g_phase -= 2.0 * M_PI;
        }
    }
    return noErr;
}

int main(int argc, char **argv)
{
    int seconds = 5;
    int buffer = 0;                 // 0 = leave the device's own buffer size alone
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--seconds=", 10) == 0) seconds = atoi(argv[i] + 10);
        else if (strncmp(argv[i], "--freq=", 7) == 0) g_freq = atof(argv[i] + 7);
        else if (strncmp(argv[i], "--channel=", 10) == 0) g_channel = atoi(argv[i] + 10);
        else if (strncmp(argv[i], "--buffer=", 9) == 0) buffer = atoi(argv[i] + 9);
        else if (strncmp(argv[i], "--file=", 7) == 0) { if (load_wav(argv[i] + 7) != 0) return 1; }
        else if (strncmp(argv[i], "--to=", 5) == 0) sscanf(argv[i] + 5, "%d,%d", &g_lo, &g_hi);
    }

    if (g_have_file) {
        // Play the whole file (plus half a second), unless --seconds was given.
        int file_secs = g_rate ? (int)(g_pcm_frames / g_rate) + 2 : seconds;
        for (int i = 1; i < argc; i++) {
            if (strncmp(argv[i], "--seconds=", 10) == 0) file_secs = atoi(argv[i] + 10);
        }
        seconds = file_secs;
    }

    AudioDeviceID dev = find_device_named("n12");
    if (dev == kAudioObjectUnknown) {
        printf("no CoreAudio device with \"n12\" in its name\n");
        printf("(start tools/n12_agent first - the device only exists while it runs)\n");
        return 1;
    }
    printf("n12 device id %u - playing %.0f Hz for %d s%s\n",
           (unsigned)dev, g_freq, seconds,
           g_channel < 0 ? " (all channels)" : "");

    // Optionally change how many frames the device asks for per callback.
    // This is the experiment for the pulsating tone: if the pulsing rate follows
    // this number, the artifact is per-buffer (a glitch at every buffer
    // boundary); if it does not, it is a slower beat between two clocks.
    if (buffer > 0) {
        UInt32 bf = (UInt32)buffer;
        AudioObjectPropertyAddress ba = {
            kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        OSStatus br = AudioObjectSetPropertyData(dev, &ba, 0, NULL, sizeof(bf), &bf);
        printf("buffer frame size -> %d (status %d)\n", buffer, (int)br);
    }

    AudioDeviceIOProcID procID = NULL;
    OSStatus st = AudioDeviceCreateIOProcID(dev, io_proc, NULL, &procID);
    if (st != noErr) { printf("AudioDeviceCreateIOProcID failed: %d\n", (int)st); return 1; }
    st = AudioDeviceStart(dev, procID);
    if (st != noErr) { printf("AudioDeviceStart failed: %d\n", (int)st);
                       AudioDeviceDestroyIOProcID(dev, procID); return 1; }

    sleep(seconds);

    AudioDeviceStop(dev, procID);
    AudioDeviceDestroyIOProcID(dev, procID);
    printf("done\n");
    return 0;
}
