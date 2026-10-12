# vw_relaunch.ps1 — restart Vectorworks 2026 with a disposable document and
# wait until the Gantry bridge (vectorworks-mcp.vlb) is listening.
#
# Usage:
#   pwsh -File scripts\vw_relaunch.ps1                      # refuses if VW is running
#   pwsh -File scripts\vw_relaunch.ps1 -Force               # kills a running VW first
#   pwsh -File scripts\vw_relaunch.ps1 -Force -PyMode execute   # sets VWMCP_PY_MODE for VW
#   pwsh -File scripts\vw_relaunch.ps1 -VwxPath D:\path\to\scratch.vwx
#
# Default document: a fresh copy of D:\CODE\TH_VWX\TEST BLANK FILE.vwx in %TEMP%,
# so nothing real is ever opened. -Force kills VW without saving: only use it when
# you have confirmed the open document is disposable.
#
# Exit 0: VW running and port open. Exit 1: refused, exited early, or timed out.

param(
    [string]$VwxPath = "",
    [switch]$Force,
    [string]$PyMode = "",
    [int]$TimeoutSec = 240,
    [int]$Port = 27231
)

$ErrorActionPreference = 'Stop'
$exe     = 'C:\Program Files\Vectorworks 2026\Vectorworks2026.exe'
$dismiss = 'D:\CODE\TH_VWX\scripts\dismiss_dialog.py'
$blank   = 'D:\CODE\TH_VWX\TEST BLANK FILE.vwx'

function Log($m) { Write-Host "[vw_relaunch] $m" }

function PortOpen([int]$p) {
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $c.Connect('127.0.0.1', $p)
        $c.Close()
        return $true
    } catch { return $false }
}

if (-not $VwxPath) {
    $VwxPath = Join-Path $env:TEMP 'gantry-test.vwx'
    Copy-Item -LiteralPath $blank -Destination $VwxPath -Force
    Log "Scratch document: $VwxPath (copy of TEST BLANK FILE.vwx)"
}
if (-not (Test-Path -LiteralPath $VwxPath)) { Log "No such file: $VwxPath"; exit 1 }

$running = @(Get-Process Vectorworks2026 -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    if (-not $Force) {
        Log "Vectorworks is running (PIDs $($running.Id -join ', ')). Re-run with -Force to kill it."
        exit 1
    }
    Log "Killing Vectorworks PIDs $($running.Id -join ', ')"
    $running | Stop-Process -Force
    Start-Sleep -Seconds 3
    Get-Process vectorworks_error_handler -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($PyMode) {
    $env:VWMCP_PY_MODE = $PyMode
    Log "VWMCP_PY_MODE=$PyMode for the new VW process"
} else {
    Remove-Item Env:VWMCP_PY_MODE -ErrorAction SilentlyContinue
}

Log "Launching: `"$exe`" `"$VwxPath`""
$proc = Start-Process -FilePath $exe -ArgumentList "`"$VwxPath`"" -PassThru
Log "Launcher PID $($proc.Id); waiting up to $TimeoutSec s for port $Port"

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    $alive = @(Get-Process Vectorworks2026 -ErrorAction SilentlyContinue)
    if ($alive.Count -eq 0) { Log "Vectorworks exited during startup"; exit 1 }

    # dismiss_dialog.py clicks the preferred button on any modal VW dialog
    # (e.g. "Unknown Developer Plug-ins") and logs what it did to
    # D:\CODE\TH_VWX\scripts\_dialog.log.
    try {
        $out = (& python $dismiss 2>&1 | Out-String).Trim()
        if ($out -and $out -notmatch '^No VW dialogs') { Log $out }
    } catch { Log "dismiss_dialog.py failed: $_" }

    if (PortOpen $Port) {
        $main = Get-Process Vectorworks2026 | Where-Object MainWindowTitle | Select-Object -First 1
        Log "Port $Port open. Main window: '$($main.MainWindowTitle)' (PID $($main.Id))"
        Log "OK -- VW + Gantry ready"
        exit 0
    }
}

Log "Timed out after $TimeoutSec s; port $Port never opened. Check the palette / _dialog.log."
exit 1
