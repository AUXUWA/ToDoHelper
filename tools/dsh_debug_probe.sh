#!/usr/bin/env bash
# Find the device's current HTTP auth token, so a script can drive
# /api/v1/debug/exec (whose routes always require auth) without the caller
# having to juggle tokens - "生成并开启 token" deliberately rotates it.
#
# Two sources. **NVS is authoritative** (it is what the device actually compares
# against), so it is tried first; the log is the fallback for when the serial port
# is busy or absent. Reading flash no longer disturbs the running device because
# `--before no-reset --after no-reset` suppresses esptool's RTS/EN pulse - the
# default reset used to reboot the board between two commands of a test sequence
# and made a page switch look like it "reverted to home".
#
#   1. **The NVS partition image** - always current.
#   2. **The device's own serial log** - `auth_enable_cb()` logs the token once
#      when it generates it (the documented way to read it off the board;
#      requirement 8.8 only forbids routine logs and backups). Stale after a
#      rotation, hence second.
#
# Usage:  TOK=$(bash tools/dsh_debug_probe.sh --token [COM34])
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${2:-COM34}"
PY=/c/Espressif/tools/python/v6.1/venv/Scripts/python.exe

# --- 2. newest capture that contains a token line -------------------------
from_log() {
    local f
    # Newest first: the board rotates its token, so an old capture is stale.
    for f in $(ls -t "$ROOT"/build/*.log 2>/dev/null); do
        local t
        t=$(sed -n 's/.*new HTTP auth token: \([0-9a-f]\{32\}\).*/\1/p' "$f" 2>/dev/null | tail -1)
        if [ -n "$t" ]; then echo "$t"; return 0; fi
    done
    return 1
}

# --- 2. read it out of NVS -------------------------------------------------
from_nvs() {
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN

    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \
      "Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; \
       . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1' *> \$null; \
       & 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' -m esptool \
         --chip esp32s31 -p $PORT -b 921600 \
         --before no-reset --after no-reset \
         read-flash 0x9000 0x6000 '$(cygpath -w "$tmp/nvs.bin")' *> \$null" >/dev/null 2>&1 || true

    [ -s "$tmp/nvs.bin" ] || return 1

    "$PY" - "$(cygpath -w "$tmp/nvs.bin")" <<'PY'
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()

# The NVS entry CRC is zlib's CRC32 *without* the final inversion; verified by
# recomputing the CRCs of the entries in a real image. Entries are append-only
# (an overwrite writes a new slot), so the LAST valid one is the live value.
def nvs_crc(b):
    return zlib.crc32(b, 0xFFFFFFFF)

best = None
for page in range(len(d) // 4096):
    base = page * 4096
    for i in range(126):                      # 4096/32 = 128, last 2 are page state
        o = base + i * 32
        raw = d[o:o + 32]
        if raw[1] != 0x21 or raw[2] == 0 or i + raw[2] > 126:
            continue                          # 0x21 = string, span = chunk count
        if raw[8:24].split(b'\x00')[0] != b'auth_token':
            continue
        size, _res, dcrc = struct.unpack('<HHI', raw[24:32])
        full = b''.join(d[o + k * 32:o + k * 32 + 32] for k in range(1, raw[2]))[:size]
        if nvs_crc(full) == dcrc:
            best = full[:-1].decode()         # drop the NUL
print(best or '')
PY
}

tok="$(from_nvs || true)"
if [ -z "$tok" ]; then
    tok="$(from_log || true)"
fi

# Only the token may reach stdout: PowerShell prints a banner, and a caller doing
# `TOK=$(...)` would otherwise capture the banner instead.
printf '%s\n' "$(printf '%s' "$tok" | tr -dc '0-9a-f')"
