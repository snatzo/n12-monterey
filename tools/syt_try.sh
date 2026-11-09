#!/bin/zsh
#
# syt_try.sh - play the same tone under different clock settings, so you can
# pick the one that does not pulse.
#
# Why this exists: the 1 kHz tone from n12_play is audible out of the desk, but
# it pulses at a slow, countable rate. A pulse that slow cannot be a per-buffer
# glitch (a 512-frame buffer would buzz at 86 Hz); it is a beat between the Mac's
# sample clock and the desk's. The one clock control the Yamaha driver exposes is
# sSetSYTSynchChannel (selector 43): 0xffffffff means "this node is the clock
# master", any channel number means "slave to whoever transmits on that channel".
#
# This script restarts n12_agent once per setting - the clock has to be chosen
# before the streams start - and plays a six-second tone each time, so you can
# listen to them one after another and compare.
#
# It changes nothing on the desk and writes nothing: it only chooses how the Mac
# derives its isochronous clock.
#
# Run:
#     cd /path/to/n12-monterey/tools
#     ./syt_try.sh

set -u
cd "$(dirname "$0")"

# Each entry: a label, and the value to hand to sSetSYTSynchChannel.
#   0xffffffff  this Mac is the clock master (the driver's own default)
#   16          slave to the desk's transmit channel (ISP 0)
#   1           slave to the desk's receive channel (ISP 2) - the odd one
#   0           the value the driver happens to hold at boot
labels=( "Mac is clock master (default)" "slave to channel 16 (desk TX)" \
         "slave to channel 1 (desk RX)"     "SYT channel 0" )
vals=( 0xffffffff 16 1 0 )

for i in {1..4}; do
    echo ""
    echo "============================================================"
    echo "  ${i} of 4 : ${labels[$i]}   (syt=${vals[$i]})"
    echo "  listen for a steady tone - a pulse you can count means a beat"
    echo "============================================================"

    # Start the device with this clock choice, and keep it alive while the tone
    # plays. --run-for is wide enough for the setup delay plus the tone.
    ./n12_agent --in-channel=16 --out-channel=1 --syt=${vals[$i]} --run-for=16 >/tmp/syt_agent.log 2>&1 &
    agent=$!
    sleep 7          # let the device appear and the streams start

    # Six seconds of tone. The idle noise before and after it is expected.
    ./n12_play --seconds=6 --freq=1000

    wait $agent 2>/dev/null
    sleep 1
done

echo ""
echo "done - tell me which number sounded steady (1, 2, 3 or 4), and whether"
echo "any of them still pulsed."
