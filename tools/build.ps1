# Build helper for ToDoHelper — the Windows-side half of tools/build.sh.
#
# WHY THIS FILE EXISTS
# --------------------
# idf.py cannot be driven from Git-Bash/MSYS directly: ESP-IDF's wrapper detects
# the MSYS environment and aborts with
#
#     MSys/Mingw is no longer supported. Please follow the getting started ...
#
# so Git-Bash hands the job to powershell.exe, which sources the ESP-IDF profile
# and runs the IDF python interpreter as a native Windows process.
#
# WHY IT LOOKS LIKE THIS (three non-obvious traps, all verified by experiment)
# ---------------------------------------------------------------------------
# 1. `Remove-Item Env:MSYSTEM` MUST happen here, in PowerShell, BEFORE running
#    python. Unsetting it in bash is useless: the MSYS runtime re-injects
#    MSYSTEM/OSTYPE into every native child process it spawns, so powershell.exe
#    always inherits them. Deleting the variable on the Windows side is the only
#    thing that stops idf.py from printing the "MSys/Mingw is no longer
#    supported" banner and exiting. Note the profile itself is sourced AFTER the
#    deletion, so the IDF environment it sets up is unaffected.
#
# 2. idf.py is called as `& $python $idfPy @idfArgs`, never as `idf.py ...`.
#    The profile defines idf.py as a PowerShell *function* (aliased to
#    Invoke-idfpy) which forwards its arguments with `@args`. That shorthand does
#    not bind when the caller is `powershell -Command`/`-File`, so the function
#    silently does nothing. Invoking the interpreter with an explicit argument
#    list always works.
#
# 3. No `| Tee-Object` and no `env -i`. Piping a native executable into a cmdlet
#    fails with "Cannot run a document in the middle of a pipeline: ...python.exe".
#    And an earlier version scrubbed the environment with `env -i` for
#    isolation; under that barren environment powershell.exe would start
#    normally, but the native python child would produce *no output at all* and
#    leave $LASTEXITCODE as $null -- so the build log came out empty while the
#    script still exited 0. Inheriting bash's normal environment avoids this
#    entirely, and trap 1 already provides the isolation that was actually
#    needed. Output is therefore captured with a normal PowerShell redirection
#    and the real exit code is propagated verbatim.
#
# Usage:  tools/build.sh [idf.py args]      (default: build)

[CmdletBinding()]
param(
    # Everything after the script name is forwarded to idf.py verbatim.
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = 'Continue'

# --- trap 1: strip the MSYS markers before any native process is spawned ------
foreach ($v in 'MSYSTEM', 'OSTYPE', 'MSYS', 'MINGW_PREFIX', 'MSYSTEM_PREFIX', 'MSYSTEM_CHOST') {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}

# --- activate the ESP-IDF environment ----------------------------------------
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1' | Out-Null

$ProjectDir = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectDir

$python = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
$idfPy  = 'D:\ESP-IDF\v6.1\esp-idf\tools\idf.py'

if (-not $IdfArgs -or $IdfArgs.Count -eq 0) { $IdfArgs = @('build') }

$logPath = Join-Path $ProjectDir 'build_last.log'

# --- trap 3: plain redirection, no cmdlet pipeline ---------------------------
# `*>` merges stdout/stderr/verbose into the log without ever putting a native
# exe in the middle of a cmdlet pipeline.
& $python $idfPy @IdfArgs *> $logPath
$code = $LASTEXITCODE
if ($null -eq $code) { $code = 1 }   # never report success for a vanished run

# Echo the log back to the caller (Git-Bash), then propagate the real status.
if (Test-Path $logPath) { Get-Content $logPath }
exit $code
