# Handoff: Gantry write-safety fix

**Written:** 2026-10-12
**Branch:** `claude/lattice-platform-mcp-TM9xi`
**Prerequisite reading for the new session:** `docs/mcp-landscape-2026-10.md` (why), `FORK_NOTES.md` (how to build/deploy), `docs/coverage-spec.md` (what comes after).

Everything below the line is the prompt. Paste it verbatim into a fresh Claude Code session opened at `D:\CODE\vectorworks-mcp`.

---

## Prompt

You are taking over development of **Gantry**, Tom Huston's fork of a Vectorworks MCP server, at `D:\CODE\vectorworks-mcp` on branch `claude/lattice-platform-mcp-TM9xi`. Tom plans in one session and delegates execution to sessions like this one, so this prompt is your full brief. Read it completely before touching anything.

### What Gantry is

```
Claude --stdio--> mcp-server/ (Rust, rmcp) --TCP 127.0.0.1:27231--> vw-plugin/ (C++ VCOM plugin, vectorworks-mcp.vlb, inside Vectorworks 2026)
```

The plugin exposes six tools; `run_script` executes Python or VectorScript inside VW through `IPythonScriptEngine` and returns captured stdout/stderr. Tom uses it daily as the `fuse-vw-gantry` MCP for FUSE lighting-production work in VW Spotlight. The full history is in `FORK_NOTES.md`.

### The bug you are fixing

Gantry crashes Vectorworks on any script that mutates the document's resource or record tables (`vs.NewField`, `vs.BuildResourceList(47, …)`, creating record formats, sometimes plain `vs.Rect`). The same scripts run fine from VW's Tools → Run Script menu.

Root cause, confirmed by source inspection and by three independent projects:
1. `SocketBridge::HandleClient` ran the command handler on the socket worker thread. VCOM, `gSDK`, and the script engines are not thread-safe. A main-thread dispatcher (`vw-plugin/src/MainThreadDispatch.{h,cpp}`, HWND_MESSAGE window + PostMessage + `std::future`) was written to fix this and is now committed, but it has **never been built or live-tested**.
2. Main-thread dispatch alone is not enough. `BridgeCore.cpp` (`HandleCommand`, the `run_script` branch) still calls bare `IPythonScriptEngine::ExecuteScript`. Two other projects found that bare `ExecuteScript` from a main-thread timer or posted message runs read-only scripts but hangs or access-violates on the first object creation. BhaveshY/vectorworks-mcp fixed it on 2026-10-11 with the SDK's own pattern:

```cpp
// native_bridge/src/ScriptEngine.cpp in https://github.com/BhaveshY/vectorworks-mcp
err = engine->ScriptContext_Begin(script, &logger);
if (err == kVCOMError_NoError) err = engine->ScriptContext_RunEx(&logger);
```

bracketed by an undo event:

```cpp
gSDK->SupportUndoAndRemove();
gSDK->SetUndoMethod(kUndoSwapObjects);
gSDK->NameUndoEvent(TXString("MCP run_script"));
// ... run ...
ok ? gSDK->EndUndoEvent() : gSDK->UndoAndRemove();
gSDK->DrawScreen();
```

and wrapped in an SEH `__try/__except` guard that lives in a small function with only POD locals (MSVC C2712 forbids `__try` in frames with C++ objects). After a contained fault they mark the engine "poisoned" and refuse further scripts until VW restarts.

The `ScriptContext_*` methods exist in Tom's SDK: `D:\CODE\TH_VWX\VWX_SDK\2026-NNA-eng-win-SDK\SDK\SDKVW(832364)\SDKLib\Include\Interfaces\VectorWorks\Scripting\IPythonScriptEngine.h` lines 47-53. The SDK sample `ImageComparisonTesting.cpp` (search the SDK tree) shows the canonical call sequence. BhaveshY's evidence is VW2024/2025; **nobody has verified this on VW2026**. That verification is the point of your work.

A clone of BhaveshY's repo, plus ReeceKalmar/vwx-mcp and MattA-Official/vwmcp, may still be at `C:\Users\tomhu\AppData\Local\Temp\claude\D--CODE-vectorworks-mcp\...\scratchpad\ext\`. If not, `git clone --depth 1` them into a scratch folder. Treat them as reference only; all three are MIT except BhaveshY, which has no license file, so **do not copy BhaveshY code verbatim**. Re-derive from the SDK header and sample.

### Your task, in order

**Phase 1 — make `run_script` write-safe (the goal).**
1. Use the brainstorming skill briefly to confirm the design below, then write a short plan with the writing-plans skill. Don't over-plan; the design is mostly decided.
2. In `BridgeCore.cpp`, replace the bare `ExecuteScript` call in the Python branch with `ScriptContext_Begin` + `ScriptContext_RunEx`, keep `CDefaultPythonLogger` for stdout/stderr capture, add the undo bracket, and add the SEH guard function. Keep an env-var or command-flag escape hatch to the old bare path for A/B testing (`VWMCP_PY_MODE=execute`).
3. Add a `transaction` (default true) and `undo_label` parameter to `run_script` in both the C++ handler and `mcp-server/src/server.rs`.
4. Add a "poisoned engine" flag: after a contained SEH fault, every later `run_script` returns `{success:false, error:"engine_poisoned"}` until VW restarts.
5. The dispatcher's 25 s ceiling in `SocketBridge.cpp` and the 30 s `SO_RCVTIMEO` on the server side will bite long scripts. Leave them for now but note the limit in the tool description.
6. Build with the exact MSBuild command in `FORK_NOTES.md` ("What was actually built and how to rebuild"), VW2026 variant. Toolset v145, SDK root as given there. Expect zero warnings.
7. Deploy: VW must be closed to overwrite `%APPDATA%\Nemetschek\Vectorworks\2026\Plug-ins\vectorworks-mcp.vlb`. Use `D:\CODE\TH_VWX\scripts\vw_relaunch.ps1` (see the `vw-gantry-recovery` skill) to relaunch VW and reopen a **disposable** test document. Never test against a real FUSE file.
8. Live test ladder, each step through the `fuse-vw-gantry` MCP (or raw TCP if the MCP isn't configured in this session; the protocol is newline-delimited JSON, see `SocketBridge.cpp`):
   - `print('hi')`
   - `import vs; print(vs.GetFName())`
   - `import vs; vs.Rect(0,0,100,100)` (the call that AVs on bare `ExecuteScript`)
   - `import vs; vs.NewField('MCP_Test', 'f1', 'x', 4, 0)` (creates a record format; the historic Gantry killer)
   - `import vs; vs.BuildResourceList(47, 0, '')` then iterate names
   - `raise Exception('x')` must return `success:false` with a traceback and no dialog
   - A script that mutates then raises: confirm `UndoAndRemove` rolled it back (count objects before/after)
   - Ten rapid sequential mutation calls, then Ctrl+Z in VW: one undo step per call
   - Repeat the ladder with `VWMCP_PY_MODE=execute` to prove the old path still crashes and the new one doesn't. Expect VW to die on this run; that's the control.
9. Record results in a table in `FORK_NOTES.md` under a new dated section, with the VW build number from Help → About. Fail honestly: if `ScriptContext_RunEx` also faults on 2026, say so and go to Phase 2.

**Phase 2 — fallback only if Phase 1 fails live.**
Add a Python menu command "Gantry Run" to the `.vwr` workspace resources and have the main-thread dispatcher call `gSDK->DoMenuName("Gantry Run", 0)` instead of executing Python directly; the menu command reads a job file and writes a result file. This is ReeceKalmar/vwx-mcp's "SDK menu broker" (`docs/ARCHITECTURE.md` there). Stdout capture moves to the script writing JSON. Only do this if Phase 1 is dead; it costs the thing that makes Gantry distinct.

**Phase 3 — once writes are safe.**
1. Copy `vwx-plugin/vs_index.json` from vicquick/vwx-mcp (MIT, attribute in a NOTICE file) into `mcp-server/` and expose a `vs_signature(name)` tool.
2. Start Phase 1 of `docs/coverage-spec.md` (the L2 templated-Python tool layer) now that the plumbing is trustworthy. `list_classes`, `list_record_formats`, `read_records_by_format`, `export_ifc` first.
3. Write `docs/decisions/001-scriptcontext-over-menu-broker.md` (or `001-menu-broker.md` if Phase 2 won) as a short ADR: context, options A/B from the landscape doc, decision, consequences.

### Ground rules

- Tom is not watching live. Do reversible things without asking; stop and ask only before anything that touches a non-disposable `.vwx` or changes his VW workspace.
- Use the systematic-debugging skill when VW crashes instead of patching blindly. `%APPDATA%\Nemetschek\Vectorworks\2026\Logs\` has the crash log; `LastShutdown=` is the key line.
- Commit after each green step with messages that say what was verified, not what was attempted. Push to `origin claude/lattice-platform-mcp-TM9xi`.
- Line endings: repo is LF with `.gitattributes` enforcing it. Don't let the editor flip files to CRLF.
- Update `FORK_NOTES.md` as you go; that file is the project's memory. Keep `docs/mcp-landscape-2026-10.md` untouched unless you learn something that contradicts it, then edit that specific claim and date the edit.
- Finish with a short recap in the final message: what works, what doesn't, exact build number tested, what's next.

### Out of scope

Mac builds, VW2027/SDK 3200, plugin credentials, Data Visualization (that lives in `D:\CODE\TH_VWX`), the Rust server's transport. Mention them in the recap if they block you; don't start them.
