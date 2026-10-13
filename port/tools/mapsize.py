# Copyright (C) 2026 HedgDifuse
# SPDX-License-Identifier: GPL-2.0-or-later
"""Flash bytes per object file from a GNU ld map: mapsize.py engine.map [N]"""
import collections
import re
import sys

c = collections.Counter()
text = open(sys.argv[1]).read()
for m in re.finditer(r"^ \.(?:text|rodata|data\.rel\.ro)\S*\s+0x0*80[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+)", text, re.M):
    c[re.split(r"[\/]", m.group(2))[-1]] += int(m.group(1), 16)
# names that do not fit on the section line are on the next one
for m in re.finditer(r"^ \.(?:text|rodata|data\.rel\.ro)\S*\n\s+0x0*80[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+)", text, re.M):
    c[re.split(r"[\/]", m.group(2))[-1]] += int(m.group(1), 16)
for k, v in c.most_common(int(sys.argv[2]) if len(sys.argv) > 2 else 30):
    print(f"{v:6d} {k}")
print(f"{sum(c.values()):6d} total")
