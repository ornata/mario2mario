#!/bin/sh
# //verify:provenance: the no-decompilation checker over gen/units/, then
# its self-test (every check must fire on a unit with that defect).
set -eu
if ! ls gen/units/*.s >/dev/null 2>&1; then
  echo "no translated units in gen/units/: they are ROM-derived and not shipped." >&2
  echo "Regenerate them from your own ROM by following PROMPT.md, then rerun." >&2
  exit 1
fi
python3 verify/provenance.py "$M2M_ROM" --out "${TEST_UNDECLARED_OUTPUTS_DIR:-/tmp}/provenance.tsv"
python3 verify/provenance_selftest.py "$M2M_ROM"
