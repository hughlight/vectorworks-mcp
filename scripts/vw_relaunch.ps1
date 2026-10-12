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

# Copy the scratch document only after VW is gone: a running VW holds the
# previous scratch copy open and the copy would fail.
if (-not $VwxPath) {
    $VwxPath = Join-Path $env:TEMP 'gantry-test.vwx'
    Copy-Item -LiteralPath $blank -Destination $VwxPath -Force
    Log "Scratch document: $VwxPath (copy of TEST BLANK FILE.vwx)"
}
if (-not (Test-Path -LiteralPath $VwxPath)) { Log "No such file: $VwxPath"; exit 1 }

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

    # Only the "Unknown Developer Plug-ins" dialog gets dismissed. Left to
    # itself dismiss_dialog.py would also click Cancel on the "Opening File"
    # progress dialog. It logs what it clicked to D:\CODE\TH_VWX\scripts\_dialog.log.
    try {
        $check = (& python $dismiss --check 2>&1 | Out-String).Trim()
        if ($check -match 'Unknown Developer') {
            $out = (& python $dismiss 2>&1 | Out-String).Trim()
            Log $out
        } elseif ($check -and $check -notmatch '^No dialogs') {
            Log "Leaving dialog alone: $($check.Split([Environment]::NewLine)[0])"
        }
    } catch { Log "dismiss_dialog.py failed: $_" }

    # Ready means: bridge listening AND the document is open. The port opens
    # early (auto-start fires during plug-in init) while VW is still loading
    # the file, so the port alone is not enough. The main window title carries
    # "[<file>]" once a document is active.
    $main = Get-Process Vectorworks2026 -ErrorAction SilentlyContinue |
        Where-Object MainWindowTitle | Select-Object -First 1
    $title = if ($main) { $main.MainWindowTitle } else { '' }
    if ((PortOpen $Port) -and ($title -match '\[.+\]')) {
        Log "Port $Port open. Main window: '$title' (PID $($main.Id))"
        Log "OK -- VW + Gantry ready"
        exit 0
    }
}

$main = Get-Process Vectorworks2026 -ErrorAction SilentlyContinue |
    Where-Object MainWindowTitle | Select-Object -First 1
Log "Timed out after $TimeoutSec s. Port open: $(PortOpen $Port). Main window: '$($main.MainWindowTitle)'."
Log "If the port is open but no document is shown, VW is probably on its Welcome screen; open the file from there."
exit 1
