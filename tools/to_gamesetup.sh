#!/bin/bash
# Two instances from launch to Game Setup (stadium). Extra args: key script appended to both after Lineup (host|guest tap lines via $EXTRA_HOST/$EXTRA_GUEST).
cd "$(dirname "$0")/.." || exit 1
tools/auto_online.sh
K=${TB2_KEYDIR:-/tmp}
send() { echo "$2" | tr ';' '\n' >> $K/$1.keys; }
SEQ="wait 2000;SIDE;wait 2000;tap space;wait 2500;tap space;wait 16000;tap space;wait 3500;tap space;wait 6000"
send host "${SEQ/SIDE/tap shift+Left}"; send guest "${SEQ/SIDE/tap shift+Right}"
[ -n "$EXTRA_HOST" ] && send host "$EXTRA_HOST"; [ -n "$EXTRA_GUEST" ] && send guest "$EXTRA_GUEST"
