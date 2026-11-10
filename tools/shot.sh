#!/bin/bash
# tools/shot.sh <tag>: screenshot every running game window to /tmp/<tag>_<pid>.png
rm -f /tmp/$1_*.png
for p in $(pgrep -x thebigs2); do import -window $(xdotool search --onlyvisible --pid $p | head -1) /tmp/$1_$p.png; echo /tmp/$1_$p.png; done
