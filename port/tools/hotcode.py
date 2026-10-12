"""Split engine code into hot (runs in steady gameplay) and cold, in ARM bytes.

    hotcode.py syms.txt armsyms.txt trace1.txt [trace2.txt ...]

syms.txt: `nm` of sim/fzsim_instr (text symbols); traces: FZ_HOT_OUT files from
sim/build_instr.sh runs ("init <addr>" / "play <addr>", runtime PIE addresses);
armsyms.txt: "<object> <size> <type> <name>" from the PIC ARM objects.
Research tool, see docs/full-doom-options.md.
"""
import collections
import sys

syms = {}
for line in open(sys.argv[1]):
    a, _, name = line.split()
    syms[int(a, 16)] = name

arm = collections.defaultdict(list)  # name -> [(obj, size, type)]
for line in open(sys.argv[2]):
    obj, size, typ, name = line.split()
    arm[name].append((obj, int(size, 16), typ))

init, play = set(), set()
for path in sys.argv[3:]:
    rows = [l.split() for l in open(path)]
    addrs = [int(a, 16) for _, a in rows]
    # PIE: find the load delta that maps the most addresses onto symbol starts
    best = max({a - s for a in addrs[:50] for s in syms}, key=lambda d: sum((a - d) in syms for a in addrs[:200]))
    for kind, a in rows:
        name = syms.get(int(a, 16) - best)
        if name:
            (play if kind == "play" else init).add(name)

text = {n: v for n, v in arm.items() if any(t in "tT" for _, _, t in v)}


def size(names):
    return sum(s for n in names for _, s, t in arm.get(n, []) if t in "tT")


hot = {n for n in text if n in play}
initonly = {n for n in text if n in init and n not in play}
never = set(text) - hot - initonly
# the Flipper glue does not exist in the simulator: count it as hot
glue = {n for n, v in text.items() if any(o == "i_fz_flipper" for o, _, _ in v)}
hot |= glue - initonly
never -= glue

print(f"text total {size(text) / 1024:6.1f} KB")
print(f"hot (gameplay) {size(hot) / 1024:6.1f} KB in {len(hot)} functions")
print(f"init/level-load only {size(initonly) / 1024:6.1f} KB in {len(initonly)} functions")
print(f"never seen {size(never) / 1024:6.1f} KB in {len(never)} functions")

by = collections.defaultdict(lambda: [0, 0, 0])
for group, idx in ((hot, 0), (initonly, 1), (never, 2)):
    for n in group:
        for obj, s, t in arm[n]:
            if t in "tT":
                by[obj][idx] += s
print("\nper object (bytes): hot / init-only / never")
for obj, (h, i, nv) in sorted(by.items(), key=lambda kv: -sum(kv[1])):
    print(f"  {obj:16} {h:6} {i:6} {nv:6}")

ro = collections.Counter()
for n, v in arm.items():
    for obj, s, t in v:
        if t in "rR":
            ro[obj] += s
print("\nrodata per object (bytes)")
for obj, s in ro.most_common():
    print(f"  {obj:16} {s:6}")
