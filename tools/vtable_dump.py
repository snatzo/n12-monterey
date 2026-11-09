#!/usr/bin/env python3
"""
vtable_dump.py - print the function-pointer tables (vtables) in a Mach-O binary.

Why this exists: the Yamaha plug-in (YamahaFWUserLib) exposes its API through
COM-style interfaces, and those interfaces are plain arrays of function
pointers in the binary's data. The kext and plug-in are not stripped, so every
one of those pointers has a symbol name. Reading the table therefore gives the
exact method order - which is what is needed to call a method by index without
having header files.

Usage:
    ./vtable_dump.py <mach-o binary> <symbol-prefix> [max-entries]

Example:
    ./vtable_dump.py YamahaFWUserLib s_IYamaha1394StreamDeviceInterface

Note on the symbol names: `nm` prints C++ symbols mangled; this script demangles
them for display by shelling out to c++filt.
"""

import re
import subprocess
import sys


def segments(path: str):
    """
    Parse `otool -l` into a list of (vmaddr, vmsize, fileoff, filesize) tuples.

    A Mach-O file offset and a virtual address are different things, and symbol
    addresses from nm are virtual. To read the bytes behind a symbol we have to
    translate through the segment table.
    """
    out = subprocess.run(["otool", "-l", path], capture_output=True, text=True).stdout
    segs = []
    cur = {}
    for line in out.splitlines():
        line = line.strip()
        # Each segment starts with a "segname" and its fields follow.
        m = re.match(r"segname (\S+)", line)
        if m:
            if cur:
                segs.append(cur)
            cur = {"segname": m.group(1)}
            continue
        for key in ("vmaddr", "vmsize", "fileoff", "filesize"):
            # vmaddr and vmsize are printed in hex with a 0x prefix, but fileoff
            # and filesize are printed in decimal. Accept either form.
            m = re.match(rf"{key} ((?:0x[0-9a-fA-F]+)|(?:\d+))", line)
            if m and cur is not None:
                text = m.group(1)
                cur[key] = int(text, 16) if text.startswith("0x") else int(text, 10)
    if cur:
        segs.append(cur)
    return [s for s in segs if "vmaddr" in s and "fileoff" in s]


def vm_to_file(segs, vmaddr: int):
    """Translate a virtual address to a file offset, or None if unmapped."""
    for s in segs:
        if s["vmaddr"] <= vmaddr < s["vmaddr"] + s.get("vmsize", 0):
            return s["fileoff"] + (vmaddr - s["vmaddr"])
    return None


def symbols(path: str):
    """
    Return {vmaddr: mangled_name} for every symbol nm can tell us about.

    Both defined symbols (T/D/S) and undefined ones are collected; undefined
    ones have no address so they simply do not appear.
    """
    out = subprocess.run(["nm", "-n", path], capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            table[addr] = parts[2]
    return table


def demangle(names):
    """Demangle a list of C++ names in one c++filt call."""
    if not names:
        return []
    proc = subprocess.run(["c++filt"], input="\n".join(names),
                          capture_output=True, text=True)
    return proc.stdout.splitlines()


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    prefix = sys.argv[2]
    max_entries = int(sys.argv[3]) if len(sys.argv) > 3 else 96
    # An IOExternalMethodDispatch table has 24-byte entries:
    #   IOExternalMethodAction function; uint32 checkScalarInputCount;
    #   uint32 checkStructureInputSize; uint32 checkScalarOutputCount;
    #   uint32 checkStructureOutputSize;   -> 8 + 4*4 = 24
    # A plain vtable of function pointers uses 8.
    stride = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    dispatch = stride == 24

    segs = segments(path)
    syms = symbols(path)

    # Every symbol whose *mangled* name contains the given text. Mangled C++
    # names embed the plain identifier, so a substring match works and is more
    # forgiving than reproducing the full mangling.
    bases = [(a, n) for a, n in syms.items() if prefix in n]
    if not bases:
        raise SystemExit(f"no symbols beginning with {prefix!r}")
    bases.sort()

    with open(path, "rb") as fh:
        blob = fh.read()

    for base_addr, base_name in bases:
        print(f"\n=== {base_name}  @ vmaddr 0x{base_addr:x}")

        off = vm_to_file(segs, base_addr)
        if off is None:
            print("  (address not mapped to a file offset)")
            continue

        # Walk entries until we hit a zero function pointer, which is where the
        # table ends.
        entries = []
        for i in range(max_entries):
            p = off + i * stride
            if p + 8 > len(blob):
                break
            ptr = int.from_bytes(blob[p:p + 8], "little")
            sizes = ()
            if dispatch and p + stride <= len(blob):
                sizes = tuple(int.from_bytes(blob[p + 8 + k * 4:p + 12 + k * 4], "little")
                              for k in range(4))
            entries.append((i, ptr, syms.get(ptr), sizes))
            if ptr == 0:
                break

        # Demangle lazily, one at a time, so that unnamed entries still get
        # printed with their raw pointer rather than being dropped.
        cache = {}
        for i, ptr, name, sizes in entries:
            if name is None:
                label = "(not a known symbol)"
            else:
                if name not in cache:
                    cache[name] = demangle([name])[0]
                label = cache[name]
            extra = ""
            if sizes:
                # inScalar, inStruct bytes, outScalar, outStruct bytes
                extra = f"   in: {sizes[0]} scalar, {sizes[1]}B struct" \
                        f"   out: {sizes[2]} scalar, {sizes[3]}B struct"
            print(f"  [{i:2d}] 0x{ptr:012x}  {label}{extra}")


if __name__ == "__main__":
    main()
