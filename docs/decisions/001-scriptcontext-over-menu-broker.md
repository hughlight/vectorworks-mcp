# ADR 001: Run scripts through the SDK script context, not a menu-command broker

**Date:** 2026-10-12
**Status:** Accepted
**Branch:** `claude/lattice-platform-mcp-TM9xi`

## Context

Gantry crashed Vectorworks on any `run_script` that mutated the document
(`vs.Rect`, `vs.NewField`, `vs.BuildResourceList(47, …)`). Two causes were
established (`docs/mcp-landscape-2026-10.md` §3):

1. The command handler ran on the socket worker thread. VCOM, `gSDK` and the
   script engines are not thread-safe. `MainThreadDispatch` (HWND_MESSAGE +
   PostMessage + future) was written for this but never live-tested.
2. Even on the main thread, bare `IPythonScriptEngine::ExecuteScript` runs
   read-only scripts but access-violates on the first object creation
   (vicquick: hangs; BhaveshY 2026-10-11: AV on `vs.Rect`, VW2024/2025).

Two ways to make writes safe were on the table:

- **A. Script context in-process.** Keep the dispatcher; replace bare
  `ExecuteScript` with `ScriptContext_Begin` + `ScriptContext_RunEx`
  (`IPythonScriptEngine.h:47-53`, used by the SDK's own
  `ImageComparisonTesting.cpp`), bracket each call in an undo event, guard
  with SEH, poison the engine after a contained fault.
- **B. Menu-command broker.** Add a Python menu command to the workspace and
  have the dispatcher call `gSDK->DoMenuName("Gantry Run", 0)`; the command
  reads a job file and writes a result file (ReeceKalmar/vwx-mcp, vicquick,
  MattA all do this). Proven on three codebases, but stdout capture is lost
  (the script has to write JSON itself) and a workspace install step is added.

## Decision

Option A. Verified live on VW2026 31.7.0.879357 on 2026-10-12
(`docs/test-runs/ladder-context-2026-10-12-173928.json`): object creation,
record-format creation, `BuildResourceList(47)` enumeration, exception
capture without a dialog, rollback on failure, and one undo step per call all
pass with the VW process alive throughout. The control run on the legacy
path (`py_mode=execute`, `docs/test-runs/ladder-execute-2026-10-12-173955.json`)
reproduces the access violation on `vs.Rect`; the SEH guard contained it and
the poison latch refused every later call. The crash is therefore the bare
`ExecuteScript` call, not the dispatcher, and the script context fixes it on
2026 as it did on 2024/2025.

Option B stays documented as the fallback (handoff Phase 2) and was not built.

## Consequences

- Gantry keeps what makes it distinct: in-process execution, millisecond
  latency, real stdout/stderr capture, no keystrokes, no workspace edits.
- `run_script` is now transactional by default. Any stderr output (not just a
  raised exception) counts as failure and rolls the step back; callers must
  print results to stdout. `transaction=false` opts out.
- After a contained hardware fault the engine is poisoned until VW restarts.
  This is deliberate: the interpreter and document state are unknown. The
  response says to Save As and restart.
- `py_mode=execute` / `VWMCP_PY_MODE=execute` keep the old path reachable for
  diagnostics only. It still faults on mutations; the guard contains it.
- Unchanged limits: 25 s dispatcher ceiling and 30 s socket timeout per call.
- The L2 templated-Python tool layer (`docs/coverage-spec.md`) is unblocked.
