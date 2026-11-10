#!/bin/zsh
#
# n12_on.sh - make the n12 available as a normal Mac audio output.
#
# Why this exists: the Yamaha driver only publishes the n12 as a CoreAudio
# device while something holds its user client open. The 32-bit component that
# used to do that cannot run on Monterey, so n12_agent stands in. This script
# starts it with the settings that are known to work and leaves it running, so
# the n12 then behaves like any other audio device.
#
# Two things it has to get right, both learned the hard way:
#
#   * The driver takes a moment to tear the old device down after the previous
#     agent exits. Starting a new one before that finishes fails with
#     sCreateAudioDevice -> 0xe00002be (out of resources), so this waits for the
#     device to actually disappear before starting again.
#
#   * By default it builds FOURTEEN OUTPUTS and SIXTEEN INPUTS, in the order
#     Mojave's driver uses. CoreAudio outputs 1-2 go to the desk's MAIN bus with
#     no channel strip (so ordinary macOS audio is heard with nothing pressed on
#     the desk), outputs 3-10 are desk channels 1-8, and outputs 11-14 are the
#     desk's stereo channels 9-12. Logic reserves outputs 1-2 as "stereo out", so
#     the first pair it offers a track is 3-4 = desk channel 1-2. The desk's
#     transmit (its stereo bus on DAW OUT 15/16) appears on Mac inputs 15/16 to
#     record. With --stereo it instead builds a single pair on desk ch 1-2.
#
# Nothing here writes to the desk.
#
# Run:
#     cd /path/to/n12-monterey/tools
#     ./n12_on.sh            # start it (device appears; choose it yourself)
#     ./n12_on.sh --default  # start it AND make it the Mac's output
#     ./n12_off.sh           # stop it

set -u
cd "$(dirname "$0")"

# The n12 does not exist in CoreAudio unless the Yamaha kexts are loaded, and
# they are loaded non-persistently, so after any reboot they must be reloaded.
# Without this check the failure was obscure: the device simply never appeared
# and the Mac quietly fell back to its built-in output.
if ! kextstat 2>/dev/null | grep -qi yamaha; then
    echo "The Yamaha kexts are not loaded, so the n12 cannot appear."
    echo "They are non-persistent, so this is expected after any reboot."
    echo ""
    echo "Load them first (needs your password), then run this again:"
    echo ""
    echo "    cd $(cd .. && pwd)"
    echo "    sudo tools/try_kextload.sh --bypass"
    echo ""
    exit 1
fi

pidfile="/tmp/n12_agent.pid"
logfile="/tmp/n12_agent.log"

want_default=0
want_stereo=0
want_rate=""
for a in "$@"; do
    [[ "$a" == "--default" ]] && want_default=1
    [[ "$a" == "--stereo" ]] && want_stereo=1
    [[ "$a" == --rate=* ]] && want_rate="${a#--rate=}"
done

# The agent takes --rate=N; in zsh an unquoted empty expansion adds no argument,
# so this is either "--rate=N" or nothing at all.
rate_flag=""
[[ -n "$want_rate" ]] && rate_flag="--rate=$want_rate"

# --- stop any previous agent, and wait for the driver to let the device go ---
# Kill by pidfile first, then by process name: an agent started by hand (for an
# experiment) is not in the pidfile, and leaving it running makes the new agent
# fail with "device already exists".
if [[ -f "$pidfile" ]]; then
    old=$(cat "$pidfile" 2>/dev/null)
    if [[ -n "$old" ]] && kill -0 "$old" 2>/dev/null; then
        echo "stopping the previous agent (pid $old)"
        kill "$old" 2>/dev/null
    fi
fi
if pgrep -f n12_agent >/dev/null 2>&1; then
    echo "stopping other n12_agent processes"
    pkill -f n12_agent 2>/dev/null
fi
for i in {1..15}; do
    # n12_fmt exits non-zero when there is no n12 device, which is our "gone" test.
    ./n12_fmt >/dev/null 2>&1 || break
    sleep 1
done

# --- start the agent and wait for it to actually build the device ---
echo "starting n12_agent..."
# Two build shapes, chosen by flag (docs/HOW-IT-WORKS.md):
#
#   default (--out-map=6,7,8,9,10,11,12,13)
#       ONE OUTPUT STREAM PER DESK CHANNEL. Each stream carries a distinct
#       channel number (+0x0c) and its own isochronous subchannel (+0x08), so
#       CoreAudio's outputs 1-8 ARE the desk's input channels 1-8 - send each
#       DAW track to its own desk strip and mix on the desk. This is the shape
#       the vendor connection manager used; the driver accepts many streams on
#       the one isochronous parameter as long as no two claim the same channel
#       number.
#
#   --stereo (--out-offset=6 --out-chans=2)
#       ONE STEREO STREAM on the desk's inputs 1-2, for ordinary system audio.
#       The offset is the stream structure's +0x08 field; channels 1-2 of the
#       device become desk inputs 1-2 (fed from DAW IN 7-8), which is what the
#       desk's stereo bus listens to.
if (( want_stereo )); then
    ./n12_agent --in-channel=16 --out-channel=1 $rate_flag --out-offset=6 --out-chans=2 >"$logfile" 2>&1 &
else
    # The Mojave order (docs/HOW-IT-WORKS.md):
    #   outputs 1-2  -> DAW IN 1-2   -> the desk's MAIN bus (no channel strip):
    #                                   this is where ordinary macOS audio lands,
    #                                   heard with nothing pressed on the desk;
    #   outputs 3-10 -> DAW IN 7-14  -> desk channels 1-8;
    #   outputs 11-14-> DAW IN 3-6   -> desk channels 9-12.
    # Logic reserves outputs 1-2 as "stereo out", so the first pair it offers a
    # track is 3-4, which is desk channel 1-2 - matching Mojave exactly. The input
    # map mirrors the desk's transmit so DAW OUT 15/16 (the stereo bus) lands on
    # Mac inputs 15/16 for recording.
    ./n12_agent --in-channel=16 --out-channel=1 $rate_flag \
        --out-map=0,1,6,7,8,9,10,11,12,13,2,3,4,5 \
        --in-map=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 >"$logfile" 2>&1 &
fi
agent=$!
echo "$agent" > "$pidfile"

ok=0
for i in {1..20}; do
    sleep 1
    if grep -q "sStartAudioDevice(1)              -> 0x00000000" "$logfile" 2>/dev/null; then
        ok=1; break
    fi
    # Give up early on a hard failure or if the process died.
    grep -q "could not create the audio device" "$logfile" 2>/dev/null && break
    kill -0 "$agent" 2>/dev/null || break
done
if (( ! ok )); then
    echo "the device did not come up within 20 s - see $logfile"
    echo "(is the desk switched on? are the Yamaha kexts loaded?)"
    exit 1
fi
echo "the n12 is up (pid $agent)."
echo ""

# Point the device's stereo pair at channels 1-2 (the main bus). CoreAudio has
# two notions of where stereo lives: the "preferred stereo pair" (used by the
# AudioUnit path - Logic, VLC, browsers) and the "channel layout" (used by the
# AudioQueue path - afplay, QuickTime, Music). The driver's layout puts stereo on
# channels 7-8, which are desk strips 5-6; n12_layout moves it to the main bus so
# both paths agree. This is not persistent, hence running it on every start.
if ./n12_layout >/dev/null 2>&1; then
    echo "  stereo pair -> device channels 1-2 (the main bus)"
fi
echo ""

# --- optionally make it the system output ---
if (( want_default )); then
    for i in {1..10}; do
        out=$(./n12_default 2>&1)
        if echo "$out" | grep -q "is now the Mac's default output"; then
            echo "$out" | sed -n '1,5p'
            break
        fi
        sleep 1          # the audio server may need a moment to see the device
    done
    echo ""
    echo "  play something - it should come out of the desk."
else
    echo "  choose \"n12\" as the output in System Settings > Sound, or in"
    echo "  your DAW / player, or re-run as:  ./n12_on.sh --default"
fi
echo ""
echo "  the device now has 14 outputs and 16 inputs, in Mojave's order:"
echo "    output 1-2   = desk MAIN bus (ordinary macOS audio, no strip, no DAW)"
echo "    output 3-10  = desk channels 1-8     (press DAW on those strips)"
echo "    output 11-14 = desk channels 9-12"
echo "    input  15/16 = the desk's stereo bus (record these for the mix)"
echo "  (start with --stereo for a single pair on desk ch 1-2 instead.)"
echo "  the Mac follows the desk's clock, so the audio is steady."
echo "  to stop:  ./n12_off.sh"
