#!/usr/bin/env bash
#
# GYRO SNAKE desktop simulator - Git Bash / POSIX launcher.
#
# 1. Build desktop/build/gyro_snake.dll when it is missing.
# 2. Probe a Python that really has tkinter + Pillow (python, py -3, python3).
# 3. Run desktop/play.py from the gyro-snake project root.

set -eu

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"
cd "$ROOT"

if [ ! -s desktop/build/gyro_snake.dll ]; then
    if [ ! -f desktop/build.sh ]; then
        echo "play.sh: desktop/build.sh is missing." >&2
        echo "         restore it from the repository, then rerun." >&2
        exit 1
    fi
    echo "[GYRO SNAKE] gyro_snake.dll missing; building with desktop/build.sh ..."
    if ! bash desktop/build.sh; then
        echo "play.sh: build failed. See the compiler output above." >&2
        exit 1
    fi
fi

if [ ! -s desktop/build/gyro_snake.dll ]; then
    echo "play.sh: build finished but desktop/build/gyro_snake.dll is missing." >&2
    exit 1
fi

PY_CMD=()
if command -v python >/dev/null 2>&1 \
   && python -c "import tkinter, PIL" >/dev/null 2>&1; then
    PY_CMD=(python)
elif command -v py >/dev/null 2>&1 \
   && py -3 -c "import tkinter, PIL" >/dev/null 2>&1; then
    PY_CMD=(py -3)
elif command -v python3 >/dev/null 2>&1 \
   && python3 -c "import tkinter, PIL" >/dev/null 2>&1; then
    PY_CMD=(python3)
fi

if [ "${#PY_CMD[@]}" -eq 0 ]; then
    echo "play.sh: no Python 3 with tkinter + Pillow found." >&2
    echo "         need Python 3.10+, Tk 8.6, Pillow 9+ (see desktop/README.md)" >&2
    exit 1
fi

exec "${PY_CMD[@]}" desktop/play.py "$@"