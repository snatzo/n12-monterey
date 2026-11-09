/*
 * n12_watch.c - report which of the n12's 16 output channels actually carry
 * audio.
 *
 * Why this exists: everything else - the default output device, the stereo
 * channel pair, the desk's channels - has been reasoned about indirectly, and
 * the reasoning keeps contradicting the ear. This measures it. It attaches an
 * IOProc to the n12 output device, and for a few seconds reports the peak level
 * it sees on each of the 16 channels.
 *
 * Because CoreAudio chains IOProcs on the same device, an IOProc attached while
 * another client (afplay, QuickTime, the system) is playing sees that client's
 * output in its buffer. This tool therefore shows which physical channels the
 * system's stereo is actually landing on - no desk, no guessing.
 *
 * It does not modify the buffer, just observes.
 *
 * Build:
 *     clang -Wall -O2 -o n12_watch n12_watch.c \
 *           -framework IOKit -framework CoreAudio -framework CoreFoundation
 *
 * Run (device up, something playing):
 *     ./n12_watch 5        # watch for 5 seconds
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#define N12_GUID 0x00a0de0000011013ULL
#if defined(MAC_OS_VERSION_12_0) && MAC_OS_VERSION_12_0
#define YAM_IO_PORT kIOMainPortDefault
#else
#define YAM_IO_PORT kIOMasterPortDefault
#endif

static double g_peak[16];
static unsigned long g_calls;

static void zero_peaks(void)
{
    for (int i = 0; i < 16; i++) g_peak[i] = 0.0;
    g_calls = 0;
}

/* Called periodically by the device. outData holds the output buffers; for this
 * device that is 16 separate one-channel float buffers, one per channel. We read
 * them and keep the running peak, and do NOT write, so we pass the audio
 * through untouched. */
static OSStatus io_proc(AudioDeviceID dev, const AudioTimeStamp *now,
                        const AudioBufferList *inData, const AudioTimeStamp *inTime,
                        AudioBufferList *outData, const AudioTimeStamp *outTime,
                        void *ctx)
{
    (void)dev; (void)now; (void)inData; (void)inTime; (void)outTime; (void)ctx;
    g_calls++;
    if (outData == NULL) return noErr;
    for (UInt32 b = 0; b < outData->mNumberBuffers && b < 16; b++) {
        float *f = outData->mBuffers[b].mData;
        UInt32 n = outData->mBuffers[b].mDataByteSize / sizeof(float);
        for (UInt32 i = 0; i < n; i++) {
            double a = fabs((double)f[i]);
            if (a > g_peak[b]) g_peak[b] = a;
        }
    }
    return noErr;
}

int main(int argc, char **argv)
{
    double seconds = (argc > 1) ? atof(argv[1]) : 5.0;

    // Find the n12 CoreAudio device by name.
    AudioDeviceID dev = kAudioObjectUnknown;
    AudioObjectPropertyAddress la = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &la, 0, NULL, &sz);
    int nd = sz / sizeof(AudioDeviceID);
    AudioDeviceID *ids = calloc(nd ? nd : 1, sizeof(AudioDeviceID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &la, 0, NULL, &sz, ids);
    for (int i = 0; i < nd; i++) {
        CFStringRef name = NULL; UInt32 ns = sizeof(name);
        AudioObjectPropertyAddress na = { kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &ns, &name) == noErr && name) {
            char buf[256] = ""; CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);
            if (strstr(buf, "n12")) { dev = ids[i]; printf("device: \"%s\" id %u\n", buf, (unsigned)dev); break; }
        }
    }
    free(ids);
    if (dev == kAudioObjectUnknown) {
        printf("no n12 device - start it with ../n12_start.sh\n");
        return 1;
    }

    zero_peaks();
    AudioDeviceIOProcID procID = NULL;
    if (AudioDeviceCreateIOProcID(dev, io_proc, NULL, &procID) != noErr) {
        printf("could not create the IOProc\n");
        return 1;
    }
    if (AudioDeviceStart(dev, procID) != noErr) {
        printf("could not start the device\n");
        AudioDeviceDestroyIOProcID(dev, procID);
        return 1;
    }

    printf("watching the device output for %.0f s...\n", seconds);
    for (int i = 0; i < (int)(seconds * 10); i++) usleep(100 * 1000);

    AudioDeviceStop(dev, procID);
    AudioDeviceDestroyIOProcID(dev, procID);

    printf("\nIOProc calls: %lu\n", g_calls);
    printf("channels carrying audio (peak):\n");
    int any = 0;
    for (int c = 0; c < 16; c++) {
        if (g_peak[c] > 1e-5) { printf("  ch %2d  peak %.5f\n", c, g_peak[c]); any++; }
    }
    if (!any) printf("  (none - nothing is playing, or the client writes after us)\n");
    return 0;
}
