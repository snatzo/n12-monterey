#!/bin/bash
#
# install.sh - build the n12-monterey tools and (optionally) auto-start them.
#
# WHAT THIS DOES
#   1. Builds every tool from source into tools/ (via make).
#   2. Optionally installs two launchd jobs so the n12 comes back after a
#      reboot without you opening a Terminal:
#        * a LaunchDaemon (root)  - loads the Yamaha kexts at boot, because they
#          do not persist across reboots;
#        * a LaunchAgent  (you)   - runs tools/n12_on.sh --default at login.
#   3. Optionally makes the n12 the Mac's default output right now.
#
# WHAT THIS DOES NOT DO
#   It does not ship, install or copy Yamaha's driver. You must install the
#   official "Yamaha Steinberg FW Driver" yourself first (see INSTALL.md).
#
# USAGE
#   ./install.sh                 build only
#   ./install.sh --default       build, then make the n12 the Mac's output
#   ./install.sh --auto-start    build, then install the launchd jobs (sudo for
#                                the kext daemon)
#   ./install.sh --auto-start --default   both
#   ./install.sh --uninstall     remove the launchd jobs
#
# Bash 3.2 note (macOS): an empty array expanded as "${arr[@]}" is an "unbound
# variable" error under `set -u`, so this script avoids that pattern.

set -u

# The repository root is the directory this script lives in, so it works from any
# current directory.
REPO="$(cd "$(dirname "$0")" && pwd)"
TOOLS="$REPO/tools"

LABEL_AGENT="com.n12monterey.agent"
LABEL_KEXTS="com.n12monterey.kexts"
AGENT_PLIST="$HOME/Library/LaunchAgents/$LABEL_AGENT.plist"
KEXTS_PLIST="/Library/LaunchDaemons/$LABEL_KEXTS.plist"

want_default=0
want_autostart=0
want_uninstall=0
for a in "$@"; do
    case "$a" in
        --default)     want_default=1 ;;
        --auto-start)  want_autostart=1 ;;
        --uninstall)   want_uninstall=1 ;;
        *) echo "unknown option: $a"; exit 2 ;;
    esac
done

# ---------------------------------------------------------------- tools check
if ! xcode-select -p >/dev/null 2>&1; then
    echo "The Xcode command line tools are required (they provide clang)."
    echo "Install them with:  xcode-select --install"
    exit 1
fi

# ------------------------------------------------------------------- uninstall
if [ "$want_uninstall" = 1 ]; then
    echo "removing launchd jobs..."
    launchctl bootout "gui/$(id -u)/$LABEL_AGENT" 2>/dev/null || true
    rm -f "$AGENT_PLIST"
    sudo launchctl bootout "system/$LABEL_KEXTS" 2>/dev/null || true
    sudo rm -f "$KEXTS_PLIST"
    echo "done. (tools are still built in tools/; remove the repo to be rid of them)"
    exit 0
fi

# ----------------------------------------------------------------------- build
echo "building tools..."
if ! make -C "$TOOLS" >/dev/null; then
    echo "build failed - run 'make -C tools' to see the errors"
    exit 1
fi
echo "built: $(cd "$TOOLS" && echo n12_agent n12_layout n12_default n12_fmt)"
echo

# ------------------------------------------------------- optional auto-start
if [ "$want_autostart" = 1 ]; then
    echo "installing auto-start (launchd)..."

    # LaunchAgent: start the device at login, as you (CoreAudio devices are
    # per-user). RunAtLoad fires it once at login; no KeepAlive, so stopping it
    # with n12_off.sh stays stopped.
    mkdir -p "$HOME/Library/LaunchAgents"
    cat > "$AGENT_PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL_AGENT</string>
    <key>ProgramArguments</key>
    <array>
        <string>$TOOLS/n12_on.sh</string>
        <string>--default</string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>StandardOutPath</key><string>/tmp/n12_monterey.agent.log</string>
    <key>StandardErrorPath</key><string>/tmp/n12_monterey.agent.log</string>
</dict>
</plist>
PLIST
    launchctl bootout "gui/$(id -u)/$LABEL_AGENT" 2>/dev/null || true
    launchctl bootstrap "gui/$(id -u)" "$AGENT_PLIST"
    echo "  agent:  $AGENT_PLIST"

    # LaunchDaemon: load the kexts at boot, as root. The Yamaha kexts are not
    # persistent, so without this the n12 is absent after every reboot.
    echo "  installing the kext LoadDaemon (needs your password)..."
    sudo tee "$KEXTS_PLIST" >/dev/null <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL_KEXTS</string>
    <key>ProgramArguments</key>
    <array>
        <string>$TOOLS/try_kextload.sh</string>
        <string>--bypass</string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>StandardOutPath</key><string>/tmp/n12_monterey.kexts.log</string>
    <key>StandardErrorPath</key><string>/tmp/n12_monterey.kexts.log</string>
</dict>
</plist>
PLIST
    sudo launchctl bootout "system/$LABEL_KEXTS" 2>/dev/null || true
    sudo launchctl bootstrap system "$KEXTS_PLIST"
    echo "  kexts:  $KEXTS_PLIST"
    echo
fi

# ------------------------------------------------- optional default output now
if [ "$want_default" = 1 ]; then
    if kextstat 2>/dev/null | grep -qi yamaha; then
        "$TOOLS/n12_on.sh" --default
    else
        echo "the Yamaha kexts are not loaded yet, so the device cannot come up."
        echo "load them once (needs your password), then re-run ./install.sh --default:"
        echo "    sudo $TOOLS/try_kextload.sh --bypass"
    fi
fi

echo
echo "next steps:"
echo "  1. read INSTALL.md"
echo "  2. if the kexts are not loaded:  sudo $TOOLS/try_kextload.sh --bypass"
echo "  3. start the device:             $TOOLS/n12_on.sh --default"
