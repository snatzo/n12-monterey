#!/bin/zsh
#
# n12_off.sh - stop the background n12_agent started by n12_on.sh.
#
# When the last user client closes, the Yamaha driver tears the CoreAudio device
# down again, so the n12 disappears from the output list. That is normal, and
# running n12_on.sh brings it back.

set -u
cd "$(dirname "$0")"

pidfile="/tmp/n12_agent.pid"

if [[ ! -f "$pidfile" ]]; then
    echo "no pid file - is n12_on.sh running it?"
    exit 0
fi

pid=$(cat "$pidfile" 2>/dev/null)
if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null
    echo "stopped the n12 agent (pid $pid)"
else
    echo "the n12 agent is not running"
fi
rm -f "$pidfile"
