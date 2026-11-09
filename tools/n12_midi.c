/*
 * n12_midi.c - talk to the n12 over CoreMIDI.
 *
 * Why this exists: every FireWire-register route to the device is closed
 * (docs/HOW-IT-WORKS.md), and the async mailbox payload is unknown.
 * But the desk also has MIDI: the "Yamaha n Extension" talks to it over CoreMIDI
 * ("mLAN Network 1..16"), and the service manual drives the n8/n12 over MIDI
 * with short commands. MIDI is a host-writable channel that has never been
 * tried in this project.
 *
 * This tool lists the CoreMIDI ports, optionally sends a byte string to the
 * n12's destination, and prints everything it receives back, so we can find out
 * whether the device answers at all.
 *
 * Build:
 *     clang -Wall -O2 -o n12_midi n12_midi.c \
 *           -framework CoreMIDI -framework CoreFoundation
 *
 * Run:
 *     ./n12_midi --list
 *     ./n12_midi --send "F0 43 10 4C 00 00 7F F7" --seconds=3
 *     ./n12_midi --send "A0 47 00"   --seconds=3      # manual's loopback test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

static volatile sig_atomic_t g_stop;

static void on_sigint(int s) { (void)s; g_stop = 1; }

/* Print an endpoint's name, or "(unnamed)". */
static void print_endpoint(MIDIEndpointRef e)
{
    CFStringRef name = NULL;
    MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &name);
    if (name == NULL) {
        MIDIObjectGetStringProperty(e, kMIDIPropertyName, &name);
    }
    char buf[256] = "(unnamed)";
    if (name != NULL) {
        CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(name);
    }
    printf("  %s\n", buf);
}

/* A CoreMIDI read callback: hex-dump whatever arrives. */
static void read_proc(const MIDIPacketList *pktlist, void *refCon, void *srcConnRefCon)
{
    (void)refCon;
    (void)srcConnRefCon;
    const MIDIPacket *p = &pktlist->packet[0];
    for (int i = 0; i < pktlist->numPackets; i++) {
        printf("RX [");
        for (int j = 0; j < p->length; j++) {
            printf("%02X%s", p->data[j], j + 1 < p->length ? " " : "");
        }
        printf("]");
        if (p->length >= 6 && p->data[0] == 0xF0 && p->data[1] == 0x43) {
            printf("   (Yamaha SysEx)");
        } else if (p->length >= 3 && p->data[0] == 0xA0) {
            printf("   (A0 service-style)");
        }
        printf("\n");
        fflush(stdout);
        p = MIDIPacketNext(p);
    }
}

/* Parse a hex byte string (spaces allowed) into out; -1 on error. */
static int parse_hex(const char *s, uint8_t *out, int max)
{
    int n = 0, hi = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == ' ' || c == '\t' || c == ':') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        if ((n & 1) == 0) hi = v;
        else { if (n / 2 >= max) return -1; out[n / 2] = (uint8_t)((hi << 4) | v); }
        n++;
    }
    return (n & 1) ? -1 : n / 2;
}

/* Does this endpoint look like the desk? */
static int looks_like_n12(MIDIEndpointRef e)
{
    CFStringRef name = NULL;
    MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &name);
    if (name == NULL) return 0;
    char buf[256] = "";
    CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(name);
    return strcasestr(buf, "mlan") != NULL || strcasestr(buf, "n8") != NULL
        || strcasestr(buf, "n12") != NULL  || strcasestr(buf, "yamaha") != NULL
        || strcasestr(buf, "steinberg") != NULL;
}

int main(int argc, char **argv)
{
    int list_only = 0, seconds = 5;
    uint8_t send_buf[256];
    int send_len = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) list_only = 1;
        else if (strncmp(argv[i], "--seconds=", 10) == 0)
            seconds = (int)strtol(argv[i] + 10, NULL, 0);
        else if (strcmp(argv[i], "--send") == 0 && i + 1 < argc) {
            send_len = parse_hex(argv[++i], send_buf, (int)sizeof(send_buf));
            if (send_len < 0) { fprintf(stderr, "--send: bad hex\n"); return 1; }
        } else {
            fprintf(stderr, "usage: %s [--list] [--send HEX] [--seconds=N]\n", argv[0]);
            return 1;
        }
    }

    printf("n12_midi - CoreMIDI view of the n12\n\n");

    int nsrc = MIDIGetNumberOfSources();
    int ndst = MIDIGetNumberOfDestinations();
    printf("CoreMIDI sources (%d):\n", nsrc);
    for (int i = 0; i < nsrc; i++) print_endpoint(MIDIGetSource(i));
    printf("CoreMIDI destinations (%d):\n", ndst);
    for (int i = 0; i < ndst; i++) print_endpoint(MIDIGetDestination(i));

    if (list_only) return 0;

    MIDIClientRef client = 0;
    MIDIPortRef in = 0, out = 0;
    if (MIDIClientCreate(CFSTR("n12_midi"), NULL, NULL, &client) != noErr) {
        printf("MIDIClientCreate failed\n");
        return 1;
    }
    if (MIDIInputPortCreate(client, CFSTR("n12_midi in"), read_proc, NULL, &in) != noErr) {
        printf("MIDIInputPortCreate failed\n");
        return 1;
    }
    if (MIDIOutputPortCreate(client, CFSTR("n12_midi out"), &out) != noErr) {
        printf("MIDIOutputPortCreate failed\n");
        return 1;
    }

    // Connect to every desk-looking source (so a reply is seen) and remember
    // every desk-looking destination (we will send to all of them).
    int connected = 0, dsts[32], ndsts = 0;
    for (int i = 0; i < nsrc; i++) {
        MIDIEndpointRef s = MIDIGetSource(i);
        if (looks_like_n12(s)) {
            MIDIPortConnectSource(in, s, NULL);
            connected++;
        }
    }
    for (int i = 0; i < ndst && ndsts < 32; i++) {
        MIDIEndpointRef d = MIDIGetDestination(i);
        if (looks_like_n12(d)) { dsts[ndsts++] = i; }
    }
    printf("\nconnected %d desk source(s); %d desk destination(s) to send to\n",
           connected, ndsts);

    if (send_len > 0 && ndsts > 0) {
        MIDIPacketList pktlist;
        MIDIPacket *p = MIDIPacketListInit(&pktlist);
        p = MIDIPacketListAdd(&pktlist, sizeof(pktlist), p, 0, send_len, send_buf);
        if (p == NULL) {
            printf("packet too large\n");
        } else {
            printf("TX [");
            for (int j = 0; j < send_len; j++)
                printf("%02X%s", send_buf[j], j + 1 < send_len ? " " : "");
            printf("]\n");
            // Send to every desk destination, so whichever port the desk
            // listens on (see the names) receives it.
            for (int k = 0; k < ndsts; k++) {
                MIDISend(out, MIDIGetDestination(dsts[k]), &pktlist);
            }
        }
    } else if (send_len > 0) {
        printf("no desk destination found - nothing sent\n");
    }

    signal(SIGINT, on_sigint);
    printf("\nlistening for %d s...\n", seconds);
    for (int i = 0; i < seconds * 10 && !g_stop; i++) {
        usleep(100 * 1000);
    }
    printf("done\n");

    MIDIPortDispose(in);
    MIDIPortDispose(out);
    MIDIClientDispose(client);
    return 0;
}
