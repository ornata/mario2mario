#!/usr/bin/env python3
"""Publish-safety check: no ROM-derived content in the repository.

  publish_check.py REPO_ROOT ROM_PATH [--history]

Scans every git-tracked file of the working tree (default) or, with
--history, every blob reachable from any ref, except the never-tracked
rom/ and out/. Text files are scanned for hex tokens (8 digits, optional
0x); a violation is a run of RUN or more tokens that equal consecutive
aligned words of the ROM (a copied code/data excerpt). Binary files are
violations if they contain a RUN*4-byte window of the ROM.

Known, deliberate short excerpts (decoder golden vectors, the worked
examples of PROMPT-translate.md) are allowed line by line through
verify/publish_allowlist.tsv: path and the SHA-256 of the whole line.
Anything else, including an allowed line in another file, fails.
Exit status 1 lists the violations.
"""
import hashlib
import os
import re
import subprocess
import sys

RUN = 4
GAP = 2
EXCLUDED = ("rom/", "out/")
TOKEN = re.compile(r"(?<![0-9A-Fa-fx])(?:0x)?([0-9A-Fa-f]{8})(?![0-9A-Fa-f])")


def load_allow(root):
    allow = set()
    for line in open(os.path.join(root, "verify", "publish_allowlist.tsv")):
        if line.strip() and not line.startswith("#"):
            path, digest = line.split("\t")[:2]
            allow.add((path, digest.strip()))
    return allow


def rom_index(rom):
    words = [int.from_bytes(rom[i:i + 4], "big") for i in range(0, len(rom), 4)]
    pos = {}
    for i, w in enumerate(words):
        pos.setdefault(w, []).append(i)
    probe = {w for w in (rom[i:i + RUN * 4] for i in range(0, 0x101000, 4 * 64))
             if len(set(w)) > 4}
    return words, pos, probe


def scan(data, words, pos, probe):
    """Returns [(line_number, line_text, why)] for one file's bytes."""
    if b"\0" in data[:4096]:  # binary: look for a raw ROM window
        if any(data[j:j + RUN * 4] in probe for j in range(0, len(data) - RUN * 4, 4)):
            return [(0, None, "binary contains ROM bytes")]
        return []
    text = data.decode("utf-8", "replace")
    lines = text.split("\n")
    toks = [(m.start(), int(m.group(1), 16)) for m in TOKEN.finditer(text)]
    found, i = [], 0
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
            ln = text.count("\n", 0, toks[i][0])
            found.append((ln + 1, lines[ln], "%d consecutive ROM words" % best))
            i += 1
            while i < len(toks) and text.count("\n", 0, toks[i][0]) == ln:
                i += 1
        else:
            i += 1
    return found


def main():
    root, rom_path = sys.argv[1], sys.argv[2]
    history = "--history" in sys.argv[3:]
    words, pos, probe = rom_index(open(rom_path, "rb").read())
    allow = load_allow(root)
    git = ["git", "-C", root]
    if history:
        objs = subprocess.run(git + ["rev-list", "--objects", "--all"], capture_output=True,
                              text=True, check=True).stdout.split("\n")
        items, seen = [], set()
        for l in objs:
            oid, _, path = l.partition(" ")
            if path and (oid, path) not in seen:
                seen.add((oid, path))
                items.append((path, oid))
        kinds = subprocess.run(git + ["cat-file", "--batch-check=%(objectname) %(objecttype)"],
                               input="\n".join(o for _, o in items), capture_output=True,
                               text=True, check=True).stdout.split("\n")
        blob = {k.split()[0] for k in kinds if k.endswith(" blob")}
        items = [(p, o) for p, o in items if o in blob]
        read = lambda p, o: subprocess.run(git + ["cat-file", "blob", o], capture_output=True,
                                           check=True).stdout
    else:
        files = subprocess.run(git + ["ls-files"], capture_output=True, text=True,
                               check=True).stdout.split("\n")
        items = [(f, None) for f in files if f]
        read = lambda p, o: open(os.path.join(root, p), "rb").read()
    bad, allowed, cache = [], 0, {}
    for path, oid in items:
        if path.startswith(EXCLUDED):
            continue
        key = oid or path
        if key not in cache:
            cache[key] = scan(read(path, oid), words, pos, probe)
        for ln, text, why in cache[key]:
            if text is not None and (path, hashlib.sha256(text.encode()).hexdigest()) in allow:
                allowed += 1
                continue
            bad.append((path, oid, ln, why))
    for path, oid, ln, why in bad:
        print("%s%s:%d: %s" % (path, "@" + oid[:10] if oid else "", ln, why))
    scope = "%d blobs in all history" % len(items) if history else "%d tracked files" % len(items)
    print("%s: %d violation(s), %d allowlisted excerpt line(s) (verify/publish_allowlist.tsv)"
          % (scope, len(bad), allowed))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
