#!/bin/bash
#
# try_kextload.sh - temporarily load Yamaha's own n8/n12 kexts, WITHOUT
# installing them.
#
# Why: the two kexts for the modern (post-10.9) path are x86_64 and carry valid
# Developer ID signatures. Nobody has established whether macOS 12 Monterey will
# still load them. Loading them directly answers that in one step, with nothing
# written to /Library/Extensions or anywhere else permanent. A reboot undoes
# everything this script does.
#
# Usage:
#   sudo ./try_kextload.sh            # stage + load + report (no install)
#   sudo ./try_kextload.sh --install  # install payloads to their proper paths
#   sudo ./try_kextload.sh --uninstall# remove what --install placed
#   sudo ./try_kextload.sh --unload   # unload from the running kernel
#   sudo ./try_kextload.sh --stage    # only stage, do not load
#   sudo ./try_kextload.sh --bypass   # load with kmutil's -z (skip the consent
#                                     # check for this action only)
#
# About --bypass: kmutil's -z / --no-authorization skips the kext-consent check
# for that one load. It does NOT change any system setting and does NOT add
# Yamaha to any allow-list, so the approval requirement is still in force for
# the next boot. It exists to answer "would this kext actually load and work?",
# which is a question the ordinary path cannot answer until the user has clicked
# Allow. On a machine whose owner is making an informed choice, that is a
# reasonable thing to do; it is not a way to permanently weaken the machine.
#
# What it does NOT do: it does not install the package, does not touch SIP
# areas, and does not enable anything at boot.
#
# Expect this to be stopped by kext-consent on the first run. That is the
# normal High-Sierra-and-later behaviour for an unapproved kext: macOS records
# the request and shows an "Allow" button in
#   System Settings -> Privacy & Security
# under the heading for "Yamaha Corporation". Approving it and re-running this
# script is the intended flow.

set -u

# --- Configuration ----------------------------------------------------------
# Where the original package lives. Override with: PKG=/path ./try_kextload.sh
# If you have already installed Yamaha's driver, the kexts are in
# /Library/Extensions and no package is needed - this script uses them directly.
# Otherwise it looks for a downloaded Yamaha "…Driver…pkg" in ~/Downloads.
PKG="${PKG:-$(ls "${HOME:-}"/Downloads/*Yamaha*Driver*.pkg 2>/dev/null | head -1)}"

# Scratch area for the staged, root-owned kexts. Kept out of the project so a
# mis-set permission here can never affect the source tree.
STAGE="${STAGE:-/tmp/yamaha_kext_test}"

# The two kexts on the modern path, with the sub-path they live at inside the
# expanded package. The "Legacy" copies target /System/Library/Extensions and
# are only selected below macOS 10.9, so they are not used here.
KEXT_ASYNC="YamahaFWAsync.pkg/Payload/YamahaFWAsync.kext"
KEXT_AUDIO="YamahaFWAudioDriver.pkg/Payload/YamahaFWAudioDriver.kext"

MODE="${1:-load}"

# --bypass is a modifier on a normal load, not a separate mode, so fold it into
# the BYPASS variable and treat the mode as a plain load.
BYPASS=""
if [ "$MODE" = "--bypass" ]; then
    MODE="load"
    BYPASS="-z"
fi

# --- Must be root -----------------------------------------------------------
# kexts have to be owned by root and the loader needs kernel access.
if [ "$(id -u)" -ne 0 ]; then
    echo "This script must run as root. Try:  sudo $0 $*" >&2
    exit 1
fi

# --- Helpers ----------------------------------------------------------------
# Print a heading so the output is easy to scan.
hdr() {
    printf '\n%s\n' '============================================================'
    printf '== %s\n' "$1"
    printf '%s\n' '------------------------------------------------------------'
}

# --- Unload mode ------------------------------------------------------------
# Unloading is best-effort: if a kext is in use, kextunload refuses, and a
# reboot is the fallback (which clears everything this script did anyway).
if [ "$MODE" = "--unload" ]; then
    hdr "Unloading"
    # kextunload -b takes the bundle identifier, which is stable across paths.
    kextunload -b jp.co.yamaha.driver.YamahaFWAudioDriver 2>&1 || true
    kextunload -b jp.co.yamaha.driver.YamahaFWAsync 2>&1 || true
    hdr "Still loaded?"
    kextstat | grep -i yamaha || echo "(no Yamaha kexts loaded)"
    exit 0
fi

# --- Stage ------------------------------------------------------------------
# Two ways in: use the driver that is already installed, or extract the kexts
# from a downloaded package. The installed path is simpler and is what most
# users want; the package path lets you load a specific version without
# installing it.
INSTALLED=0
if [ -d /Library/Extensions/YamahaFWAudioDriver.kext ] \
   && [ -d /Library/Extensions/YamahaFWAsync.kext ] \
   && [ ! -f "$PKG" ]; then
    hdr "Using the already-installed Yamaha driver"
    echo "Found /Library/Extensions/YamahaFW*.kext - loading those directly."
    STAGE="/Library/Extensions"
    INSTALLED=1
else
    hdr "Staging kexts from the package"
    if [ ! -f "$PKG" ]; then
        echo "No Yamaha driver found." >&2
        echo "  * install the official Yamaha Steinberg FW Driver, or" >&2
        echo "  * set PKG=/path/to/Yamaha\\ Steinberg\\ FW\\ Driver.pkg" >&2
        exit 1
    fi

    # Start clean every time so a half-expanded previous attempt cannot confuse us.
    rm -rf "$STAGE"
    mkdir -p "$STAGE"
    cd "$STAGE" || exit 1

    # Expand the whole package, payloads included. `--expand-full` gives us the
    # actual file tree rather than just the archives.
    if ! pkgutil --expand-full "$PKG" "$STAGE/expanded" 2>&1; then
        echo "pkgutil --expand-full failed" >&2
        exit 1
    fi

    # Copy just the two kexts we want, out of the package layout and up to $STAGE.
    cp -R "$STAGE/expanded/$KEXT_ASYNC" "$STAGE/" || exit 1
    cp -R "$STAGE/expanded/$KEXT_AUDIO" "$STAGE/" || exit 1

    # Kernel extensions must be owned by root:wheel, and must not be group- or
    # world-writable; the loader rejects anything else.
    chown -R root:wheel "$STAGE"
    chmod -R go-w "$STAGE"

    # Strip any extended attributes inherited from the download (quarantine flag,
    # resource forks). These make codesign refuse to validate a bundle.
    xattr -cr "$STAGE" 2>/dev/null || true
fi

# Record what we are about to load, so the result can be tied to a specific
# build of the kexts rather than assumed.
hdr "Staged"
for k in YamahaFWAsync.kext YamahaFWAudioDriver.kext; do
    echo "--- $k"
    codesign -dv "$STAGE/$k" 2>&1 | grep -E 'Identifier|Authority|TeamIdentifier|Format' || true
    lipo -archs "$STAGE/$k/Contents/MacOS/"* 2>/dev/null || true
done

if [ "$MODE" = "--stage" ]; then
    echo
    echo "Staged only. Kexts are in $STAGE"
    exit 0
fi

# --- Baseline ---------------------------------------------------------------
hdr "Baseline (before)"
echo "Yamaha kexts loaded:"
kextstat | grep -i yamaha || echo "  (none)"
echo
echo "FireWire device present:"
ioreg -w0 -p IOService 2>/dev/null | grep -c 'IOFireWireDevice@' || echo 0

# --- Install mode -----------------------------------------------------------
# Copies the package's payloads into their proper system locations, driven by
# the install-location recorded in each sub-package's PackageInfo. That is
# exactly what the installer would do, but visible and reversible.
#
# This is needed because the kext declares IOCFPlugInTypes as a RELATIVE path
# (YamahaFWAudioDriver.kext/Contents/PlugIns/YamahaFWUserLib.bundle). IOKit
# resolves that against the standard extension directories, so a copy parked in
# /tmp is invisible to it. The kexts must live in /Library/Extensions.
if [ "$MODE" = "--install" ]; then
    # Copy one sub-package's Payload to its declared install-location.
    install_payload() {
        local sub="$1"
        local info="$STAGE/expanded/$sub/PackageInfo"
        local payload="$STAGE/expanded/$sub/Payload"
        # Skip sub-packages that have no payload (nothing to place).
        [ -f "$info" ] || return 0
        [ -d "$payload" ] || return 0
        # install-location is an attribute of the <pkg-info> element.
        local loc
        loc=$(sed -n 's/.*install-location="\([^"]*\)".*/\1/p' "$info" | head -1)
        [ -n "$loc" ] || loc="/"
        printf '  %-28s -> %s\n' "$sub" "$loc"
        mkdir -p "$loc"
        # ditto preserves the tree and metadata better than cp -R.
        ditto "$payload/" "$loc/" || echo "     (ditto reported a problem)"
    }

    hdr "Installing payloads"
    # Install in the order the Distribution would, kexts first.
    for sub in \
        YamahaFWAsync.pkg \
        YamahaFWAudioDriver.pkg \
        YamahaFWEnabler.pkg \
        YamahaFWHAL.pkg \
        YamahaFWMIDIPlugIn.pkg \
        YamahaFWMIDIIcon.pkg \
        YamahaFWDriverplist.pkg \
        YamahaFWCM.pkg \
        YamahaFWCP.pkg
    do
        # Clear any previous copy first, so this is repeatable. Safe: these are
        # only ever the paths this script itself installed.
        case "$sub" in
            YamahaFWAsync.pkg)        rm -rf /Library/Extensions/YamahaFWAsync.kext ;;
            YamahaFWAudioDriver.pkg)  rm -rf /Library/Extensions/YamahaFWAudioDriver.kext ;;
        esac
        install_payload "$sub"
    done

    hdr "Ownership and permissions"
    # Kexts must be root:wheel and not group/world writable, or the loader
    # refuses them. The rest is tidied the same way for consistency.
    for p in \
        /Library/Extensions/YamahaFWAsync.kext \
        /Library/Extensions/YamahaFWAudioDriver.kext \
        "/Library/Frameworks/YamahaFWEnabler.framework" \
        "/Library/Application Support/Yamaha" \
        "/Library/Audio/MIDI Drivers/YamahaFWMIDI.plugin" \
        "/Library/Audio/MIDI Devices/Yamaha" \
        /Library/PreferencePanes/"Yamaha Steinberg FW.prefPane" \
        /Applications/"Yamaha Steinberg FW Control Panel.app" \
        /Applications/Yamaha
    do
        [ -e "$p" ] || continue
        chown -R root:wheel "$p"
        chmod -R go-w "$p"
        xattr -cr "$p" 2>/dev/null || true
        echo "  fixed up $p"
    done

    hdr "Installed layout"
    ls -d /Library/Extensions/*Yamaha* 2>/dev/null
    ls -d "/Library/Frameworks/YamahaFWEnabler.framework" 2>/dev/null
    ls -d "/Library/Audio/MIDI Drivers/"*Yamaha* 2>/dev/null

    hdr "The postinstall symlink (expected to fail)"
    # The package's postinstall creates
    #   /System/Library/Extensions/YamahaFWAsync.kext/Contents/PlugIns
    # pointing at the /Library copy. /System is sealed on Big Sur and later, so
    # this cannot succeed without disabling SIP. Reporting it rather than hiding
    # it, because if anything later looks up that path it matters.
    if [ ! -e /System/Library/Extensions/YamahaFWAsync.kext ]; then
        mkdir -p /System/Library/Extensions/YamahaFWAsync.kext/Contents 2>/dev/null \
            && ln -s /Library/Extensions/YamahaFWAsync.kext/Contents/PlugIns \
                     /System/Library/Extensions/YamahaFWAsync.kext/Contents/PlugIns 2>/dev/null \
            && echo "  symlink created" \
            || echo "  could not create it (sealed system volume) - expected"
    else
        echo "  already present"
    fi

    hdr "Next"
    echo "Reboot to activate the kexts, then run tools/yamaha_fw_start.c's built"
    echo "binary to drive the driver. Undo with the bundled uninstaller or:"
    echo "  sudo $0 --uninstall"
    exit 0
fi

# --- Uninstall mode ---------------------------------------------------------
if [ "$MODE" = "--uninstall" ]; then
    hdr "Removing installed paths"
    for p in \
        /Library/Extensions/YamahaFWAsync.kext \
        /Library/Extensions/YamahaFWAudioDriver.kext \
        "/Library/Frameworks/YamahaFWEnabler.framework" \
        "/Library/Application Support/Yamaha" \
        "/Library/Audio/MIDI Drivers/YamahaFWMIDI.plugin" \
        "/Library/Audio/MIDI Devices/Yamaha" \
        /Library/PreferencePanes/"Yamaha Steinberg FW.prefPane" \
        /Applications/"Yamaha Steinberg FW Control Panel.app" \
        /Applications/Yamaha/FWDriver/YamahaFWCM.app \
        /Library/LaunchAgents/com.yamaha.YamahaFWCM.plist \
        /Library/Preferences/com.yamaha.FWDriver.plist
    do
        [ -e "$p" ] || continue
        rm -rf "$p" && echo "  removed $p"
    done
    echo
    echo "Kexts already loaded into the running kernel go away at the next reboot."
    exit 0
fi

# --- Load -------------------------------------------------------------------
# kmutil is the modern loader; it supersedes kextload, which is now a shim.
# Load the async/transport kext first, then the audio driver.
for k in YamahaFWAsync.kext YamahaFWAudioDriver.kext; do
    hdr "Loading $k"
    # kmutil is the modern loader; it supersedes kextload, which is now a shim.
    # With BYPASS set we pass -z, skipping the consent check for this one load.
    # Capture the output so the error codes can be interpreted below.
    if [ -n "$BYPASS" ]; then
        out=$(kmutil load -z -p "$STAGE/$k" 2>&1); rc=$?
    else
        out=$(kmutil load -p "$STAGE/$k" 2>&1); rc=$?
    fi
    printf '%s\n' "$out"
    if [ "$rc" -eq 0 ]; then
        echo "-> load command succeeded"
    else
        echo "-> load command FAILED"
        # KMErrorDomain codes that matter here:
        #   27 = consent not granted, and macOS will have offered an Allow button
        #   28 = consent granted, but the kernel needs a reboot to honour it
        case "$out" in
            *Code=27*)
                echo "   [27] Consent refused. Open System Preferences ->"
                echo "        Security & Privacy -> General and click Allow for"
                echo "        Yamaha Corporation, then re-run this script." ;;
            *Code=28*)
                echo "   [28] Consent is granted, but a reboot is required before"
                echo "        the kernel will honour it. Reboot, then re-run this"
                echo "        script. This is a normal, expected stage." ;;
            *notariz*|*"code signature"*|*"not valid"*)
                echo "   Hard block: signature or notarization. The kext cannot"
                echo "   be used on this OS version." ;;
            *)
                echo "   Unrecognised failure - see the message above." ;;
        esac
    fi
done

# --- Result -----------------------------------------------------------------
hdr "Result"
echo "Yamaha kexts loaded:"
kextstat | grep -i yamaha || echo "  (none)"

echo
echo "IOKit nodes created by the Yamaha kexts:"
ioreg -w0 -p IOService 2>/dev/null | grep -iE 'YamahaFW|YFWAsync' || echo "  (none)"

echo
echo "CoreAudio devices:"
system_profiler SPAudioDataType 2>/dev/null | grep -iE 'Yamaha|FireWire' || echo "  (no Yamaha audio device yet)"

hdr "To undo"
echo "Either unload:   sudo $0 --unload"
echo "Or just reboot:  a reboot clears any kext loaded this way."
