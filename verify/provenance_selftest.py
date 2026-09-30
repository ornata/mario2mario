#!/usr/bin/env python3
"""Self-test for verify/provenance.py: each check must fire on a unit
carrying exactly the defect it exists to catch. Mutations are applied to
copies of real units in a temporary directory; nothing in gen/ changes.

Usage: provenance_selftest.py ROM   (run from the repo root)
"""
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import provenance  # noqa: E402

BASE = 'gen/units/unit_80247F08_641534AD.s'


def swap_groups(t):
    """Code motion: exchange the bodies of two adjacent non-transfer
    instructions (their labels and provenance comments stay put)."""
    parts = re.split(r'^(L_[0-9A-F]{8}:[^\n]*\n)', t, flags=re.M)
    for i in range(1, len(parts) - 3, 2):
        a, b = parts[i + 1], parts[i + 3]
        if 'M2M_READ' in a and 'M2M_' not in b and 'add   x28' in b:
            parts[i + 1], parts[i + 3] = b, a
            return ''.join(parts)
    raise RuntimeError('no swappable pair')


def hoist_across(t):
    """Code motion across instructions: move one instruction's memory
    access into the preceding instruction's range."""
    m = re.search(r'(\n)(L_[0-9A-F]{8}:[^\n]*\n)(\s+add\s+w0, [^\n]*\n\s+M2M_READ32 [^\n]*\n)', t)
    return t[:m.start()] + '\n' + m.group(3).lstrip('\n') + m.group(2) + t[m.end():]


MUTATIONS = [
    ('cfg', 'real divergence #3 output (fallthrough to B+0xC)',
     lambda t: open('gen/trial/opus_divergence3/unit_80247F08_641534AD.a2.s').read()),
    ('cfg', 'branch retargeted to another in-unit label',
     lambda t: re.sub(r'M2M_GOTO L_([0-9A-F]{8}), 0x\1', lambda m: 'M2M_GOTO L_80247F08, 0x80247F08', t, count=1)),
    ('order', 'two instruction ranges exchanged (labels reordered)',
     lambda t: t.replace('L_80247F0C:', 'L_TMP:').replace('L_80247F10:', 'L_80247F0C:').replace('L_TMP:', 'L_80247F10:')),
    ('motion', 'two instruction bodies exchanged', swap_groups),
    ('motion', 'memory access hoisted into the previous instruction', hoist_across),
    ('labels', 'a label not derived from an address',
     lambda t: re.sub(r'Lnt_([0-9A-F]{8})', r'Lskip\1x', t, count=2)),
    ('labels', 'local branch escaping its instruction',
     lambda t: re.sub(r'(cbz\s+w0, )Lnt_[0-9A-F]{8}', r'\1L_80247F08', t, count=1)),
    ('words', 'provenance comment word altered',
     lambda t: re.sub(r'(// 80247F10: )([0-9A-F]{8})', lambda m: m.group(1) + f'{int(m.group(2), 16) ^ 1:08X}', t, count=1)),
    ('words', 'footer word altered (not in ROM or listing)',
     lambda t: re.sub(r'(\.long 0x80247F10, 0x)([0-9A-F]{8})', lambda m: m.group(1) + f'{int(m.group(2), 16) ^ 1:08X}', t, count=1)),
    ('bounded', 'an instruction range padded past the bound',
     lambda t: t.replace('L_80247F10:', 'L_80247F10:', 1).replace(
         '\nL_80247F14:', '\n' + '    mov   x16, x16\n' * 30 + 'L_80247F14:', 1)),
]


def main():
    rom = provenance.rom_words(sys.argv[1])
    uid, _, _, _, _, errs = provenance.check_unit(BASE, rom)
    ok = not errs
    print(f'baseline {uid}: {"clean" if ok else errs[:3]}')
    tmp = tempfile.mkdtemp()
    try:
        for check, what, mut in MUTATIONS:
            path = os.path.join(tmp, os.path.basename(BASE))
            with open(path, 'w') as f:
                f.write(mut(open(BASE).read()))
            errs = provenance.check_unit(path, rom)[5]
            fired = bool(errs)
            ok &= fired
            print(f'{"caught " if fired else "MISSED "} [{check}] {what}: {errs[0] if errs else "-"}')
    finally:
        shutil.rmtree(tmp)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
