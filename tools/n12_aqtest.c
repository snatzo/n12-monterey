/*
 * n12_aqtest.c - play through an AudioQueue and report every return code.
 *
 * Why this exists: afplay and QuickTime (both AudioQueue-based) produce no sound
 * on the n12 while a directly-configured AudioUnit does. Rather than keep
 * guessing at the cause, this does the smallest possible AudioQueue playback and
 * prints the OSStatus of every call, so "silent" becomes a specific error.
 *
 * Build:
 *     clang -Wall -O2 -o n12_aqtest n12_aqtest.c \
 *           -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
 *
 * Run (device up):
 *     ./n12_aqtest            # default output device
 *     ./n12_aqtest n12        # force the queue onto the named device by UID
 *     ./n12_aqtest "Built-in Output"   # control: a device known to work
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

/* Print an OSStatus as a number and, when it is a four-character code, as text
 * (Apple error codes like '!dat' are far easier to recognise than 0x21646174). */
static void show(const char *what, OSStatus st)
{
    char cc[5] = {0};
    unsigned v = (unsigned)st;
    if (v >= 0x20202020 && v <= 0x7e7e7e7e) {
        cc[0] = (v >> 24) & 0xff; cc[1] = (v >> 16) & 0xff;
        cc[2] = (v >> 8) & 0xff;  cc[3] = v & 0xff;
    }
    if (cc[0])
        printf("  %-34s -> %d (0x%08x) '%s'\n", what, (int)st, v, cc);
    else
        printf("  %-34s -> %d (0x%08x)\n", what, (int)st, v);
}

/* Find the UID string of the audio device whose name contains `want`. */
static int device_uid(const char *want, char *uid, size_t n)
{
    AudioObjectPropertyAddress la = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &la, 0, NULL, &sz);
    int nd = sz / sizeof(AudioDeviceID);
    AudioDeviceID *ids = calloc(nd ? nd : 1, sizeof(AudioDeviceID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &la, 0, NULL, &sz, ids);
    int found = 0;
    for (int i = 0; i < nd && !found; i++) {
        CFStringRef name = NULL; UInt32 ns = sizeof(name);
        AudioObjectPropertyAddress na = { kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &ns, &name) != noErr || !name)
            continue;
        char buf[256] = ""; CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(name);
        if (strcasestr(buf, want) == NULL) continue;
        CFStringRef u = NULL; UInt32 us = sizeof(u);
        AudioObjectPropertyAddress ua = { kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &ua, 0, NULL, &us, &u) == noErr && u) {
            CFStringGetCString(u, uid, n, kCFStringEncodingUTF8);
            CFRelease(u);
            printf("device: \"%s\"  uid=%s\n", buf, uid);
            found = 1;
        }
    }
    free(ids);
    return found;
}

static void cb(void *user, AudioQueueRef q, AudioQueueBufferRef buf)
{
    (void)user; (void)q;
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);   // keep it fed
}

int main(int argc, char **argv)
{
    printf("n12_aqtest - AudioQueue playback with error codes\n\n");

    // A plain CD-style format, exactly what an app would use.
    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate       = 44100;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 16;
    fmt.mChannelsPerFrame = 2;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = 4;
    fmt.mBytesPerPacket   = 4;

    AudioQueueRef q = NULL;
    show("AudioQueueNewOutput", AudioQueueNewOutput(&fmt, cb, NULL, NULL, NULL, 0, &q));
    if (q == NULL) { printf("no queue created - stopping\n"); return 1; }

    // Optionally steer the queue at a specific device by UID.
    if (argc > 1) {
        char uid[256] = "";
        if (device_uid(argv[1], uid, sizeof(uid))) {
            CFStringRef u = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
            show("set kAudioQueueProperty_CurrentDevice",
                 AudioQueueSetProperty(q, kAudioQueueProperty_CurrentDevice, &u, sizeof(u)));
            if (u) CFRelease(u);
        } else {
            printf("no device matching \"%s\"\n", argv[1]);
        }
    }

    // Report which device the queue ended up on.
    CFStringRef cur = NULL; UInt32 cs = sizeof(cur);
    if (AudioQueueGetProperty(q, kAudioQueueProperty_CurrentDevice, &cur, &cs) == noErr && cur) {
        char b[256] = ""; CFStringGetCString(cur, b, sizeof(b), kCFStringEncodingUTF8);
        printf("  queue is on device uid=%s\n", b);
        CFRelease(cur);
    }

    AudioQueueBufferRef buf = NULL;
    show("AudioQueueAllocateBuffer", AudioQueueAllocateBuffer(q, 44100, &buf));
    if (buf == NULL) { AudioQueueDispose(q, true); return 1; }

    // Fill with a short 1 kHz tone so there is something definite to hear.
    short *s = (short *)buf->mAudioData;
    int frames = buf->mAudioDataBytesCapacity / 4;
    for (int i = 0; i < frames; i++) {
        short v = (short)(0.3 * 32767 * sin(2.0 * M_PI * 1000 * i / 44100.0));
        s[i * 2] = v; s[i * 2 + 1] = v;
    }
    buf->mAudioDataByteSize = frames * 4;

    show("AudioQueueEnqueueBuffer", AudioQueueEnqueueBuffer(q, buf, 0, NULL));

    OSStatus st = AudioQueueStart(q, NULL);
    show("AudioQueueStart", st);
    if (st == noErr) {
        printf("  queue running - listen for a 1 kHz tone (3 s)...\n");
        sleep(3);
    }

    AudioQueueFlush(q);
    show("AudioQueueStop", AudioQueueStop(q, true));
    show("AudioQueueDispose", AudioQueueDispose(q, true));
    printf("\n  0 = success. Any '!xxx' code is the exact reason the queue failed.\n");
    return 0;
}
