# Gantry write-safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `run_script` survive document mutations on VW2026 by running Python through `ScriptContext_Begin`/`ScriptContext_RunEx` on the main thread, inside an undo bracket, under an SEH guard, with a poisoned-engine latch and an `execute` escape hatch for A/B control.

**Architecture:** `SocketBridge::HandleClient` already marshals every command onto VW's main thread through `MainThreadDispatch` (HWND_MESSAGE + PostMessage + future). This plan changes only what the handler does once it is on the main thread: the `run_script` branch in `BridgeCore.cpp` gains a script-context path, an undo bracket, SEH containment in POD-only frames, and a process-wide poison latch. The Rust server passes three new optional parameters through unchanged. A PowerShell relaunch script and a Python ladder script make the live test repeatable.

**Tech Stack:** C++17 (MSVC v145, VW2026 SDK 3100 at `D:\CODE\TH_VWX\VWX_SDK\2026-NNA-eng-win-SDK\SDK\SDKVW(832364)`), Rust (rmcp 1.4, tokio), PowerShell 7, Python 3 stdlib.

**Spec:** `docs/handoff-gantry-write-safety.md` (the brief), `docs/mcp-landscape-2026-10.md` section 3 (why), BhaveshY/vectorworks-mcp `native_bridge/src/ScriptEngine.cpp` (reference only, no license; re-derive from `IPythonScriptEngine.h:47-53` and the SDK sample `ImageComparisonTesting.cpp:175-183`).

## Global Constraints

- Branch `claude/lattice-platform-mcp-TM9xi`; LF line endings (`.gitattributes`), `.vcxproj` and `.ps1` are CRLF.
- Build command (VW2026): `"F:\Visual_Studio\MSBuild\Current\Bin\MSBuild.exe" vw-plugin\vectorworks-mcp.vcxproj -p:Configuration=Release -p:Platform=x64 -p:VWSDKRoot="D:\CODE\TH_VWX\VWX_SDK\2026-NNA-eng-win-SDK\SDK\SDKVW(832364)" -p:OutDir="<repo>\vw-plugin\build\Release-VW2026\\" -p:IntDir="<repo>\vw-plugin\build\Release-VW2026\obj\\"`; zero warnings expected.
- Deploy target: `%APPDATA%\Nemetschek\Vectorworks\2026\Plug-ins\vectorworks-mcp.vlb` plus `vectorworks-mcp.vwr\`. VW must be restarted to load it. The currently deployed, disabled `FuseVWGantry.*_OLD` files are left untouched.
- Never test against a non-disposable `.vwx`. Test doc: a scratch copy of `D:\CODE\TH_VWX\TEST BLANK FILE.vwx`.
- Do not copy BhaveshY code verbatim. Tom's own `harden/crash-resistance` branch (`SehCallPythonExecute`) is fair to reuse.
- Existing success semantics stay: `success = (err == kVCOMError_NoError) && stderr empty`.
- `MainThreadDispatch` 25 s ceiling and the server's 30 s `SO_RCVTIMEO` stay; the tool description must say so.

## Review Focus

1. A Python script that writes to stderr without raising (warnings) is treated as failure and rolled back. Pinned by ladder step 7 only for `raise`; the semantics are documented in the tool description (Task 2).
2. A contained SEH fault inside an open undo event: the event must be closed with `EndUndoEvent`, never replayed with `UndoAndRemove` (the replay may re-fault). Pinned by code review in Task 1 and by the control run in Task 5.
3. `py_mode` from the request must override the `VWMCP_PY_MODE` env var, and an unknown value must fall back to `context`, so a typo cannot silently select the crashing path. Pinned by the ladder's `--py-mode` flag in Task 3.
4. `undo_label` longer than 120 chars or empty must be clamped or defaulted, not passed raw to `NameUndoEvent`. Pinned by Task 1 code.
5. Any `run_script` after a contained fault must return `error:"engine_poisoned"` without touching the engine. Pinned by Task 5 (if the control run faults but is contained, the next call must be refused).

---

### Task 1: ScriptContext path, undo bracket, SEH guard, poison latch (C++)

**Files:**
- Modify: `vw-plugin/src/BridgeCore.cpp` (the `run_script` branch, plus new helpers above `HandleCommand`)
- Modify: `vw-plugin/src/BridgeCore.h` (declare `bool IsEnginePoisoned();`)

**Interfaces:**
- Consumes: `IPythonScriptEngine::{ScriptContext_Begin, ScriptContext_RunEx, ExecuteScript}`, `IVectorScriptEngine::ExecuteScript`, `gSDK->{SupportUndoAndRemove, SetUndoMethod, NameUndoEvent, EndUndoEvent, UndoAndRemove, DrawScreen, SetProgramVariable}`, `kUndoSwapObjects` (= 21, `MiniCadCallBacks.h:53`), program variable 9876 (suppress VS error dialogs, per `ImageComparisonTesting.cpp:181-182`).
- Produces: `run_script` request params `script`, `language`, `transaction` (bool, default true), `undo_label` (string, default `"MCP run_script"`, max 120), `py_mode` (`"context"` | `"execute"`, default env `VWMCP_PY_MODE` else `"context"`). Response `data`: `stdout`, `stderr`, `error_code`, `transaction`, `rolled_back`, `py_mode`, `undo_label`; on a contained fault `error:"script_crashed"`, `fault_code:"0x........"`, `error_code:-2`; when poisoned `error:"engine_poisoned"`, `error_code:-3`.

- [ ] **Step 1: Add the SEH shims and latch above `HandleCommand`** (POD-only frames because of MSVC C2712):

```cpp
#ifdef _WIN32
  #include <windows.h>
  #include <malloc.h>   // _resetstkoflw
#endif
#include <atomic>

static std::atomic<bool>          g_enginePoisoned{false};
static std::atomic<unsigned long> g_firstFaultCode{0};
bool IsEnginePoisoned() { return g_enginePoisoned.load(); }

#ifdef _WIN32
static int ScriptFaultFilter(unsigned long code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION: case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_STACK_OVERFLOW:
            return EXCEPTION_EXECUTE_HANDLER;
        default: return EXCEPTION_CONTINUE_SEARCH;   // C++ exceptions keep unwinding
    }
}
static unsigned long GuardedPyContextRun(IPythonScriptEngine* eng, const TXString* script,
                                         IPythonLogger* logger, VCOMError* err)
{
    __try {
        *err = eng->ScriptContext_Begin(*script, logger);
        if (*err == kVCOMError_NoError) *err = eng->ScriptContext_RunEx(logger);
        return 0;
    } __except (ScriptFaultFilter(GetExceptionCode())) {
        unsigned long code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return code ? code : 1u;
    }
}
// GuardedPyExecute (bare ExecuteScript), GuardedVsExecute, GuardedEndUndoEvent and
// GuardedUndoAndRemove follow the same shape.
#endif
```

- [ ] **Step 2: Rewrite the `run_script` branch.** Order: poisoned check; parse `transaction`, `undo_label`, `py_mode`; no-document guard; set program variable 9876 for the duration; open the undo bracket if `transaction`; run via the guarded shim chosen by `py_mode`; on fault latch the poison flag, `GuardedEndUndoEvent()` if transaction, return `script_crashed`; otherwise `ok ? EndUndoEvent : UndoAndRemove`, `DrawScreen()`, build `data`.

- [ ] **Step 3: Build** with the Global Constraints command. Expected: `0 Warning(s), 0 Error(s)` (the `pwsh.exe` post-build message is cosmetic).

- [ ] **Step 4: Commit** `Plugin: run_script via ScriptContext + undo bracket + SEH guard + poison latch (builds, untested live)`.

### Task 2: Rust server parameters and description

**Files:**
- Modify: `mcp-server/src/server.rs` (`RunScriptInput`, `run_script`)

**Interfaces:**
- Produces: MCP `run_script` tool with optional `transaction: bool`, `undo_label: string`, `py_mode: string`; only forwarded when present so the plugin applies its defaults.

- [ ] **Step 1: Extend `RunScriptInput`** with the three `Option` fields and `schemars` descriptions. Update the tool description to say: runs on VW's main thread inside one undo step; any stderr output counts as failure and rolls the step back; 25 s ceiling per call; `py_mode=execute` is a diagnostic that can crash VW.
- [ ] **Step 2: `cargo build --release`** in `mcp-server/`. Expected: success, no warnings from `server.rs`.
- [ ] **Step 3: Commit** `Server: run_script transaction/undo_label/py_mode params + timeout note`.

### Task 3: Relaunch and ladder scripts

**Files:**
- Create: `scripts/vw_relaunch.ps1`: `-VwxPath` (default: copies `D:\CODE\TH_VWX\TEST BLANK FILE.vwx` to `%TEMP%\gantry-test.vwx`), `-Force` (kill running VW), `-PyMode` (sets `VWMCP_PY_MODE` for the child). Launches `Vectorworks2026.exe <vwx>`, loops `D:\CODE\TH_VWX\scripts\dismiss_dialog.py` plus a port 27231 probe until ready or timeout. Exits 0/1.
- Create: `scripts/gantry_ladder.py`: stdlib TCP client; runs the handoff's ladder in order, checks VW PID liveness after each step, writes `ladder-<mode>-<timestamp>.json` and prints a markdown table. Flags `--py-mode context|execute`, `--port`, `--out-dir`.

- [ ] **Step 1: Write both scripts.** Ladder steps and pass criteria:
  1. `ping` returns `pong`
  2. `print('hi')` returns stdout `hi`
  3. `import vs; print(vs.GetFName())` succeeds
  4. `import vs; vs.Rect(0,0,100,100); print('rect')` succeeds, VW alive
  5. `import vs; print(vs.NewField('MCP_Test','f1','x',4,0))` succeeds, VW alive
  6. `import vs; lid,n=vs.BuildResourceList(47,0,''); print(n, [vs.GetNameFromResourceList(lid,i) for i in range(1,n+1)])` succeeds and `MCP_Test` is in the names
  7. `raise Exception('x')` returns success false, `Traceback` in stderr, VW alive
  8. count via `vs.Count('ALL')`; then `import vs; vs.Rect(0,0,50,50); raise Exception('rollback')` with transaction true; count again: equal, and `rolled_back:true`
  9. ten calls `import vs; vs.Rect(i*10,0,i*10+5,5)` each with `undo_label='ladder N'`: count +10; then one `vs.DoMenuTextByName('Undo',0)` with `transaction:false`: count +9
- [ ] **Step 2: Dry-run the ladder against the current (dead) port.** Expect a clean "connection refused" report and exit 1.
- [ ] **Step 3: Commit** `Scripts: VW relaunch + write-safety test ladder`.

### Task 4: Deploy and run the ladder on the new path

- [ ] **Step 1:** Copy `build/Release-VW2026/vectorworks-mcp.vlb` and the `.vwr` folder into the Plug-ins dir, then `scripts/vw_relaunch.ps1 -Force` (VW currently holds a blank "Untitled 4"; verified disposable by screenshot 2026-10-12).
- [ ] **Step 2:** `python scripts/gantry_ladder.py --py-mode context`. Expected: all nine steps pass, VW PID unchanged throughout.
- [ ] **Step 3:** If any step crashes VW: use superpowers:systematic-debugging; collect the tail of `%APPDATA%\Nemetschek\Vectorworks\2026\Vectorworks Log.txt` and the `Sentry\` listing; do not patch blindly.
- [ ] **Step 4: Commit** the ladder JSON under `docs/test-runs/` and the FORK_NOTES table (Task 6).

### Task 5: Control run on the old path

- [ ] **Step 1:** `python scripts/gantry_ladder.py --py-mode execute`. Expected per the handoff: VW dies (or a contained `script_crashed` followed by `engine_poisoned` on the next call) at step 4 or 5.
- [ ] **Step 2:** Relaunch VW with `scripts/vw_relaunch.ps1 -Force` and re-run `--py-mode context` once more to prove the new path still passes in a fresh process.

### Task 6: Record results, ADR, push

**Files:**
- Modify: `FORK_NOTES.md` (new dated section with the results table, VW build `31.7.0.879357`, plugin naming note about `harden/crash-resistance`).
- Create: `docs/decisions/001-scriptcontext-over-menu-broker.md` (or `001-menu-broker.md` if Phase 2 was needed).
- Create: `docs/test-runs/2026-10-12-ladder-*.json`.

- [ ] **Step 1:** Write the table honestly (pass, fail or crash per step, both modes).
- [ ] **Step 2:** `git push origin claude/lattice-platform-mcp-TM9xi`.

### Task 7 (Phase 3, only after Task 4 is green): `vs_signature` tool and first L2 tools

- [ ] **Step 1:** Clone `vicquick/vwx-mcp` (depth 1) into the scratchpad, copy `vwx-plugin/vs_index.json` to `mcp-server/vs_index.json`, add `NOTICE` with the MIT attribution.
- [ ] **Step 2:** `server.rs`: embed with `include_str!`, add a `vs_signature(name)` tool returning the entry or a "not found" message; unit test for one known name and one unknown.
- [ ] **Step 3:** Add templated-Python tools `list_classes`, `list_record_formats`, `read_records_by_format`, `export_ifc` as thin wrappers over `run_script` (each a Python body that prints JSON to stdout, parsed server-side). Live-check each once.
- [ ] **Step 4:** Commit per tool; push.
