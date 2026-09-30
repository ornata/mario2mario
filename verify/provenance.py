#!/usr/bin/env python3
"""Provenance verifier: the machine-checkable no-decompilation evidence.

Checks every translated unit in gen/units/ against its own footer table
(the (vaddr, word) rows the runtime links by), its rendered listing
(gen/work/**/<id>.in.txt) and the ROM image. It needs no knowledge of the
game, only the MIPS instruction encoding and the translation contract's
macro vocabulary. It is a checker; it never emits or rewrites code.

Per unit it proves:
  order      every footer row has exactly one `L_<pc>:` label, and the
             labels appear in footer (= listing) order: source order.
  bounded    the AArch64 range of each MIPS instruction (from its label
             to the next `L_` label) holds at most MAX_RANGE instructions
             (a macro counts as one).
  labels     every defined label is address-derived: `L_<pc>` or
             `L<tag>_<pc>` for a pc of the unit, and every local branch
             (b, b.<cc>, cbz, cbnz, tbz, tbnz) stays inside the range of
             the MIPS instruction (or branch/delay-slot pair) that owns it.
  motion     no code motion across instructions: each range retires
             exactly once (`add x28, x28, #1`), and every macro that
             carries a pc (memory, COP0/COP1, FPU, TLB, RAISE, MISSING)
             names the pc of the instruction whose range it sits in (a
             branch-likely's MISSING names its absent slot, B+4).
  cfg        the control-flow edges emitted for each MIPS instruction
             equal the edges of the MIPS CFG decoded here from the word:
             fallthrough P+4 for non-transfers; {T, B+8} for conditional
             branches (the statically dead side may be omitted);
             {T | missing-slot, B+8} for branch-likely; {T} for j/jal;
             indirect for jr/jalr; eret; exception for syscall/break.
             In-unit edges must use the label of the target pc.
  words      every provenance comment `// <pc>: <word>` matches the
             footer row, the rendered listing, and the ROM: each maximal
             run of consecutive pcs is found as a contiguous word-aligned
             byte run in the ROM image.

Usage: provenance.py ROM [--out TSV]   (run from the repo root)
Exit status 0 iff every unit passes every check.
"""
import glob
import os
import re
import sys

MAX_RANGE = 24

LABEL = re.compile(r'^(\w+):')
PROV = re.compile(r'//\s*([0-9A-F]{8}):\s*([0-9A-F]{8})\b')
ROW = re.compile(r'^\s*\.long 0x([0-9A-F]{8}), 0x([0-9A-F]{8}), (L_[0-9A-F]{8}|-1)', re.M)
LOCAL_BR = re.compile(r'^(b|b\.\w+|cbz|cbnz|tbz|tbnz)\s+(.*)$')
# Macros that carry the pc of their MIPS instruction: name -> (index of
# the pc argument, index of the delay-slot flag or None), per the macro
# table in PROMPT-translate.md / src/native/m2m.inc.
PC_ARGS = {'M2M_COP1': (0, 1), 'M2M_CU1': (0, 1), 'M2M_FPU_BEGIN': (0, 1),
           'M2M_FPU_END': (0, 1), 'M2M_MFC0': (1, 2), 'M2M_MTC0': (1, 2),
           'M2M_CFC1': (1, 2), 'M2M_CTC1': (1, 2), 'M2M_TLB': (1, None),
           'M2M_RAISE': (1, 2), 'M2M_ERET': (0, None)}
for _w in (8, 16, 32, 64):
    PC_ARGS[f'M2M_READ{_w}'] = (0, 1)
    PC_ARGS[f'M2M_WRITE{_w}'] = (0, 1)
EDGE_MACROS = ('M2M_GOTO', 'M2M_GOTO_FAR', 'M2M_GOTO_W0', 'M2M_FALL',
               'M2M_FALLTHROUGH', 'M2M_MISSING', 'M2M_ERET')


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode_cti(pc, w):
    """MIPS control-transfer decode (R4300i encoding). Returns
    (kind, target, likely, dead) with kind in None, 'br', 'j', 'jr',
    'eret', 'exc'; dead is 'nt' (never falls through) or 't' (never
    taken) for statically decided branches."""
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    btarget = (pc + 4 + (sext16(w & 0xFFFF) << 2)) & 0xFFFFFFFF
    if op == 0:
        f = w & 63
        if f in (8, 9):
            return 'jr', None, False, None
        if f in (12, 13):
            return 'exc', None, False, None
        return None, None, False, None
    if op == 1:
        if rt in (0, 1, 2, 3, 16, 17, 18, 19):
            ge = rt & 1
            dead = ('nt' if ge else 't') if rs == 0 else None
            return 'br', btarget, bool(rt & 2), dead
        return None, None, False, None
    if op in (2, 3):
        return 'j', ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2), False, None
    if op in (4, 5, 6, 7, 20, 21, 22, 23):
        base = op & ~16
        dead = None
        if base == 4 and rs == rt:
            dead = 'nt'
        elif base == 5 and rs == rt:
            dead = 't'
        return 'br', btarget, op >= 20, dead
    if op == 16 and rs == 16 and (w & 63) == 0x18:
        return 'eret', None, False, None
    if op == 17 and rs == 8:
        return 'br', btarget, bool(rt & 2), None
    return None, None, False, None


def rom_words(path):
    data = open(path, 'rb').read()
    return data


def find_run(rom, words):
    pat = b''.join(w.to_bytes(4, 'big') for w in words)
    i = rom.find(pat)
    while i != -1 and i % 4:
        i = rom.find(pat, i + 1)
    return i


def listing_words(uid):
    for p in glob.glob(f'gen/work/**/{uid}.in.txt', recursive=True):
        t = open(p).read()
        lst = t.split('=== LISTING ===\n')[1].split('=== HEADER')[0]
        return {l[0:8]: l[10:18] for l in lst.splitlines() if l and not l.startswith('---')}
    return None


def check_unit(path, rom):
    uid = os.path.basename(path)[5:-2]
    errs = []
    text = open(path).read()
    body, _, tail = text.partition('.section __TEXT,__const')
    rows = [(int(a, 16), int(w, 16), k != '-1') for a, w, k in ROW.findall(tail)]
    pcs = [r[0] for r in rows]
    word = {r[0]: r[1] for r in rows}
    pcset = set(pcs)
    if len(pcset) != len(pcs):
        errs.append('duplicate footer rows')

    # Split the body into per-instruction ranges at L_<pc> labels.
    ranges = []  # (pc, [lines])
    cur = None
    for line in body.splitlines():
        code = line.split('//')[0].strip()
        m = LABEL.match(code)
        if m and re.fullmatch(r'L_[0-9A-F]{8}', m.group(1)):
            pc = int(m.group(1)[2:], 16)
            pm = PROV.search(line)
            if not pm or int(pm.group(1), 16) != pc:
                errs.append(f'L_{pc:08X}: provenance comment missing or names another pc')
            elif int(pm.group(2), 16) != word.get(pc, -1):
                errs.append(f'L_{pc:08X}: provenance word {pm.group(2)} != footer')
            cur = (pc, [])
            ranges.append(cur)
            continue
        if cur is not None and code:
            cur[1].append(code)

    # order
    got = [r[0] for r in ranges]
    if got != pcs:
        errs.append(f'label order differs from footer order ({len(got)} labels, {len(pcs)} rows)')
    rng = {pc: lines for pc, lines in ranges}

    # words vs listing and ROM
    lw = listing_words(uid)
    if lw is None:
        errs.append('no rendered listing found')
    else:
        for pc in pcs:
            if lw.get(f'{pc:08X}') != f'{word[pc]:08X}':
                errs.append(f'{pc:08X}: footer word differs from listing')
                break
    runs, start = [], 0
    for i in range(1, len(pcs) + 1):
        if i == len(pcs) or pcs[i] != pcs[i - 1] + 4:
            runs.append(pcs[start:i])
            start = i
    unfound = 0
    for run in runs:
        if find_run(rom, [word[p] for p in run]) < 0:
            unfound += 1
            errs.append(f'run {run[0]:08X}..{run[-1]:08X} ({len(run)} words) not found in ROM')

    # delay slots: the pc after any branch/jump present in the unit
    slots = {pc + 4 for pc in pcs if decode_cti(pc, word[pc])[0] in ('br', 'j', 'jr')
             and pc + 4 in pcset}

    # labels, local branches, bounded, motion
    owner = {}  # local label -> owning region pc
    for pc, lines in ranges:
        n = 0
        retires = 0
        for c in lines:
            m = LABEL.match(c)
            if m:
                lab = m.group(1)
                lm = re.fullmatch(r'L([a-z]*)_([0-9A-F]{8})', lab)
                if not lm or int(lm.group(2), 16) not in pcset:
                    errs.append(f'{pc:08X}: label {lab} is not address-derived')
                owner[lab] = pc
                continue
            if c.startswith('.'):
                errs.append(f'{pc:08X}: directive inside code: {c}')
                continue
            n += 1
            if re.fullmatch(r'add\s+x28,\s*x28,\s*#1', c):
                retires += 1
            mm = re.match(r'(M2M_\w+)\s*(.*)', c)
            if mm and mm.group(1) in PC_ARGS:
                args = [a.strip() for a in mm.group(2).split(',')]
                ip, ids = PC_ARGS[mm.group(1)]
                a = re.fullmatch(r'0x([0-9A-F]{8})', args[ip]) if ip < len(args) else None
                if not a or int(a.group(1), 16) != pc:
                    errs.append(f'{pc:08X}: {mm.group(1)} names another pc ({mm.group(2)})')
                elif ids is not None and (ids >= len(args) or args[ids] != ('1' if pc in slots else '0')):
                    errs.append(f'{pc:08X}: {mm.group(1)} delay-slot flag {args[ids:ids + 1]} wrong')
        if n > MAX_RANGE:
            errs.append(f'{pc:08X}: range of {n} instructions exceeds {MAX_RANGE}')
        kind = decode_cti(pc, word[pc])[0]
        if retires != 1 and kind != 'exc':
            errs.append(f'{pc:08X}: {retires} retires in range')

    # region of each pc: its own range, extended over the delay slot for a
    # CTI whose slot follows it in the unit.
    region = {}
    idx = {pc: i for i, pc in enumerate(got)}
    for i, pc in enumerate(got):
        kind, tgt, likely, dead = decode_cti(pc, word[pc])
        if kind in ('br', 'j', 'jr') and i + 1 < len(got) and got[i + 1] == pc + 4:
            region[pc] = [pc, pc + 4]
        else:
            region.setdefault(pc, [pc])
    slot_of = {r[1]: r[0] for r in region.values() if len(r) == 2}

    for pc in got:
        if pc in slot_of:
            continue  # checked as part of its branch
        reg = region[pc]
        lines = [c for p in reg for c in rng[p]]
        # local branches stay in the region
        for c in lines:
            m = LOCAL_BR.match(c)
            if m:
                tgt = m.group(2).split(',')[-1].strip()
                if owner.get(tgt) not in reg:
                    errs.append(f'{pc:08X}: local branch to {tgt} leaves the instruction region')
        # emitted edges
        edges = set()
        for c in lines:
            mm = re.match(r'(M2M_\w+)\s*(.*)', c)
            if not mm or mm.group(1) not in EDGE_MACROS:
                continue
            mac, arg = mm.groups()
            if mac == 'M2M_GOTO':
                g = re.match(r'L_([0-9A-F]{8}),\s*0x([0-9A-F]{8})', arg)
                if not g or g.group(1) != g.group(2) or int(g.group(2), 16) not in pcset:
                    errs.append(f'{pc:08X}: M2M_GOTO {arg} is not an in-unit label of its target')
                    continue
                edges.add(int(g.group(2), 16))
            elif mac in ('M2M_GOTO_FAR', 'M2M_FALL', 'M2M_FALLTHROUGH'):
                edges.add(int(arg[2:10], 16))
            elif mac == 'M2M_GOTO_W0':
                edges.add('indirect')
            elif mac == 'M2M_MISSING':
                edges.add('missing')
                if int(arg[2:10], 16) != pc + 4:
                    errs.append(f'{pc:08X}: M2M_MISSING names {arg[:10]}, not the slot')
            elif mac == 'M2M_ERET':
                edges.add('eret')
        last = reg[-1]
        nxt = last + 4
        if idx[last] + 1 < len(got) and got[idx[last] + 1] == nxt:
            implicit = nxt  # falls into the next range
        else:
            implicit = None
        kind, tgt, likely, dead = decode_cti(pc, word[pc])
        if kind is None or kind == 'exc':
            want = {pc + 4}
            if implicit is not None:
                edges.add(implicit)
            if kind == 'exc':
                want = edges if edges <= {pc + 4} else {pc + 4}
        elif kind == 'eret':
            want = {'eret'}
        elif kind == 'jr':
            want = {'indirect'}
        elif kind == 'j':
            want = {tgt}
        else:  # conditional branch
            taken = tgt if len(reg) == 2 else ('missing' if likely else None)
            want = {taken, pc + 8}
            if taken is None:
                errs.append(f'{pc:08X}: branch without its delay slot in the unit')
            # A statically decided branch may omit its dead side (unless
            # both sides are the same address, e.g. beq $0,$0,B+8).
            if dead == 'nt' and taken != pc + 8:
                edges.discard(pc + 8)
                want.discard(pc + 8)
            if dead == 't' and taken != pc + 8:
                edges.discard(taken)
                want.discard(taken)
        if edges != want:
            fmt = lambda s: sorted(x if isinstance(x, str) else f'{x:08X}' for x in s)
            errs.append(f'{pc:08X}: CFG edges {fmt(edges)} != MIPS {fmt(want)}')
    return uid, len(pcs), len(runs), unfound, max((len(l) for _, l in ranges), default=0), errs


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rom = rom_words(sys.argv[1])
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
    units = sorted(glob.glob('gen/units/unit_*.s'))
    total_pcs = total_runs = failed = 0
    rows = []
    for p in units:
        uid, npcs, nruns, unfound, maxr, errs = check_unit(p, rom)
        total_pcs += npcs
        total_runs += nruns
        rows.append((uid, npcs, nruns, maxr, len(errs)))
        if errs:
            failed += 1
            for e in errs[:8]:
                print(f'FAIL {uid}: {e}')
    if out:
        with open(out, 'w') as f:
            f.write('unit\tmips_instructions\trom_runs\tmax_range\tviolations\n')
            for r in rows:
                f.write('\t'.join(map(str, r)) + '\n')
    print(f'provenance: {len(units)} units, {total_pcs} MIPS instructions, '
          f'{total_runs} ROM runs; checks: order, bounded (<= {MAX_RANGE}), '
          f'labels, motion, cfg, words; {failed} unit(s) failing')
    return 1 if failed or not units else 0


if __name__ == '__main__':
    sys.exit(main())
