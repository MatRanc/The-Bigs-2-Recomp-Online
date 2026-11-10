#!/bin/bash
# Start host (User1) then joiner (User2) on this machine. Usage: tools/run_pair.sh [host|guest|both]
# "both" waits for the host listener on :51000 before starting the guest. Logs: out/build/linux-amd64-release/logs.
# The online env comes from the launcher (thebigs2_gui.py --online-env), so this tests exactly what players get.
# DEBUG=0 drops the trace/log flags; GAME_ARGS is passed to the game (e.g. --fps_log_interval=5). PEER_IP defaults to this machine's LAN IP: both ends then see the same IP, the
# game splits the lockstep link onto UDP 1795 (host) / 1796 (guest) and the GameConn hook swaps the connect ports.
PEER_IP=${PEER_IP:-$(hostname -I | awk '{print $1}')}
cd "$(dirname "$0")/.." || exit 1
BIN=./out/build/linux-amd64-release/thebigs2
COMMON="SDL_VIDEODRIVER=x11"
[ "${DEBUG:-1}" = 1 ] && COMMON="$COMMON TB2_CTRL_TRACE=1 TB2_UI_TRACE=1 TB2_NET_LOG=1 TB2_ONLINE_DEBUG=1"
penv() { python3 thebigs2_gui.py --online-env "$@" | tr '\n' ' '; }
host() { env $COMMON $(penv Host User1) TB2_AUTOKEY=${TB2_KEYDIR:-/tmp}/host.keys nohup $BIN $GAME_ARGS >/dev/null 2>&1 & }
guest() { env $COMMON $(penv Join User2 "$PEER_IP") TB2_AUTOKEY=${TB2_KEYDIR:-/tmp}/guest.keys nohup $BIN $GAME_ARGS >/dev/null 2>&1 & }
case "${1:-both}" in
  host) host ;;
  guest) guest ;;
  both) host; until ss -ltn | grep -q :51000; do sleep 1; done; guest ;;
esac
