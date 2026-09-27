#!/usr/bin/env bash
# Run idf.py (build/flash/monitor/...) for ToDoHelper from Git-Bash / MSYS.
#
#   tools/build.sh                 -> idf.py build
#   tools/build.sh flash           -> idf.py flash
#   tools/build.sh -v build        -> verbose build
#   tools/build.sh --version       -> idf.py --version
#
# Anything after the script name is forwarded to idf.py verbatim.
#
# WHY THE POWERSHELL HOP
# ----------------------
# `idf.py` refuses to run under MSYS ("MSys/Mingw is no longer supported"), so
# the work is delegated to a native powershell.exe which sources the ESP-IDF
# profile. See tools/build.ps1 for the details -- in particular for why the
# MSYSTEM variable must be removed *inside* PowerShell (MSYS re-injects it into
# every native child) and why the environment must NOT be scrubbed with `env -i`
# (that made python emit no output and left the exit code unset, producing an
# empty build log with a bogus success status).
#
# This script does not use `set -e`: the whole point is to relay idf.py's own
# exit status faithfully, so the exit code is captured explicitly below.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"

# Windows-style path, because powershell.exe needs a native path for -File.
WIN_SCRIPT="$(cd "$HERE" && pwd -W)/build.ps1"

# Prefer the OS-provided powershell.exe; fall back to the canonical absolute path.
POWERSHELL="/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
if [ ! -x "$POWERSHELL" ]; then
    POWERSHELL="$(command -v powershell.exe || true)"
fi
if [ -z "${POWERSHELL:-}" ] || [ ! -x "$POWERSHELL" ]; then
    echo "build.sh: could not find powershell.exe" >&2
    exit 127
fi

cd "$PROJECT" || exit 1

"$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$WIN_SCRIPT" "$@"
exit $?
