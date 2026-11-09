#!/usr/bin/env python3
"""
midparse.py - inspect a Standard MIDI File, with an eye on Yamaha SysEx.

Why this exists: Yamaha shipped the n8/n12 firmware as `N_FIRM_103.MID`, a
Standard MIDI File full of System Exclusive messages. The framing of those
messages - who they are addressed to, how the payload is chunked, and what the
payload contains - is evidence about the device's internal protocol, so it is
worth pulling apart.

Usage:
    ./midparse.py <file.mid>            # summary
    ./midparse.py <file.mid> --strings  # also scan the payload for ASCII runs
    ./midparse.py <file.mid> --dump N   # dump the first N sysex messages in full

References: SMF spec (MIDI 1.0, Standard MIDI Files), Yamaha SysEx
manufacturer ID 0x43.
"""

import re
import sys
from collections import Counter


def read_vlq(data: bytes, pos: int) -> tuple[int, int]:
    """
    Read a variable-length quantity (used for delta times and sysex lengths).

    VLQ bytes carry 7 bits each, most significant group first, and the last byte
    has bit 7 clear. Returns (value, new_position).
    """
    value = 0
    while True:
        if pos >= len(data):
            raise ValueError("VLQ ran off the end of the file")
        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, pos


def parse_smf(data: bytes):
    """Yield (delta, status, payload) for every track event in the file."""
    if data[:4] != b"MThd":
        raise ValueError("not a Standard MIDI File (no MThd)")
    header_len = int.from_bytes(data[4:8], "big")
    fmt = int.from_bytes(data[8:10], "big")
    ntrks = int.from_bytes(data[10:12], "big")
    division = int.from_bytes(data[12:14], "big")
    print(f"SMF format {fmt}, {ntrks} track(s), division {division}")

    # Header chunk may be longer than the 6 bytes we understand; skip it fully.
    pos = 8 + header_len
    for _ in range(ntrks):
        if data[pos:pos + 4] != b"MTrk":
            raise ValueError("expected MTrk chunk")
        track_len = int.from_bytes(data[pos + 4:pos + 8], "big")
        end = pos + 8 + track_len
        pos += 8
        running_status = None
        while pos < end:
            delta, pos = read_vlq(data, pos)
            status = data[pos]
            if status & 0x80:
                # A status byte; remember it for running status.
                running_status = status
                pos += 1
            else:
                # Data byte: running status - reuse the previous status byte.
                status = running_status
            if status is None:
                raise ValueError("data byte before any status byte")

            if status == 0xFF:
                # Meta event: type byte, then a VLQ length and that many bytes.
                meta_type = data[pos]
                pos += 1
                length, pos = read_vlq(data, pos)
                yield delta, 0xFF00 | meta_type, data[pos:pos + length]
                pos += length
            elif status in (0xF0, 0xF7):
                # SysEx: a VLQ length then the raw bytes (including the F7 end).
                length, pos = read_vlq(data, pos)
                yield delta, status, data[pos:pos + length]
                pos += length
            else:
                # Channel message: work out the data length from the status nibble.
                high = status & 0xF0
                ndata = 1 if high in (0xC0, 0xD0) else 2
                yield delta, status, data[pos:pos + ndata]
                pos += ndata
        pos = end  # in case of trailing padding


def main() -> None:
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    path = args[0]
    want_strings = "--strings" in args
    dump_n = 0
    if "--dump" in args:
        dump_n = int(args[args.index("--dump") + 1])

    data = open(path, "rb").read()
    print(f"file: {path} ({len(data)} bytes)")

    sysex = []
    meta = Counter()
    channels = Counter()
    for delta, status, payload in parse_smf(data):
        if status < 0x80:
            channels[status] += 1
        elif status >= 0xFF00:
            meta[status & 0xFF] += 1
        else:
            sysex.append((delta, status, payload))

    print(f"\ntrack events: {len(sysex)} sysex, "
          f"{sum(meta.values())} meta, {sum(channels.values())} channel")
    if meta:
        names = {0x51: "tempo", 0x58: "time-sig", 0x59: "key-sig",
                 0x2F: "end-of-track", 0x01: "text", 0x03: "track-name",
                 0x05: "lyric", 0x7F: "sequencer-specific"}
        print("meta events:")
        for t, c in sorted(meta.items()):
            print(f"  0x{t:02X} {names.get(t, '?'):<93} x{c}")

    if not sysex:
        return

    # Every Yamaha SysEx starts F0 43 ...; strip F0/F7 and look at the framing.
    print(f"\nsysex messages: {len(sysex)}")
    # Group by the first 6 bytes after F0 - that is where the address lives.
    prefixes = Counter()
    for _, _, p in sysex:
        prefixes[bytes(p[:6])] += 1
    print(f"distinct 6-byte prefixes after F0: {len(prefixes)}")
    for pre, c in prefixes.most_common(10):
        print(f"  {pre.hex(' '):<20} x{c}")

    total = sum(len(p) for _, _, p in sysex)
    sizes = [len(p) for _, _, p in sysex]
    print(f"\npayload: {total} bytes total, "
          f"{min(sizes)} min, {max(sizes)} max, "
          f"{total / len(sizes):.1f} mean per message")

    # Show the first couple of messages in full - they carry the handshake.
    print("\nfirst messages:")
    for i, (delta, status, p) in enumerate(sysex[:4]):
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in p)
        print(f"  [{i}] delta={delta} kind=0x{status:02X} len={len(p)}")
        print(f"      hex: {p[:64].hex(' ')}")
        print(f"      txt: {text[:64]}")

    if dump_n:
        print("\nfull dump:")
        for i, (delta, status, p) in enumerate(sysex[:dump_n]):
            text = "".join(chr(b) if 32 <= b < 127 else "." for b in p)
            print(f"[{i}] len={len(p)}")
            print(p.hex(" "))
            print(text)

    if want_strings:
        # Concatenate all payloads, then look for printable runs. Firmware
        # images often carry model names, version strings and error text.
        blob = b"".join(p for _, _, p in sysex)
        print(f"\nASCII runs >= 6 chars in the concatenated {len(blob)}-byte payload:")
        found = set()
        for m in re.finditer(rb"[\x20-\x7e]{6,}", blob):
            s = m.group().decode("ascii")
            if s not in found:
                found.add(s)
                print(f"  @0x{m.start():08x}  {s[:100]}")


if __name__ == "__main__":
    main()
