#!/bin/bash
# Two local instances to the Game Room, both joined, nobody Ready (for looking at the player slots).
cd "$(dirname "$0")/.." || exit 1
K=${TB2_KEYDIR:-/tmp}; : > $K/host.keys; : > $K/guest.keys
pkill -x thebigs2; sleep 1
send() { echo "$2" | tr ';' '\n' >> $K/$1.keys; }
tools/run_pair.sh host; sleep 13
send host "untilmenu space;wait 3000;tap shift+Up;wait 800;tap space;wait 3500;tap space;wait 3500;tap space;wait 7000;tap l"
until ss -ltn | grep -q :51000; do sleep 1; done; sleep 4
tools/run_pair.sh guest; sleep 13
send guest "untilmenu space;wait 3000;tap shift+Up;wait 800;tap space;wait 3500;tap space;wait 3500;tap space"
sleep 26
echo "to_gameroom: done"
