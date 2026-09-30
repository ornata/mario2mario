#!/bin/sh
# //verify:coverage: every executed (vaddr, word) of the full route is
# covered by a translated unit with an ok provenance row.
set -eu
if ! ls gen/units/*.s >/dev/null 2>&1; then
  echo "no translated units in gen/units/: they are ROM-derived and not shipped." >&2
  echo "Regenerate them from your own ROM by following PROMPT.md, then rerun." >&2
  exit 1
fi
COVERAGE_OUT="${TEST_TMPDIR:-/tmp}/coverage.tsv" exec gen/work/coverage.sh
