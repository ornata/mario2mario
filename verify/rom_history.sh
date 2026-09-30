#!/bin/sh
# //verify:rom_history: no ROM-like blob anywhere in git history. Reads the
# repository that holds the ROM (rom/ is its child), so it runs unsandboxed.
ROM="$M2M_ROM"
exec python3 "$(dirname "$0")/rom_history.py" "$(dirname "$(dirname "$ROM")")"
