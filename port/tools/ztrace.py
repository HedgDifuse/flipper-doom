# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
"""Sum FZ_ZTRACE zone allocations by caller: ztrace.py fzsim trace.txt"""
import collections
import subprocess
import sys

exe, trace = sys.argv[1:3]
agg = collections.defaultdict(lambda: [0, 0])
for line in open(trace):
    _, size, tag, addr = line.split()
    agg[(addr, tag)][0] += int(size)
    agg[(addr, tag)][1] += 1
for (addr, tag), (size, n) in sorted(agg.items(), key=lambda kv: -kv[1][0])[:30]:
    func, loc = (subprocess.run(["addr2line", "-e", exe, "-f", "-s", addr], capture_output=True, text=True).stdout.split() + ["", ""])[:2]
    print(f"{size:7d} {n:5d} tag {tag:>3} {func} {loc}")
