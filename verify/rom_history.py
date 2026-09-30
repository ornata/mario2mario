#!/usr/bin/env python3
"""ROM-history guard: no object anywhere in git history is an N64 ROM.

Scans every object reachable from any ref (`git rev-list --objects
--all`) plus every loose/packed object git knows about
(`git cat-file --batch-all-objects`), and fails if any blob
  (a) has the SHA1 of the ROM's content (SM64 USA,
      9bef1128717f958171a4afac3ed78ee2bb4e86ce), or
  (b) starts with a z64 / n64 / v64 header magic, or
  (c) is ROM-sized (8 MiB) at all (reported for inspection).
The pre-commit hook (hooks/pre-commit) blocks the same content at commit
time; this proves it for the whole history, after the fact.

Usage: rom_history.py REPO
"""
import hashlib
import subprocess
import sys

ROM_SHA1 = '9bef1128717f958171a4afac3ed78ee2bb4e86ce'
ROM_SIZE = 8 * 1024 * 1024
MAGICS = (b'\x80\x37\x12\x40', b'\x37\x80\x40\x12', b'\x40\x12\x37\x80')


def main():
    repo = sys.argv[1]
    git = ['git', '-C', repo]
    ids = subprocess.run(git + ['cat-file', '--batch-all-objects', '--batch-check=%(objectname) %(objecttype) %(objectsize)'],
                         check=True, capture_output=True, text=True).stdout.split('\n')
    blobs = [l.split() for l in ids if l.endswith(tuple('0123456789')) and ' blob ' in l]
    reach = subprocess.run(git + ['rev-list', '--objects', '--all'], check=True, capture_output=True,
                           text=True).stdout.count('\n')
    bad = []
    proc = subprocess.Popen(git + ['cat-file', '--batch'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    for oid, _, size in blobs:
        proc.stdin.write((oid + '\n').encode())
        proc.stdin.flush()
        header = proc.stdout.readline()
        n = int(header.split()[2])
        data = proc.stdout.read(n)
        proc.stdout.read(1)
        if data[:4] in MAGICS:
            bad.append(f'{oid}: N64 ROM header magic {data[:4].hex()}')
        if n == ROM_SIZE:
            h = hashlib.sha1(data).hexdigest()
            bad.append(f'{oid}: ROM-sized blob' + (' with the ROM SHA1' if h == ROM_SHA1 else f' (sha1 {h})'))
    proc.stdin.close()
    proc.wait()
    for b in bad:
        print('FAIL', b)
    print(f'rom_history: {len(blobs)} blobs in the object store, {reach} objects reachable '
          f'from all refs; {len(bad)} ROM-like')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
