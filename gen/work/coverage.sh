#!/bin/sh
# Coverage ledger (recipe). Maps every distinct (vaddr, word) the oracle
# executed over the full gameplay.rec route (gen/work/exec_words_full.txt)
# to the translated unit rows that cover it (unit footer tables in
# gen/units/*.s), and joins each unit with its final provenance from
# gen/worker_ledger.tsv (last row per unit). Writes gen/coverage.tsv (one
# row per unit) and prints a summary; exits 1 if any executed pair is not
# covered. "Verified" is the full-route lockstep: native executes every
# covered pair and matches the oracle byte-for-byte end to end
# (//src/native:full_route_lockstep_test). COVERAGE_OUT overrides the
# output path (//verify:coverage writes to the test's temp dir).
set -eu
cd "$(dirname "$0")/../.."
COVERAGE_OUT="${COVERAGE_OUT:-gen/coverage.tsv}" python3 - <<'PY'
import glob, os, re, sys
cover = {}
units = {}
for p in sorted(glob.glob('gen/units/*.s')):
    u = p.split('unit_')[1][:-2]
    rows = re.findall(r'^\s*\.long 0x([0-9A-F]{8}), 0x([0-9A-F]{8}), (L_|-1)', open(p).read(), re.M)
    units[u] = {'pcs': sum(1 for r in rows if r[2] == 'L_'), 'slots': sum(1 for r in rows if r[2] == '-1'), 'hit': 0}
    for pc, w, k in rows:
        cover.setdefault((pc, w), []).append(u)
prov = {}
for l in open('gen/worker_ledger.tsv'):
    f = l.rstrip('\n').split('\t')
    if len(f) == 9 and f[0] != 'unit' and not f[0].startswith('#') and f[6].startswith('ok'):
        prov[f[0]] = (f[8], f[5], f[7])
executed = [tuple(l.split()[:2]) for l in open('gen/work/exec_words_full.txt') if l.strip()]
missing = [e for e in executed if e not in cover]
for e in executed:
    for u in cover.get(e, []):
        units[u]['hit'] += 1
with open(os.environ['COVERAGE_OUT'], 'w') as out:
    out.write('unit\ttranslated_pcs\tdelay_slot_rows\texecuted_pairs_covered\tmodel\tfinal_attempt\thand\n')
    for u, d in sorted(units.items()):
        m, a, h = prov.get(u, ('?', '?', '?'))
        out.write(f"{u}\t{d['pcs']}\t{d['slots']}\t{d['hit']}\t{m}\t{a}\t{h}\n")
by = {}
for u in units:
    by[prov.get(u, ('?',))[0]] = by.get(prov.get(u, ('?',))[0], 0) + 1
print(f'executed pairs: {len(executed)}; covered: {len(executed) - len(missing)}; uncovered: {len(missing)}')
print(f'units: {len(units)}; by model: {by}; no ok provenance row: {[u for u in units if u not in prov]}')
for e in missing[:20]:
    print('UNCOVERED', *e)
sys.exit(1 if missing or any(u not in prov for u in units) else 0)
PY
