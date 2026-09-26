#!/bin/sh
# Runs verify/publish_check.py over the repository that holds the ROM.
ROM="$M2M_ROM"
exec python3 "$(dirname "$0")/publish_check.py" "$(dirname "$(dirname "$ROM")")" "$ROM"
