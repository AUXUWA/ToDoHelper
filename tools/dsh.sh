#!/usr/bin/env bash
# Drive the on-device debug shell (components/debug_shell).
#
#   bash tools/dsh.sh <command...>        # run one shell command
#   bash tools/dsh.sh --raw 'widgets'     # print the raw JSON envelope
#
# Env overrides: DSH_IP, DSH_PORT (default COM34), DSH_TOKEN (skip discovery).
#
# ## Why this wrapper is not just a curl
#
# The shell's routes always require auth (that is deliberate - see
# components/debug_shell/README.md), and two normal things rotate the token while
# you work:
#
#   * `click 生成并开启` runs the settings handler, which generates a NEW token
#     as its whole purpose;
#   * a reboot leaves the token unchanged, but the board may take a new DHCP lease.
#
# So a naive `curl -H X-Auth-Token:$T` breaks in the middle of any sequence that
# exercises that dialog. This wrapper therefore:
#
#   1. discovers the token afresh for every call (NVS image, then logs);
#   2. captures the serial port *while the command runs*, so a token the command
#      just rotated is picked up from the device's own log line;
#   3. retries once with the refreshed token;
#   4. reads the device IP from the newest capture (a stale address just times out).
#
# Point (2) is the one that matters for UI work: it makes "click the button that
# generates a token, then inspect the dialog it opened" a single command.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${DSH_PORT:-COM34}"
DSH_TMP=""
DSH_TOKEN_CACHE=""
PY=/c/Espressif/tools/python/v6.1/venv/Scripts/python.exe
# Tokens this wrapper has seen work. Persisted because the device only logs a
# token when it *generates* one, so a token set before the wrapper ever ran (or
# one whose log line the serial listener missed) is otherwise unknowable without
# rebooting the board to read NVS.
TOKEN_CACHE="$ROOT/build/.dsh_tokens"

# --- token discovery -------------------------------------------------------

# Every distinct token that ever appeared in a capture or the cache,
# newest first.
candidate_tokens() {
    {
        [ -f "$TOKEN_CACHE" ] && tac "$TOKEN_CACHE" 2>/dev/null
        local f
        for f in $(ls -t "$ROOT"/build/*.log 2>/dev/null); do
            sed -n 's/.*new HTTP auth token: \([0-9a-f]\{32\}\).*/\1/p' "$f" 2>/dev/null | tac
        done
    } | awk '!seen[$0]++'
}

remember_token() {
    [ -n "${1:-}" ] || return 0
    { candidate_tokens | grep -qx "$1"; } 2>/dev/null && return 0
    printf '%s\n' "$1" >> "$TOKEN_CACHE"
}

from_nvs() {
    # Authoritative (it is what the device compares against), but **reading flash
    # resets the board**: esptool cannot attach to a running application (with
    # --no-reset it reports "Failed to connect ... No serial data received"), so
    # it must pull the chip into the ROM loader, which reboots the app.
    #
    # That side effect is why this is the *fallback* and not the first choice: a
    # reset mid-sequence closes any dialog under test and loses the page you were
    # on, which is indistinguishable from a UI bug. Prefer from_log().
    local tmp
    tmp="$(mktemp -d)"

    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \
      "Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; \
       . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1' *> \$null; \
       & 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' -m esptool \
         --chip esp32s31 -p $PORT -b 921600 \
         read-flash 0x9000 0x6000 '$(cygpath -w "$tmp/nvs.bin")' *> \$null" >/dev/null 2>&1 || true
    [ -s "$tmp/nvs.bin" ] || return 1

    "$PY" - "$(cygpath -w "$tmp/nvs.bin")" <<'PY'
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
# The NVS entry CRC is zlib's CRC32 *without* the final inversion (verified by
# recomputing the CRCs of a real image). Entries are append-only, so the LAST
# valid one is the live value.
def nvs_crc(b): return zlib.crc32(b, 0xFFFFFFFF)
best = None
for pg in range(len(d) // 4096):
    base = pg * 4096
    for i in range(126):                  # 4096/32 = 128; last 2 are page state
        o = base + i * 32
        r = d[o:o + 32]
        if r[1] != 0x21 or r[2] == 0 or i + r[2] > 126:
            continue                      # 0x21 = string; span = chunk count
        if r[8:24].split(b'\x00')[0] != b'auth_token':
            continue
        size, _res, dcrc = struct.unpack('<HHI', r[24:32])
        full = b''.join(d[o + k * 32:o + k * 32 + 32] for k in range(1, r[2]))[:size]
        if nvs_crc(full) == dcrc:
            best = full[:-1].decode()
print(best or '')
PY
    rm -rf "$tmp"
}

discover_token() {
    if [ -n "${DSH_TOKEN:-}" ]; then echo "$DSH_TOKEN"; return; fi
    # Cheap, non-invasive sources only. Never fall back to from_nvs() **here**:
    # that reads flash, which reboots the board, and this function is called on
    # every invocation - so putting it on the discovery path would reset the
    # device between every pair of commands. That is exactly the failure this
    # wrapper spent a while exhibiting: "switch to settings, it is back on home"
    # looked like a UI bug but was the tool rebooting the board (uptime kept
    # dropping to ~9 s, i.e. the boot hand-off switching to home).
    candidate_tokens | head -1
}

device_ip() {
    if [ -n "${DSH_IP:-}" ]; then echo "$DSH_IP"; return; fi
    local newest ip
    newest=$(ls -t "$ROOT"/build/*.log 2>/dev/null | head -5 || true)
    if [ -n "$newest" ]; then
        # shellcheck disable=SC2086
        ip=$(grep -ah 'TH:wifi: got ip [0-9.]*' $newest 2>/dev/null |
             tail -1 | sed -n 's/.*TH:wifi: got ip \([0-9.]*\).*/\1/p' || true)
    fi
    echo "$ip"
}

# --- serial side-channel ---------------------------------------------------

# Listen to the port for a few seconds *without* resetting the board, and print
# any token line that appears. Runs in the background while the command executes.
listen_for_token() {
    local out="$1"
    "$PY" "$ROOT/tools/monitor_capture.py" --port "$PORT" --seconds 6 --no-reset \
        --out "$out" >/dev/null 2>&1 || true
}

print_out() {
    "$PY" -c '
import sys, json
raw = sys.stdin.read()
try:
    r = json.loads(raw)
except Exception:
    sys.stderr.write("dsh: device did not return JSON: %s\n" % raw[:200])
    raise SystemExit(3)
if r.get("code") == 401:
    raise SystemExit(4)                      # sentinel: retry with a fresh token
d = r.get("data") or {}
sys.stdout.write(d.get("out", ""))
if d.get("truncated"):
    sys.stdout.write("(output truncated)\n")
'
}

main() {
    local raw=0
    if [ "${1:-}" = "--raw" ]; then raw=1; shift; fi
    if [ $# -eq 0 ]; then
        echo "usage: dsh.sh [--raw] <shell command...>" >&2
        exit 2
    fi

    local ip out rc
    ip=$(device_ip)
    if [ -z "$ip" ]; then
        echo "dsh: cannot determine the device IP (set DSH_IP=...)" >&2
        exit 1
    fi

    # Script-level (not `local`): the EXIT trap runs after main() returns, where a
    # local would already be out of scope and `set -u` would abort the trap.
    DSH_TMP="$(mktemp)"
    tmp="$DSH_TMP"
    # Watch the port for a token rotation caused by this very command.
    listen_for_token "$tmp" &

    # Try every candidate, newest first.
    #
    # The device rotates its token whenever the settings button is used, so a
    # single "discover then request" can race: discovery may return a token that
    # the device has already replaced by the time the request lands. Trying the
    # list costs one extra request in the failure case and removes the need for
    # the caller to know or care which token is live.
    local try_tokens
    try_tokens="$DSH_TOKEN_CACHE $(candidate_tokens | tr '\n' ' ')"
    if [ -z "${DSH_TOKEN_CACHE:-}" ]; then
        try_tokens=$(discover_token)
    fi

    rc=4
    for tok in $try_tokens; do
        [ -z "$tok" ] && continue
        out=$(curl -s -m 25 -X POST -H "X-Auth-Token: $tok" \
                  --data-binary "$*" "http://$ip/api/v1/debug/exec")

        if [ "$raw" = "1" ]; then
            printf '%s\n' "$out"
            return 0
        fi

        set +e
        printf '%s' "$out" | print_out
        rc=$?
        set -e
        if [ "$rc" != "4" ]; then
            remember_token "$tok"      # teach the cache which token is live
            return $rc
        fi
    done

    # Still 401: the command itself may have rotated the token while running.
    # Give the listener time to flush, then take the newest token it saw.
    sleep 2
    wait || true
    local fresh
    fresh=$(sed -n 's/.*new HTTP auth token: \([0-9a-f]\{32\}\).*/\1/p' "$tmp" 2>/dev/null | tail -1)
    if [ -n "$fresh" ]; then
        remember_token "$fresh"
        out=$(curl -s -m 25 -X POST -H "X-Auth-Token: $fresh" \
                  --data-binary "$*" "http://$ip/api/v1/debug/exec")
        printf '%s' "$out" | print_out
        return $?
    fi

    # Last resort: read flash for the authoritative token. This **reboots the
    # board** (see from_nvs), so it is opt-in via DSH_READ_NVS=1 rather than
    # automatic - a debugging tool must not silently reset what it is debugging.
    if [ "${DSH_READ_NVS:-0}" = "1" ]; then
        local nvs_tok
        nvs_tok=$(from_nvs || true)
        if [ -n "$nvs_tok" ]; then
            remember_token "$nvs_tok"
            echo "dsh: rebooted the board to read the token from NVS ($nvs_tok)" >&2
            return 4
        fi
    fi
    echo "dsh: 401 on every known token." >&2
    echo "     * the settings button rotates the token - open the token dialog once, or" >&2
    echo "     * DSH_TOKEN=<token>, or DSH_READ_NVS=1 (WARNING: reboots the board)" >&2
    return 4
}

# Clean up the serial-capture temp file. Declared here (not inside main) so the
# trap can still see it after main() has returned.
cleanup() { [ -n "$DSH_TMP" ] && rm -f "$DSH_TMP"; }
trap cleanup EXIT

main "$@"
