#!/usr/bin/env python3
"""SHARC chunk layout from a profile: how many chunk changes another layout would leave.

    python3 scripts/sharc_layout.py CFG DUMP [--cap N ...]

CFG:  m2sharcrecomp --cfg's output (each instruction's handler, whether it falls
      through to the next, its delay slots and constant target).
DUMP: a profiled build's SHARC dump (m2run M2_SHARC_DUMP=FILE, or the Vita's
      ux0:data/<set>/sharc_hot.txt): executions per instruction and the control
      transfers that are not a fall-through (> from to kind count).

The program's chunks (m2sharcrecomp --chunk, 256 instructions) are today cut by
address; a transfer between two chunks goes back to run_generated's switch and
into the other chunk's (a "chunk change"). This groups the instructions into
units that only fall through inside them (a unit is never split, but by a cap),
then merges units along their heaviest transfers while the chunk stays within
--cap instructions (Pettis-Hansen's greedy merge), and prints the chunk changes
a frame each layout would leave. Fall-through flow, not in the dump, is each
instruction's executions minus the transfers into it.

Nothing is changed here: it is the measurement before m2sharcrecomp's layout pass.
"""

import argparse
from collections import defaultdict


def read_cfg(path):
    cfg = {}
    for line in open(path):
        if line.startswith("#"):
            continue
        addr, handler, fall, delayed, target, kind = line.split()
        cfg[int(addr, 16)] = (handler, fall == "f", delayed == "j", kind)
    return cfg


def read_dump(path):
    counts, edges, header = {}, [], ""
    for line in open(path):
        if line.startswith("#"):
            header += line
            continue
        f = line.split()
        if f[0] == ">":
            frm = None if f[1] == "entry" else int(f[1], 16)
            edges.append((frm, int(f[2], 16), f[3], float(f[4])))
        else:
            counts[int(f[0], 16)] = float(f[1])
    return counts, edges, header


def units_of(cfg):
    """Maximal address ranges whose instructions only reach the next by falling through."""
    addrs = sorted(cfg)

    def reaches(p, a):  # can p's execution continue at a = p + 1 without a transfer?
        if a != p + 1:
            return False
        _, falls, delayed, _ = cfg[p]
        if not falls and not delayed:
            return False
        branch = cfg.get(p - 2)  # p the second delay slot of a branch that never falls through
        if branch and not branch[1] and branch[2]:
            return False
        return True

    unit, units, cur = {}, [], []
    for i, a in enumerate(addrs):
        if cur and not reaches(addrs[i - 1], a):
            units.append(cur)
            cur = []
        cur.append(a)
    units.append(cur)
    for n, u in enumerate(units):
        for a in u:
            unit[a] = n
    return units, unit


def fall_flows(cfg, counts, edges):
    """(a-1 -> a): executions of a not explained by a transfer into it."""
    into = defaultdict(float)
    for frm, to, kind, n in edges:
        into[to] += n
    flows = {}
    for a, n in counts.items():
        if a - 1 in cfg:
            f = n - into[a]
            if f > 0.5:
                flows[a] = f
    return flows


def transfers(edges, flows):
    """Every transfer (from, to, executions a frame): the dump's (but entries) and the fall-throughs."""
    out = [(frm, to, n) for frm, to, kind, n in edges if frm is not None]
    out += [(a - 1, a, f) for a, f in flows.items()]
    return out


def cost(chunk_of, xfers):
    return sum(n for frm, to, n in xfers if chunk_of(frm) != chunk_of(to))


def layout(units, unit, xfers, cap):
    """Greedy merge of the units along their heaviest transfers, within cap instructions a chunk."""
    size = [len(u) for u in units]
    group = list(range(len(units)))  # unit -> its group's representative
    members = {g: [g] for g in group}
    gsize = {g: size[g] for g in group}

    def find(u):
        while group[u] != u:
            group[u] = group[group[u]]
            u = group[u]
        return u

    weight = defaultdict(float)
    for frm, to, n in xfers:
        a, b = unit[frm], unit[to]
        if a != b:
            weight[(min(a, b), max(a, b))] += n
    # A unit longer than the cap is split at multiples of it (as today).
    for (a, b), w in sorted(weight.items(), key=lambda e: -e[1]):
        ga, gb = find(a), find(b)
        if ga == gb or gsize[ga] + gsize[gb] > cap:
            continue
        group[gb] = ga
        members[ga] += members.pop(gb)
        gsize[ga] += gsize.pop(gb)
    # Chunk index per address: each group's units in address order, cold units after, packed by cap.
    chunk = {}
    groups = sorted(members.values(), key=lambda m: min(units[u][0] for u in m))
    nchunk, fill = 0, 0
    for m in groups:
        n = sum(len(units[u]) for u in m)
        if fill and fill + n > cap:
            nchunk, fill = nchunk + 1, 0
        for u in sorted(m, key=lambda u: units[u][0]):
            for a in units[u]:
                chunk[a] = nchunk + (fill // cap)
                fill += 1
        if fill >= cap:
            nchunk, fill = nchunk + fill // cap + (1 if fill % cap else 0), 0
    return chunk, nchunk + (1 if fill else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cfg")
    ap.add_argument("dump")
    ap.add_argument("--cap", type=int, nargs="*", default=[256, 384, 512])
    ap.add_argument("--chunk", type=int, default=256, help="today's chunk size (m2sharcrecomp --chunk)")
    args = ap.parse_args()
    cfg = read_cfg(args.cfg)
    counts, edges, header = read_dump(args.dump)
    base = min(cfg)
    units, unit = units_of(cfg)
    flows = fall_flows(cfg, counts, edges)
    xfers = transfers(edges, flows)
    measured = sum(n for frm, to, kind, n in edges if kind == "c")
    by_address = cost(lambda a: (a - base) // args.chunk, xfers)
    hot = [u for u in units if any(counts.get(a, 0) > 0 for a in u)]
    print(f"{len(cfg)} instructions, {len(units)} units ({len(hot)} executed), "
          f"{sum(n for _, _, n in xfers):.0f} transfers a frame (fall-throughs included)")
    print(f"chunk changes a frame: measured {measured:.0f}; by address, {args.chunk} a chunk (model) {by_address:.0f}")
    for cap in args.cap:
        chunk, n = layout(units, unit, xfers, cap)
        c = cost(lambda a: chunk[a], xfers)
        print(f"  by affinity, at most {cap} a chunk: {c:.0f} ({100 * c / by_address if by_address else 0:.1f} %), {n} chunks")
    # The heaviest transfers that stay between chunks at the first cap.
    chunk, _ = layout(units, unit, xfers, args.cap[0])
    left = defaultdict(float)
    for frm, to, n in xfers:
        if chunk[frm] != chunk[to]:
            left[(frm, to)] += n
    print(f"heaviest left at {args.cap[0]}:")
    for (frm, to), n in sorted(left.items(), key=lambda e: -e[1])[:10]:
        print(f"  {frm:05x} {cfg[frm][0]:<28} -> {to:05x} {n:8.1f}")


if __name__ == "__main__":
    main()
