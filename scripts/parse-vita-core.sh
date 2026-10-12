#!/bin/sh
# Parse a PS Vita coredump against this project's unstripped ELF.
# Usage: scripts/parse-vita-core.sh dump.psp2dmp [elf] [parser args...]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
VITASDK=${VITASDK:-/usr/local/vitasdk}
if [ -d "$VITASDK/bin" ]; then
    PATH="$VITASDK/bin:$PATH"
    export PATH
fi
VENV="$ROOT/tools/vita-parse-core/.venv"
if [ ! -x "$VENV/bin/python" ]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q -r "$ROOT/tools/vita-parse-core/requirements.txt"
fi
exec "$VENV/bin/python" "$ROOT/tools/vita-parse-core/main.py" "$@"
