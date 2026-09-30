#!/bin/sh
# //verify:provenance: the no-decompilation checker over gen/units/, then
# its self-test (every check must fire on a unit with that defect).
set -eu
python3 verify/provenance.py "$M2M_ROM" --out "${TEST_UNDECLARED_OUTPUTS_DIR:-/tmp}/provenance.tsv"
python3 verify/provenance_selftest.py "$M2M_ROM"
