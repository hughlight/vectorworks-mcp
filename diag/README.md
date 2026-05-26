# VW2026.5 Diagnostic Probes

Three probes designed to localize the U5 regression to a specific layer.
Run them **in this order** and stop at the first one that fails — that's
where the regression lives.

## TL;DR — what each probe tests

| # | Probe | Layer | What a failure means |
|---|---|---|---|
| 0 | Read crash logs | OS / VW process | Tells you SEH vs. hang vs. clean exit before you launch anything |
| 1 | `probe_vectorscript.vs` | VW core (most stable layer) | If this crashes, the regression is in VW itself, not plugins |
| 2 | `probe_python.py` (in VW) | `IPythonScriptEngine` | If this crashes but #1 didn't, the Python engine is the broken piece |
| 3 | `probe_tcp_bridge.py` (in shell) | Our plugin's socket + command handler | If this works but Claude/MCP doesn't, the Rust server or MCP protocol is the issue |

## 0 — Read the crash log first (2 minutes, no VW launch needed)

The single most informative cheap data point.

**Windows (PowerShell):**
```powershell
$log = Get-ChildItem "$env:APPDATA\Nemetschek\Vectorworks\2026\Logs\" -Filter "*.txt" |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
Get-Content $log.FullName | Select-String "LastShutdown|SDK_VERSION|Version=|^Error"
```

**Mac:**
```bash
ls -t "$HOME/Library/Application Support/Vectorworks/2026/Logs/" | head -3
grep -E "LastShutdown|SDK_VERSION|Version=|^Error" \
    "$HOME/Library/Application Support/Vectorworks/2026/Logs/$(ls -t ~/Library/Application\ Support/Vectorworks/2026/Logs/ | head -1)"
```

**Interpreting `LastShutdown`:**
- `LastShutdown=Normal` — VW exited cleanly. Whatever's wrong, it didn't take down the process. Look elsewhere (functional regression, not stability).
- `LastShutdown=Crash` — SEH or uncaught exception. Suggests **VCOM ABI drift** (H2) or **IPythonScriptEngine init contract** (H3) from `docs/vw2026.5-triage.md`.
- `LastShutdown=Hung` — Deadlock. Suggests **resource contention** (H4) — Morpholio Trace background services, or socket binding race.
- No line at all — log was truncated; the crash happened before logging flushed. Strong SEH signal.

## 1 — VectorScript probe (`probe_vectorscript.vs`)

**With our plugin DISABLED**, this isolates the question "is VW itself broken on U5?" from "is our plugin broken on U5?".

### How to disable the MCP plugin
Rename `%APPDATA%\Nemetschek\Vectorworks\2026\Plug-ins\vectorworks-mcp.vlb`
to `.vlb.OFF` (or move it to the desktop). Restart VW. The MCP Bridge
palette will not appear.

### Run the probe
1. Open VW2026.5 with the plugin disabled.
2. Open or create any document (the probe needs a doc to write its report next to).
3. **Tools → Plug-Ins → Run Script…** → pick `diag/probe_vectorscript.vs`.
4. Expect an `AlrtDialog` with VW version, layer count, class count, IFC scheme, and the path to the JSON report.

### Outcomes
- **AlrtDialog appears, JSON file written, no crash** → VW core is healthy. Move to probe #2.
- **VW crashes during the script** → VW core regression. Stop. Capture the crash log. This is a `devsupport@vectorworks.net` ticket — our plugin can't help.
- **Script errors but VW doesn't crash** (e.g. `IFC_GetIFCScheme` reports a different scheme name) → catalog the diff but proceed to #2.

## 2 — Python probe (`probe_python.py`)

Still with our plugin disabled. Tests whether `IPythonScriptEngine` itself
runs `vs.*` scripts cleanly on U5.

1. **Tools → Plug-Ins → Run Script…** → pick `diag/probe_python.py`.
2. Expect an `AlrtDialog` with `Python probe: <N> errors. Report: <path>`.
3. Open the JSON report — the `errors` array tells you exactly which `vs.*` call (if any) misbehaved.

### Outcomes
- **0 errors, AlrtDialog appears, JSON written** → Python engine is healthy. The U5 regression is **not** in `vs.*` or `IPythonScriptEngine`. Move to probe #3 (re-enable the plugin).
- **VW crashes during the script** → confirms `IPythonScriptEngine` regression (H3). Without rebuilding against a refreshed SDK (H2), our plugin's `run_script` will keep crashing too. Document the call site (last successful `step()` in the report) and file with VW dev support.
- **N > 0 errors but no crash** → diff the failing steps against what we expect. A `GetVersion` signature change is harmless data; an `IFCGetIFCScheme` failure points at where Morpholio Trace's export refactor may have touched things.

## 3 — TCP bridge probe (`probe_tcp_bridge.py`)

Re-enable the plugin (`.vlb.OFF` → `.vlb`, relaunch VW, open the MCP
Bridge palette, click Start). This probe hits the listener directly —
no MCP server, no Claude.

```bash
# Simplest: just ping
python diag/probe_tcp_bridge.py

# Read VW state via VCOM
python diag/probe_tcp_bridge.py --method get_drawing_info

# Hardest: Python execution via IPythonScriptEngine (only meaningful if probe #2 passed)
python diag/probe_tcp_bridge.py --method run_script --script "import vs; print(vs.GetFName())"
```

### Outcomes
- **`ping` → `pong` round-trips, RTT under ~50ms** → socket + command handler healthy.
- **`ConnectionRefusedError`** → listener isn't actually bound, even though palette says "running." This is the singleton-state-wipe bug we already fixed (FORK_NOTES §"Three follow-up fixes" #1) — if it reappears, something tore the FD again. Restart VW.
- **Timeout** → TCP `accept()` worked but the plugin never wrote a response. Hang inside the command handler. Most likely culprit: `IPythonScriptEngine::ExecuteScript` deadlocked. Cross-reference probe #2 — if that crashed/hung too, you have a strong U5-specific Python-engine signal.
- **Response with `success: false`, `error_code != 0`, traceback in `stderr`** → handler returned cleanly, the script itself errored. This is the *good* failure mode. Read the traceback.

## 4 — Then and only then, through Claude

If probes 1–3 all pass and the regression still shows up via MCP:

```
Claude:  call mcp__vectorworks-2026__run_script with script="print('hi')"
```

If that fails while probe #3's `run_script` succeeds, the failure is in
the Rust server (`mcp-server/src/`) or the MCP protocol layer (`rmcp`),
**not** in VW. Vanishingly unlikely given the failure pattern, but
useful to know it's been excluded.

## What to collect for a bug report

If escalating to `devsupport@vectorworks.net`:

1. Crash log from step 0 (full file).
2. `vs-probe-*.json` from step 1.
3. `python-probe-*.json` from step 2.
4. Output of probe #3 for `ping`, `get_drawing_info`, and `run_script`.
5. Exact VW build number (`Help → About Vectorworks`).
6. Whether the plugin credentials file is present (per `FORK_NOTES.md` it isn't, yet).

The combination of crash log + which probe stopped working narrows the
root cause to one of the five hypotheses in `docs/vw2026.5-triage.md`.

## File map

```
diag/
├── README.md                  this file
├── probe_vectorscript.vs      runs in VW; VectorScript (oldest, most stable)
├── probe_python.py            runs in VW; vs.* via IPythonScriptEngine
└── probe_tcp_bridge.py        runs in shell; pure stdlib TCP probe of our plugin
```

All three probes write JSON reports. The Python and VectorScript probes
target `~/.vwmcp-diag/` and the document's directory respectively — bring
both files when reporting.
