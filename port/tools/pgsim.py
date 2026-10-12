"""Replay a pager trace (sim FZ_PGTRACE) under different cache policies.

    pgsim.py FZDOOM.WAD pg1.bin
Prices misses like the sim: 0.9 ms + 1.1 us per byte of a 512 B page.
"""
import array
import collections
import struct
import sys

PAGE = 512
MISS_MS = 0.9 + PAGE * 0.0011

wad, trace = sys.argv[1:3]
data = open(wad, "rb").read()
_, n, _, ofs = struct.unpack_from("<4shhi", data, 0)
lumps = sorted((struct.unpack_from("<i", data, ofs + 16 * i)[0],
                data[ofs + 16 * i + 8: ofs + 16 * i + 16].rstrip(b"\0").decode("latin1")) for i in range(n))
dir_pages = set(range(ofs // PAGE, (ofs + 16 * n) // PAGE + 1))


def kind(page):
    if page in dir_pages:
        return "DIR"
    pos = page * PAGE
    name = "?"
    for p, nm in lumps:
        if p <= pos:
            name = nm
        else:
            break
    return name if name in ("FZTX", "NODES", "SEGS", "BLOCKMAP", "REJECT") else "other"


t = array.array("H")
t.frombytes(open(trace, "rb").read())
frames = []
cur = []
for p in t:
    if p == 0xFFFF:
        frames.append(cur)
        cur = []
    else:
        cur.append(p)
kinds = {p: kind(p) for p in set(t) if p != 0xFFFF}

# distinct pages per frame by kind
ws = collections.defaultdict(list)
for f in frames:
    c = collections.Counter(kinds[p] for p in set(f))
    for k in ("FZTX", "NODES", "SEGS", "BLOCKMAP", "REJECT", "DIR", "other"):
        ws[k].append(c[k])
print("distinct pages per frame (avg / p90 / max):")
for k, v in ws.items():
    s = sorted(v)
    print(f"  {k:8} {sum(v) / len(v):5.1f} {s[len(s) * 9 // 10]:4d} {s[-1]:4d}")
tot = sorted(len(set(f)) for f in frames)
print(f"  total    {sum(tot) / len(tot):5.1f} {tot[len(tot) * 9 // 10]:4d} {tot[-1]:4d}")
print("whole-run distinct pages by kind:", dict(collections.Counter(kinds.values())))


def lru(slots, pools=None):
    """pools: {kind-set: slots}, else one shared LRU"""
    caches = {}
    if pools:
        for ks, n in pools.items():
            pool = (collections.OrderedDict(), n)
            for k in ks:
                caches[k] = pool
    else:
        shared = (collections.OrderedDict(), slots)
    misses = 0
    for f in frames:
        for p in f:
            c, n = caches.get(kinds[p], caches.get("*")) if pools else shared
            if p in c:
                c.move_to_end(p)
            else:
                misses += 1
                c[p] = 1
                if len(c) > n:
                    c.popitem(last=False)
    return misses * MISS_MS / len(frames)


def opt(slots):
    """Belady: evict the page used furthest in future (lower bound)"""
    seq = [p for f in frames for p in f]
    nxt = [0] * len(seq)
    last = {}
    for i in range(len(seq) - 1, -1, -1):
        nxt[i] = last.get(seq[i], 1 << 30)
        last[seq[i]] = i
    import heapq
    cache = {}
    heap = []
    misses = 0
    for i, p in enumerate(seq):
        if p not in cache:
            misses += 1
            if len(cache) >= slots:
                while True:
                    negn, q = heapq.heappop(heap)
                    if cache.get(q) == -negn:
                        del cache[q]
                        break
        cache[p] = nxt[i]
        heapq.heappush(heap, (-nxt[i], p))
    return misses * MISS_MS / len(frames)


for s in (22, 32, 40, 48):
    print(f"LRU {s}: {lru(s):.2f} ms/frame   OPT {s}: {opt(s):.2f}")
MAP = {"NODES", "SEGS", "BLOCKMAP", "REJECT", "DIR", "other"}
for a in (8, 10, 12, 14):
    print(f"pools map {22 - a} + FZTX {a}: {lru(22, {frozenset(MAP): 22 - a, frozenset({'FZTX'}): a}):.2f}")

# FZTX at 4 bits per texel: the same columns in half the pages
fz = sorted(p for p, k in kinds.items() if k == "FZTX")
if fz:
    base = fz[0]
    frames[:] = [[(base + (p - base) // 2) if kinds[p] == "FZTX" else p for p in f] for f in frames]
    for p in fz:
        kinds[base + (p - base) // 2] = "FZTX"
    print("--- FZTX 4-bit")
    for s in (22,):
        print(f"LRU {s}: {lru(s):.2f}")
    for a in (6, 8, 10):
        print(f"pools map {22 - a} + FZTX {a}: {lru(22, {frozenset(MAP): 22 - a, frozenset({'FZTX'}): a}):.2f}")
    NM = MAP - {"NODES"}
    for nodes, rest, tx in ((12, 4, 6), (12, 2, 8), (11, 5, 6), (10, 6, 6)):
        print(f"pools NODES {nodes} + map {rest} + FZTX {tx}: "
              f"{lru(22, {frozenset({'NODES'}): nodes, frozenset(NM): rest, frozenset({'FZTX'}): tx}):.2f}")

print("--- budgets (4-bit FZTX)")
for total in (22, 28, 32, 36, 40):
    best = min((lru(total, {frozenset(MAP): total - a, frozenset({'FZTX'}): a}), a) for a in range(4, total - 8, 2))
    print(f"{total} slots: shared LRU {lru(total):.2f}, best pools {best[0]:.2f} (FZTX {best[1]})")
