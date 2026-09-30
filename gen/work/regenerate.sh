#!/bin/sh
# Regenerates the translation-pipeline inputs from the ROM (recipe; the
# outputs are committed alongside it). Run from the repo root after
# `bazelisk build //oracle:run //tools:codemap //tools:unitrender`.
#
# 1. Full-route capture: every distinct (vaddr, word) executed by the oracle
#    over tests/inputs/gameplay.rec (10B instructions, ~90 s), plus the
#    oracle's cached lockstep references in out/ (git-ignored).
# 2. Remove the pairs already covered by translated units (their footer
#    tables), so existing translations are never re-requested.
# 3. codemap -> runs, unitrender -> per-unit worker inputs in gen/work/full.
set -eu
B=bazel-bin/src
mkdir -p out gen/work/runs_rest gen/work/full
$B/oracle/run --rom rom/mario64usa.z64 --max-insns 10000000000 \
  --replay tests/inputs/gameplay.rec \
  --exec-words gen/work/exec_words_full.txt \
  --checkpoints out/oracle_full_s1000.ckpt --checkpoint-stride 1000 \
  --task-hashes out/oracle_full_tasks.txt
cat gen/units/*.s | awk '/^[ \t]*\.long 0x[0-9A-F]+, 0x[0-9A-F]+,/ {
  gsub(",", ""); print $2, $3 }' | sed 's/0x//g' | sort -u > out/covered_pairs.txt
awk 'NR == FNR { have[$1 " " $2] = 1; next } !(($1 " " $2) in have)' \
  out/covered_pairs.txt gen/work/exec_words_full.txt \
  > gen/work/exec_words_remaining.txt
$B/tools/codemap gen/work/exec_words_remaining.txt gen/work/runs_rest
$B/tools/unitrender gen/work/runs_rest gen/work/full > gen/work/full/units.txt
