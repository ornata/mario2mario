#!/bin/sh
# //verify:coverage: every executed (vaddr, word) of the full route is
# covered by a translated unit with an ok provenance row.
set -eu
COVERAGE_OUT="${TEST_TMPDIR:-/tmp}/coverage.tsv" exec gen/work/coverage.sh
