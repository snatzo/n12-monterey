#!/bin/zsh
#
# n12_launchd.sh - the entry point the login LaunchAgent runs.
#
# WHY THIS EXISTS
#   n12_on.sh starts n12_agent in the background and then exits. That is right
#   for a Terminal, but launchd reclaims a job's leftover processes when the
#   job's *main process* exits - so under launchd the agent would be killed the
#   instant n12_on.sh finished, and the device would disappear again.
#
#   So the job runs this wrapper instead: it does the normal bring-up (via
#   n12_on.sh, so there is only one place that knows the channel map), then
#   stays alive for as long as the agent is running. Keeping the main process
#   alive keeps the agent alive.
#
# Not meant to be run by hand; install.sh points the LaunchAgent here.

set -u

# Run from the directory this script lives in, where the tools are.
cd "$(dirname "$0")" || exit 1

# Bring the device up exactly as a user would (and make it the Mac's output).
./n12_on.sh --default

# n12_on.sh wrote the agent's pid. Wait on it: this loop is what stops launchd
# from tearing the agent down. It exits when the agent does (so n12_off.sh still
# works), which is what we want.
pidfile="/tmp/n12_agent.pid"
while :; do
    pid=$(cat "$pidfile" 2>/dev/null)
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        break
    fi
    sleep 5
done
