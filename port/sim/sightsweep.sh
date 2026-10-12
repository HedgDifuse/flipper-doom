#!/bin/bash
# SD ms per frame in play (and the sight checks' reads) for "chase,idle" memo
# tics; real-time clock (FZ_FAST packs many tics into a frame), maps in parallel
cd "$(dirname "$0")"
MAPS=${MAPS:-"2 3 9"}
for n in "$@"; do
    a=${n%%,*}; b=${n#*,}
    EXTRA="-URANGECHECK -DFZ_READSTATS -DSIGHT_MEMO_TICS=$a -DSIGHT_MEMO_IDLE_TICS=$b" ./build.sh >/dev/null || exit 1
    cp fzsim /tmp/fzsim_${a}_${b}
    for m in $MAPS; do
        k=0
        for I in "U:30 R:10 U:30 A:3 .:5 L:12 U:20" "U:60 L:20 U:40 S:10 R:25 U:50 A:2 D:20"; do
            k=$((k+1))
            (FZ_SD_SKIP=100 FZ_ZONE_KB=80 FZ_FRAMES=${FRAMES:-1200} FZ_INPUT="$I" /tmp/fzsim_${a}_${b} -nosound -warp $m > /tmp/sw_${m}_${k}.txt 2>&1) &
        done
    done
    wait
    printf "memo %-6s:" $n
    for m in $MAPS; do
        for k in 1 2; do
            out=$(cat /tmp/sw_${m}_${k}.txt)
            v=$(echo "$out" | grep -oE "[0-9.]+ ms/frame" | tail -1 | cut -d" " -f1)
            s=$(echo "$out" | grep -oE "sight [0-9]+" | cut -d" " -f2)
            printf " %6s(%5s)" ${v:-ERR} ${s:-?}
        done
        printf " |"
    done
    echo
done
