#!/bin/bash
# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
# Smallest zone (KB) that survives a scripted walk on E1M$1. Run in WSL after sim/build.sh.
cd "$(dirname "$0")/../sim"
MAP=$1; FRAMES=${FRAMES:-1500}
WALK=${WALK:-"U:40 R:15 U:40 B:3 L:25 U:60 B:3 R:30 U:50 D:20 L:15 U:70 B:3 A:2"}
ok() { FZ_ZONE_KB=$1 FZ_FAST=1 FZ_FRAMES=$FRAMES FZ_INPUT="$WALK" timeout 120 ./fzsim -nosound -warp $MAP 2>&1 | grep -q "I_Quit"; }
lo=16; hi=250
ok $hi || { echo "E1M$MAP: fails even at ${hi} KB"; exit 1; }
while [ $((hi - lo)) -gt 1 ]; do mid=$(((lo + hi) / 2)); if ok $mid; then hi=$mid; else lo=$mid; fi; done
echo "E1M$MAP: min zone ${hi} KB"
