#!/bin/bash
#
# fw_probe.sh — read-only FireWire / CoreAudio diagnostic for the Yamaha n12.
#
# What it does: gathers everything needed to answer "does macOS already drive
# this device, and if not, where does the chain break?" It changes nothing —
# no kext loading, no writes outside its own output directory.
#
# Usage:   ./fw_probe.sh [output_dir]
# Default output dir: ./probe_out
#
# Notes on style: this deliberately avoids bash arrays. macOS ships bash 3.2,
# where expanding an empty array under `set -u` is an "unbound variable" error.
#
# Exit status is always 0; this is a diagnostic, not a test.

set -u

# --- Where to put the report -------------------------------------------------
# ${1:-default} means "first argument, or this default if none was given".
OUT_DIR="${1:-probe_out}"
mkdir -p "$OUT_DIR" || exit 1

# A single timestamped report file, plus one raw file per section so nothing
# gets lost if a command produces huge output.
STAMP="$(date '+%Y%m%d-%H%M%S')"
REPORT="$OUT_DIR/report-$STAMP.txt"

# --- Helper: run a command, print a header, tee it to a raw file -------------
# $1 = section title, $2 = raw filename, rest = command to run.
# We keep going even if a command fails (some only work with a device attached).
section() {
    local title="$1"; shift
    local rawfile="$1"; shift
    printf '\n================================================================\n' >>"$REPORT"
    printf '== %s\n' "$title" >>"$REPORT"
    printf '   cmd: %s\n' "$*" >>"$REPORT"
    # printf '%s\n' is used because a format string starting with "--" would
    # otherwise be parsed as an option flag by printf.
    printf '%s\n' '----------------------------------------------------------------' >>"$REPORT"
    # Run the command once, capturing both streams into the raw file...
    "$@" >"$OUT_DIR/$rawfile" 2>&1
    local rc=$?
    # ...then copy that into the combined report so both views stay in sync.
    cat "$OUT_DIR/$rawfile" >>"$REPORT"
    if [ $rc -ne 0 ]; then
        printf '(command exited %d)\n' "$rc" >>"$REPORT"
    fi
}

# --- Header -----------------------------------------------------------------
{
    printf 'FireWire / CoreAudio probe\n'
    printf 'when:    %s\n' "$(date)"
    printf 'host:    %s\n' "$(hostname)"
    printf 'user:    %s\n' "$(whoami)"
    printf 'macOS:   %s (%s)\n' \
        "$(sw_vers -productVersion)" "$(sw_vers -buildVersion)"
    printf 'arch:    %s   cputype: %s\n' \
        "$(uname -m)" "$(sysctl -n hw.cputype 2>/dev/null)"
    printf 'boot:    %s\n' "$(uptime | sed 's/^ *//')"
} >"$REPORT"

# --- 0. Thunderbolt chain ---------------------------------------------------
# The FireWire controller arrives via Thunderbolt, so the TB topology matters:
# a TB-to-FireWire adapter appears as a PCIe bridge, and AppleThunderboltPCIUpAdapter
# must bind it before IOFireWireFamily can drive the controller behind it.
section "Thunderbolt topology" \
    "00-thunderbolt.txt" \
    system_profiler -detailLevel full SPThunderboltDataType

# --- 1. Is a FireWire device attached at all? -------------------------------
# system_profiler parses the config ROM for us: vendor/model IDs, Unit Spec ID,
# Unit Software Version, and the device's textual names.
section "FireWire device tree (system_profiler)" \
    "01-spfirewire.txt" \
    system_profiler -detailLevel full SPFireWireDataType

# --- 2. Raw IORegistry, per FireWire class ---------------------------------
# These are the exact IOKit classes involved in the audio chain, in order.
# IOFireWireUnit          - a FireWire "unit" (device) on the bus
# IOFireWireAVCUnit       - Apple's AV/C layer on top of that unit
# IOFireWireAVCSubUnit    - an AV/C subunit; a Music Subunit is type 1
# AppleFWAudioDevice      - Apple's audio driver, binds the subunit
section "IOFireWireDevice (all)"        "02-ioreg-device.txt"  ioreg -l -w0 -c IOFireWireDevice
section "IOFireWireUnit (all)"          "03-ioreg-unit.txt"    ioreg -l -w0 -c IOFireWireUnit
section "IOFireWireLocalNode (all)"     "04-ioreg-localnode.txt" ioreg -l -w0 -c IOFireWireLocalNode
section "IOFireWireAVCUnit (all)"       "05-ioreg-avcunit.txt" ioreg -l -w0 -c IOFireWireAVCUnit
section "IOFireWireAVCSubUnit (all)"    "06-ioreg-avcsubunit.txt" ioreg -l -w0 -c IOFireWireAVCSubUnit
section "AppleFWAudioDevice (all)"      "07-ioreg-fwaudio.txt" ioreg -l -w0 -c AppleFWAudioDevice
section "IOAudioDevice (all)"           "08-ioreg-audiodevice.txt" ioreg -l -w0 -c IOAudioDevice

# --- 3. FireWire subtree in full -------------------------------------------
# A plain tree view makes the parent/child relationships obvious, which is how
# you tell whether Apple's AVC layer attached underneath the device.
section "Full IORegistry subtree under FireWire" \
    "09-ioreg-tree.txt" \
    ioreg -l -w0 -p IOService -n IOFireWireDevice

# --- 4. Which kexts are loaded ---------------------------------------------
# kextstat lists loaded kernel extensions. We filter for the FireWire and audio
# families we care about, so we can see exactly how far the stack got.
section "Loaded kexts: firewire / audio" \
    "10-kextstat.txt" \
    sh -c 'kextstat | grep -iE "firewire|fwaudio|IOAudio|AppleAVC"'

# --- 5. Are Yamaha's own kexts present or loaded? --------------------------
# If these show up, the vendor driver is installed and would be claiming the
# device before Apple's stack gets a chance.
section "Yamaha kexts present" \
    "11-yamaha-kexts.txt" \
    sh -c 'ls -d /Library/Extensions/*[Yy]amaha* /System/Library/Extensions/*[Yy]amaha* 2>/dev/null; kextstat | grep -i yamaha; echo "(end)"'

# --- 6. What CoreAudio can see ---------------------------------------------
# If the device reached CoreAudio, it appears here with its channel counts.
section "CoreAudio devices" \
    "12-coreaudio.txt" \
    system_profiler -detailLevel full SPAudioDataType

# --- 7. What CoreMIDI can see ----------------------------------------------
section "CoreMIDI devices" \
    "13-coremidi.txt" \
    system_profiler SPMIDIDataType

# --- 8. Live Audio MIDI Setup view -----------------------------------------
# `system_profiler` is a snapshot; this asks the running daemons directly.
section "coreaudiod / coremidid processes" \
    "14-daemons.txt" \
    sh -c 'pgrep -l -f "coreaudiod|coremidi" || echo "(none found)"'

# --- 9. Full property list of the AVC subunit, if present -------------------
# This is where the Music Subunit's plug/stream descriptors would show up.
section "AVC subunit properties (verbose)" \
    "15-avcsubunit-verbose.txt" \
    sh -c 'ioreg -l -w0 -c IOFireWireAVCSubUnit -d 1'

printf '\nReport written to: %s\n' "$REPORT"
printf 'Raw sections in:   %s/\n' "$OUT_DIR"

# --- 10. One-line verdict ---------------------------------------------------
# Make the result easy to skim without reading the whole report.
echo
echo "--- quick verdict ---"
if system_profiler SPFireWireDataType 2>/dev/null | grep -qi 'FireWire'; then
    echo "FireWire:      a device appears to be attached"
else
    echo "FireWire:      NO device detected — plug the n12 in and power it on"
fi
if ioreg -c AppleFWAudioDevice 2>/dev/null | grep -q AppleFWAudioDevice; then
    echo "AppleFWAudio:  BOUND — Apple's audio driver has claimed the device"
else
    echo "AppleFWAudio:  not bound"
fi
if ioreg -c IOFireWireAVCSubUnit 2>/dev/null | grep -q IOFireWireAVCSubUnit; then
    echo "AVC subunit:   present — the device exposes an AV/C subunit"
else
    echo "AVC subunit:   none found"
fi
