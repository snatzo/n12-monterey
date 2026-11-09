#!/usr/bin/env python3
"""
romdump.py - decode an IEEE 1212 / IEEE 1394 configuration ROM.

Why this exists: `system_profiler SPFireWireDataType` prints only a summary of
the config ROM. The detail that matters for driver work - which unit
directories exist, what specifier IDs and versions they declare, and what
"dependent info" they carry - is all in the raw ROM. IORegistry exposes the raw
ROM as a hex string under the `FireWire Device ROM` property, so this script
takes that hex and makes it readable.

Usage:
    ./romdump.py <hexstring>
    ./romdump.py --ioreg
    cat rom.hex | ./romdump.py

`--ioreg` shells out to ioreg, finds the first `FireWire Device ROM` property and
decodes it - convenient but fragile, since it scrapes ioreg's text output.

ROM structure, briefly:
  - A Bus Info Block (BIB), 5 quadlets, at offset 0. It carries the node's
    unique ID, the max payload size, and the length of the following root
    directory.
  - A root directory: a length quadlet, then entries. Each entry is a quadlet:
         bits 31:30  key type (0 immediate, 1 CSR offset, 2 leaf, 3 directory)
         bits 29:24  key id
         bits 23:0   value (immediate value, or a byte offset / 4)
  - 0x11 "Unit_Directory" entries point at unit directories, each of which
    declares a Unit_Spec_ID (0x12) and Unit_SW_Version (0x13). Those two values
    are what a host driver matches on.
  - 0x01 "Textual_Descriptor" points at a leaf holding a NUL-terminated string.

Reference: IEEE 1212 (CSR architecture) and the 1394 Trade Association config
ROM specification.
"""

import re
import subprocess
import sys

# Directory key names, per the config ROM standard. Keys 0x00-0x3F are the
# standardised set; vendors may use higher keys for their own purposes.
KEY_NAMES = {
    0x01: "Textual_Descriptor",
    0x02: "Bus_Dependent_Info",
    0x03: "Module_Vendor_ID",
    0x04: "Module_HW_Version",
    0x05: "Module_Spec_ID",
    0x06: "Module_SW_Version",
    0x07: "Module_Dependent_Info",
    0x08: "Node_Vendor_ID",
    0x09: "Node_HW_Version",
    0x0A: "Node_Spec_ID",
    0x0B: "Node_SW_Version",
    0x0C: "Node_Capabilities",
    0x0D: "Node_Unique_ID",
    0x0E: "Node_Units_Extent",
    0x0F: "Node_Memory_Extent",
    0x10: "Node_Dependent_Info",
    0x11: "Unit_Directory",
    0x12: "Unit_Spec_ID",
    0x13: "Unit_SW_Version",
    0x14: "Unit_Dependent_Info",
    0x15: "Unit_Location",
    0x16: "Unit_Poll_Mask",
    0x17: "Management_Agent",
    0x18: "Unit_Characteristics",
    0x38: "Model_ID",
    0x39: "Vendor_ID_instance",
    0x54: "Unit_Type",
}

TYPE_NAMES = {0: "immediate", 1: "offset", 2: "leaf", 3: "directory"}

# A few specifier IDs worth naming, so the output is readable rather than
# being a wall of hex.
SPECIFIER_IDS = {
    0x00A02D: "1394 Trade Association (AV/C, mLAN)",
    0x0000A0DE: "Yamaha Corporation",
}


class Rom:
    """A parsed IEEE 1212 configuration ROM."""

    def __init__(self, data: bytes):
        self.data = data

    def u32(self, quad: int) -> int:
        """Read the quadlet at quadlet index `quad` as an unsigned 32-bit int."""
        off = quad * 4
        chunk = self.data[off:off + 4]
        if len(chunk) < 4:
            return 0
        return int.from_bytes(chunk, "big")

    def read_string(self, quad: int) -> str:
        """
        Decode a textual descriptor leaf whose header quadlet is at `quad`.

        The leaf's header carries the character set in bits 23:16 and the
        language in bits 15:8; the characters follow from byte 4 onward and end
        at the first NUL.
        """
        out = []
        for b in self.data[quad * 4 + 4:]:
            if b == 0:
                break
            out.append(chr(b))
        return "".join(out)

    def dump_dir(self, quad: int, name: str, indent: int, depth: int = 0) -> None:
        """Print a directory starting at `quad`, recursing into subdirectories."""
        pad = " " * indent
        # Guard against runaway recursion if the ROM contains a bad offset.
        if depth > 6 or quad * 4 + 4 > len(self.data):
            print(f"{pad}{name}: offset out of range")
            return

        length = (self.u32(quad) >> 16) & 0xFFFF  # length in quadlets, incl. header
        print(f"{pad}== {name}  @0x{quad * 4:03x}  ({length} quadlets)")

        for i in range(1, length):
            entry = self.u32(quad + i)
            key_type = (entry >> 30) & 0x3
            key_id = (entry >> 24) & 0x3F
            value = entry & 0xFFFFFF
            key_name = KEY_NAMES.get(key_id, f"key_0x{key_id:02x}")

            line = f"{pad}   {key_name:<20} [{TYPE_NAMES[key_type]:<9}] "

            if key_type == 0:
                # Immediate: the value is the value.
                line += f"= 0x{value:06x}"
                # Name well-known IDs inline so the output reads clearly.
                if key_id in (0x03, 0x05, 0x08, 0x0A, 0x11, 0x12) and value in SPECIFIER_IDS:
                    line += f"   ({SPECIFIER_IDS[value]})"
            else:
                # Offset or leaf: the value is a byte offset divided by 4.
                line += f"-> @0x{value * 4:03x}"

            if key_type == 2:
                line += f'   "{self.read_string(value)}"'

            print(line)

            # Recurse into anything that is itself a directory. In unit
            # directories the dependent-info entry is an offset (type 1)
            # rather than a directory (type 3), so treat both as directories
            # when they appear under a unit directory.
            if key_type == 3 or (key_type == 1 and key_id == 0x14):
                self.dump_dir(value, key_name, indent + 6, depth + 1)

    def report(self) -> None:
        """Print the whole ROM: bus info block, then the directory tree."""
        print(f"Config ROM: {len(self.data)} bytes ({len(self.data) // 4} quadlets)")

        bib_len = (self.u32(0) >> 24) & 0xFF
        print("\nBus Info Block:")
        print(f"  info_length      = {bib_len} quadlets")
        print(f"  crc_length       = {(self.u32(0) >> 16) & 0xFF} quadlets")
        print(f"  capabilities     = 0x{self.u32(2):08x}  (max_rec={(self.u32(2) >> 12) & 0xF})")
        node_vendor = self.u32(3) >> 16
        chip_hi = self.u32(3) & 0xFFFF
        print(f"  node_vendor_id   = 0x{node_vendor:04x}")
        print(f"  chip_id          = 0x{chip_hi:04x}{self.u32(4):08x}")
        print(f"  node_unique_id   = 0x{node_vendor:04x}{chip_hi:04x}{self.u32(4):08x}")

        # The root directory starts immediately after the bus info block.
        print()
        self.dump_dir(bib_len, "ROOT DIRECTORY", 0)


def hex_from_ioreg() -> str:
    """
    Scrape the raw config ROM hex out of ioreg's text output.

    ioreg prints the ROM as `"FireWire Device ROM" = {"Offset 0"=<hex>}`. This
    findsthe first such property. It is deliberately tolerant of surrounding
    formatting because ioreg's output style is not a stable interface.
    """
    # Capture raw bytes: ioreg can emit non-UTF-8 data (it dumps binary
    # properties), so decoding strictly would raise. Latin-1 never fails and is
    # fine here because we only ever look at ASCII hex digits.
    raw = subprocess.run(
        ["ioreg", "-l", "-w0", "-c", "IOFireWireDevice"],
        capture_output=True,
    ).stdout
    out = raw.decode("latin-1")
    m = re.search(r'"FireWire Device ROM".*?<([0-9a-fA-F]+)>', out, re.S)
    if not m:
        raise SystemExit("no 'FireWire Device ROM' property found in ioreg output")
    return m.group(1)


def main() -> None:
    args = sys.argv[1:]
    if not args:
        text = sys.stdin.read()
        hexstr = "".join(re.findall(r"[0-9a-fA-F]", text))
    elif args[0] == "--ioreg":
        hexstr = hex_from_ioreg()
    else:
        hexstr = "".join(re.findall(r"[0-9a-fA-F]", "".join(args)))

    # Hex digits must come in pairs; drop any stray trailing nibble.
    hexstr = hexstr[: len(hexstr) - (len(hexstr) % 2)]
    if not hexstr:
        raise SystemExit("no hex data given")

    Rom(bytes.fromhex(hexstr)).report()


if __name__ == "__main__":
    main()
