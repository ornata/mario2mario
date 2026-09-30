#!/bin/sh
# Runs verify/publish_check.py over the repository that holds the ROM: the
# tracked files, then every blob in the git history.
set -eu
ROM="$M2M_ROM"
REPO="$(dirname "$(dirname "$ROM")")"
python3 "$(dirname "$0")/publish_check.py" "$REPO" "$ROM"
python3 "$(dirname "$0")/publish_check.py" "$REPO" "$ROM" --history
