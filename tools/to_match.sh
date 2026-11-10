#!/bin/bash
# Two local instances from launch to the first at-bat (native lockstep). Host = User1 (Home), guest = User2 (Road).
# Uses tools/auto_online.sh, then Team Setup (pads to sides, team, uniform), pitcher, lineup, host picks the stadium.
cd "$(dirname "$0")/.." || exit 1
K=${TB2_KEYDIR:-/tmp}
send() { echo "$2" | tr ';' '\n' >> $K/$1.keys; }
both() { send host "$1"; send guest "$1"; }
tools/auto_online.sh
send host "tap shift+Left"; send guest "tap shift+Right"; sleep 3
both "tap space;wait 1500;tap space"; sleep 8    # team + uniform
both "tap space"; sleep 6                        # starting pitcher
both "tap space"; sleep 6                        # lineup done
send host "tap space"; sleep 40                  # host: stadium -> match load
echo "to_match: done (first at-bat should be up)"
