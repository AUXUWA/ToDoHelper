#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Fast flash for ToDoHelper (ESP32-S31-Korvo on COM34).

.DESCRIPTION
    `idf.py flash` is roughly 3.4x slower than necessary on this target, for two
    reasons (measured, 4.16 MB image):

        idf.py flash (460800, --no-stub)   147.9 s   224.6 kbit/s
        921600 + --no-stub                  98.7 s   337.4 kbit/s
        921600 + stub loader                35.8 s   930.9 kbit/s

    1. idf.py hardcodes `--no-stub` for esp32s31, which uses the ROM download
       protocol: no compression, smaller chunks. Dropping it enables the stub
       loader, which compresses the image (4.16 MB -> 2.57 MB here). This is the
       dominant win: 148 s -> 44 s end to end.
    2. The upload baud defaults to 460800. It is **not** a Kconfig option in
       IDF v6 (there is no CONFIG_ESPTOOLPY_BAUD* symbol to set) - only the
       `-b` flag or the ESPBAUD environment variable control it.

    Always build first (idf.py build); this script only flashes.

.EXAMPLE
    pwsh tools/flash.ps1
    pwsh tools/flash.ps1 -Port COM34 -Baud 921600 -NoBuild
#>
[CmdletBinding()]
param(
    [string]$Port = 'COM34',
    [int]$Baud = 921600,
    [switch]$NoBuild,
    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $NoBuild) {
    Write-Host '== build ==' -ForegroundColor Cyan
    & idf.py build
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

# esptool refuses to open the port while a monitor holds it, and a stale
# idf_monitor is the usual reason a flash "mysteriously" fails.
Get-CimInstance Win32_Process -Filter "Name like '%python%'" |
    Where-Object { $_.CommandLine -match 'idf_monitor|esp_idf_monitor' } |
    ForEach-Object {
        Write-Host "closing monitor pid $($_.ProcessId)" -ForegroundColor Yellow
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
Start-Sleep -Seconds 1

$esptool = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
if (-not (Test-Path $esptool)) { $esptool = 'python' }

$args = @(
    '-m', 'esptool', '--chip', 'esp32s31', '-p', $Port, '-b', "$Baud",
    '--before=default-reset', '--after=hard-reset',
    # No --no-stub: this is the whole point, see the header comment.
    'write-flash', '--flash-mode', 'dio', '--flash-freq', '80m', '--flash-size', '16MB',
    '0x2000',  'build\bootloader\bootloader.bin',
    '0x8000',  'build\partition_table\partition-table.bin',
    '0xa10000','build\mmap_build\ui\assets\assets.bin',
    '0x10000', 'build\todohelper.bin'
)

Write-Host "== flash @ $Baud baud (stub + compression) ==" -ForegroundColor Cyan
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $esptool @args
$code = $LASTEXITCODE
$sw.Stop()
Write-Host ("flash finished in {0:N1} s" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green

if ($code -ne 0) { throw "flash failed ($code)" }

if ($Monitor) {
    & idf.py -p $Port monitor
}
