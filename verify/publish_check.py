#!/usr/bin/env python3
"""Publish-safety check: no ROM-derived content outside the ROM-derived paths.

  publish_check.py REPO_ROOT ROM_PATH

ROM-derived paths (excluded from publishing, and from this scan): gen/,
tests/goldens/, rom/, out/. Every other git-tracked file is scanned for hex
tokens (8 digits, optional 0x); a violation is a run of RUN or more tokens that
equal consecutive aligned words of the ROM (a copied code/data excerpt), or a
binary file whose bytes contain a RUN*4-byte window of the ROM. Exit status 1
lists the violations; 0 means the repo minus those paths is publishable as far
as ROM content goes.
"""
import re, subprocess, sys

RUN = 4
GAP = 2
EXCLUDED = ("gen/", "tests/goldens/", "rom/", "out/")

root, rom_path = sys.argv[1], sys.argv[2]
rom = open(rom_path, "rb").read()
words = [int.from_bytes(rom[i:i + 4], "big") for i in range(0, len(rom), 4)]
pos = {}
for i, w in enumerate(words):
    pos.setdefault(w, []).append(i)

files = subprocess.run(["git", "-C", root, "ls-files"], capture_output=True,
                       text=True, check=True).stdout.split()
bad = []
for f in files:
    if f.startswith(EXCLUDED):
        continue
    data = open(f"{root}/{f}", "rb").read()
    if b"\0" in data[:4096]:  # binary: look for a raw ROM window
        # low-entropy windows (zero fill, padding) are not evidence of copying
        probe = {w for w in (rom[i:i + RUN * 4] for i in range(0, 0x101000, 4 * 64))
                 if len(set(w)) > 4}
        if any(data[j:j + RUN * 4] in probe for j in range(0, len(data) - RUN * 4, 4)):
            bad.append((f, 0, "binary contains ROM bytes"))
        continue
    toks = [(m.start(), int(m.group(1), 16)) for m in
            re.finditer(r"(?<![0-9A-Fa-fx])(?:0x)?([0-9A-Fa-f]{8})(?![0-9A-Fa-f])",
                        data.decode("utf-8", "replace"))]
    i = 0
    while i < len(toks):
        best = 0
        # consecutive ROM words, allowing up to GAP unrelated tokens between
        # them (listing lines interleave addresses with the words)
        for p in pos.get(toks[i][1], [])[:64]:
            n, j = 1, i + 1
            while j < len(toks) and p + n < len(words):
                k = next((k for k in range(j, min(j + GAP + 1, len(toks)))
                          if toks[k][1] == words[p + n]), None)
                if k is None:
                    break
                n, j = n + 1, k + 1
            best = max(best, n)
        if best >= RUN:
            line = data[:toks[i][0]].count(b"\n") + 1
            bad.append((f, line, "%d consecutive ROM words" % best))
            i += 1
            while i < len(toks) and data[:toks[i][0]].count(b"\n") + 1 == line:
                i += 1
        else:
            i += 1
for f, line, why in bad:
    print("%s:%d: %s" % (f, line, why))
print("%d violation(s) outside %s" % (len(bad), ", ".join(EXCLUDED)))
sys.exit(1 if bad else 0)
