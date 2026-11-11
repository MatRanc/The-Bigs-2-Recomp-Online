#!/bin/bash
# Drive two local instances from launch to both-Ready (Team Setup) with the game's TB2_AUTOKEY injector
# (see online_debug.cpp). Keys: space=A, l=X, BackSpace=B, shift+Down=d-pad down. Screenshots: KEYDIR/*.png
cd "$(dirname "$0")/.." || exit 1
export TB2_KEYDIR=${TB2_KEYDIR:-/tmp}
K=$TB2_KEYDIR; : > $K/host.keys; : > $K/guest.keys
pkill -x thebigs2; sleep 1
DOWNS="tap shift+Up"   # menu wraps: Up once from the top reaches ONLINE
send() { echo "$2" | tr ';' '\n' >> $K/$1.keys; }
tools/run_pair.sh host; sleep 13
send host "untilmenu space;wait 3000;$DOWNS;wait 800;tap space;wait 3500;tap space;wait 3500;tap space;wait 7000;tap l"
until ss -ltn | grep -q :51000; do sleep 1; done; sleep 4
tools/run_pair.sh guest; sleep 13
send guest "untilmenu space;wait 3000;$DOWNS;wait 800;tap space;wait 3500;tap space;wait 3500;tap space"
sleep 24
send guest "tap space"; sleep 4; send host "tap space"; sleep 16
echo "auto_online: done (both Ready sent)"
