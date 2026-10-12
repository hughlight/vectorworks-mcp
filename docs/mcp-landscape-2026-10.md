# Vectorworks MCP Landscape — October 2026

**Status:** survey, 2026-10-12
**Branch:** `claude/lattice-platform-mcp-TM9xi`
**Supersedes:** the 2026-09-11 survey (memory only, never written to the repo)
**Purpose:** record every public VW MCP project, how each one solved the write-safety problem, and where Gantry (this fork) sits.

## 1. One-paragraph verdict

There is still no official Vectorworks MCP. VW2027 shipped with an in-app AI Assistant and an AI Visualizer, nothing on the public roadmap mentions MCP, agents, SDK, or scripting, and the two community-board threads asking for one have no staff reply. The community has converged on one design: **execute every document mutation from inside VW's own Python menu-command runner**, triggered externally. Gantry is the only live fork of the original in-process C++/socket design that still executes scripts on a worker thread. The single most useful new fact from this survey is that BhaveshY's native bridge hit Gantry's exact crash on the main thread and fixed it on 2026-10-11 by switching from `IPythonScriptEngine::ExecuteScript` to `ScriptContext_Begin` + `ScriptContext_RunEx`, bracketed by an undo event. That API exists in our VW2026 SDK headers. It is the cheapest unverified lead for making Gantry's uncommitted `MainThreadDispatch` sufficient.

## 2. Project table

| Project | Lineage | VW / OS | Transport to VW | Where scripts run | Tools | Stars / last push | License | Notes |
|---|---|---|---|---|---|---|---|---|
| [vicquick/vwx-mcp](https://github.com/vicquick/vwx-mcp) | original | 2026 / Win (TCP fallback for Mac) | File IPC + C++ palette (`VwxBridge.vlb`) | Writes: Ctrl+Shift+B keystroke fires "VWX Bridge Start" Python menu command. Reads: OnIdle notification (background, no focus). | 248 + `vs_index.json` (3071 sigs) + `execute_script` | 15 / 2026-10-10 | MIT | Reference implementation. Roadmap marked complete 2026-07-07. Sept 2026 added `resource_list` via `BuildResourceList`, native resource import/duplicate, plug-in style relink. Spotlight, Truss, ConnectCAD explicitly out of scope. Export/import verbs still open modal dialogs. |
| [ReeceKalmar/vwx-mcp](https://github.com/ReeceKalmar/vwx-mcp) | fork of vicquick | **2027** / Win (SDK 3200, build 882699) | File IPC + C++ palette | Native UI-thread "menu broker" calls `gSDK->DoMenuName("VWX Bridge Start")`; no keystroke, no focus. Both reads and writes go through the menu command. One job per invocation. | 294 handwritten + 3098 generated SDK adapters; `landscape` profile ~257 | 0 / 2026-09-30 | MIT | Most rigorous engineering in the field: 948 offline tests, 2303 live fixture jobs, SHA-pinned deployments, maintenance restart controller. `NewField` passed 13 live cases on this path. Landscape-architecture focus. |
| [MattA-Official/vwmcp](https://github.com/MattA-Official/vwmcp) | ideas from vicquick | 2026/2027 / Win | File IPC, **no compiled plugin** (Python menu command only) | Server sends Ctrl+Shift+B from outside VW; a one-line `vwmcp` menu command runs the pump. Each job carries its own tool code. | 12 (`run_python`, `vs_lookup`, `vs_docs`, `query_objects`, `screenshot`, library tools) | 2 / 2026-10-11 | MIT | v0.3.0, confirmed on VW2027. Single `.exe`, CI on Linux+Windows, has a "migrate from vwx-mcp" guide. Lowest install friction of anything here. |
| [BhaveshY/vectorworks-mcp](https://github.com/BhaveshY/vectorworks-mcp) | independent | 2024/2025 (SDK req. lists 2026) / Win | TCP loopback + token to C++ `VectorworksMCPBridge` plugin | Message-only **main-thread pump window** (WM_TIMER + posted wake-ups). Scripts via `ScriptContext_Begin`/`RunEx` inside `SupportUndoAndRemove`/`NameUndoEvent`/`EndUndoEvent`, SEH-guarded, with rollback and "poisoned engine" fail-fast. | 10 coarse tools (`vw_read`, `vw_create`, `vw_edit`, `vw_script`…) | 1 / 2026-10-11 | none | "1.0" landed 2026-10-02. Architecturally the closest sibling to Gantry (native C++ socket plugin). Ships as a Claude Code plugin marketplace. No license file. |
| [SRVR-JOE/showcad-mcp](https://github.com/SRVR-JOE/showcad-mcp) | fork of vicquick | 2026 / Win | as vicquick | as vicquick | 248 (README unchanged) | 0 / 2026-09-02 | MIT | Claims ConnectCAD + Spotlight signal-flow/patch/cable tools in the blurb; README does not yet show any. Watch, don't adopt. |
| [Gribiche64/vectorworks-bridge](https://github.com/Gribiche64/vectorworks-bridge) | independent | any / Mac-leaning | **No plugin, no SDK.** Watches the Lightwright Data Exchange XML VW writes next to the `.vwx`. | Nothing runs in VW. Writes patch the XML and VW's file watcher applies them. | ~13 (fixture counts, channels, equipment list, `plot_qc`, `write_fixture_patch`) | 1 / 2026-09-05 | MIT | Spotlight-specific, reverse-engineered LW exchange protocol included. Directly relevant to FUSE lighting workflows; zero crash surface. |
| [upipibi/claude-vectorworks-mcp](https://github.com/upipibi/claude-vectorworks-mcp) | fork of mako-357 | 2026 / Mac only | Unix socket | Socket worker thread (same as Gantry). README admits writes "can crash or corrupt"; main-thread marshalling "not yet implemented". | 6 | 1 / 2026-07-04 | MIT + SDK EULA | `run_script` off by default. Confirms the mako lineage's threading problem is universal. |
| [chronista-club/vectorworks-mcp](https://github.com/chronista-club/vectorworks-mcp) (was mako-357) | original | 2025+ / Mac | Unix socket | Socket worker thread | 6 | 10 / 2026-04-14 | MIT | Frozen single commit. Gantry's upstream. Nothing to harvest. |
| [togawamanabu/vectorworks-mcp](https://github.com/togawamanabu/vectorworks-mcp) | independent | n/a | none (docs RAG) | n/a | 3 (`vw.search`, `vw.answer`, `vw.get`) | 5 / 2025-09-26 | none | FAISS index over VS/Python docs, Japanese. Knowledge-only. |
| [JeromyJSmith/lattice-platform](https://github.com/JeromyJSmith/lattice-platform) | independent | 2026 / Mac | AppleScript + temp files | VW's Run Script | ~10 | 4 / 2026-08-12 | none | Already assessed in `coverage-spec.md`. Consumer-side platform, not a peer. |
| SteeZyT33/av-pio-test-mcp, edmacovaz/rt-vectorworks-mcp, SRVR-JOE/VECTORWORKS_CONNECTCAD-MCP | forks | — | — | — | — | 0 | — | Unmodified forks or empty. Ignore. |

Official signals checked: VW2027 "New features" page, Vectorworks public roadmap, 2025/2026 AI Assistant press, forum threads #134256 ("official Model Context Protocol") and #134481 ("MCP for AI / Claude"). No MCP, no agent API, no SDK roadmap item. The forum threads only point readers at vicquick.

## 3. The execution-context consensus

Three independent authors (vicquick, BhaveshY, ReeceKalmar) have now live-tested where Python mutations survive. Combined picture on VW2026/2027 Windows:

| Context | Read-only `vs.*` | Mutation |
|---|---|---|
| Socket worker thread (mako, upipibi, **Gantry today**) | works by luck | crashes (resource manager race) |
| CEF web-palette sync callback | works | crashes |
| OnIdle notification | works | crashes |
| WM_TIMER on main thread + bare `ExecuteScript` | works | hangs (vicquick) / AV on first object create (BhaveshY, 2026-10-11) |
| Native menu `DoInterface` + bare `ExecuteScript` | works | crashes |
| **VW's Python menu-command runner** (vicquick, Reece, MattA) | works | works |
| **WM_TIMER on main thread + `ScriptContext_Begin`/`RunEx` + undo bracket** (BhaveshY) | works | works per their 2026-10-11 live round, VW2024/2025 |
| Native `gSDK` calls on main-thread timer inside `SupportUndoAndRemove`…`EndUndoEvent` (BhaveshY) | works | works |

Two consequences for us:

1. Tom's "known-bad" calls are context bugs, not API bugs. `vs.BuildResourceList(47, …)` is used directly in vicquick's `resource_list`; `vs.NewField` passed 13 live cases on ReeceKalmar's 2027 bridge. Both crash Gantry because Gantry runs them off the main thread without a script context.
2. Gantry's uncommitted `MainThreadDispatch` (HWND_MESSAGE + PostMessage + future) is the right half of the fix. `BridgeCore.cpp:211` still calls bare `py->ExecuteScript`. The other half is the `ScriptContext_*` path plus an undo bracket, which the VW2026 SDK ships (`IPythonScriptEngine.h:47-53`) and demonstrates in `ImageComparisonTesting.cpp`.

## 4. Where Gantry fits

**What Gantry has that nobody else in the table has:**
- Real `stdout`/`stderr` capture through `IPythonScriptEngine` + logger, structured error returns, no error dialogs. File-IPC designs recover prints by having the script write JSON.
- In-process, millisecond latency, no keystroke injection, no focus/minimize dependency, no modal-dialog auto-dismissal hacks.
- Dual VW2025/VW2026 build, per-version ports, web palette with auto-start.
- A Rust server (rmcp) rather than FastMCP. Neutral; nobody else is Rust except upipibi.

**What Gantry lacks:**
- Write safety. Every other actively maintained project solved this; Gantry's fix is written but unverified and incomplete (see §3).
- Tool surface: 6 tools vs 248/294. `coverage-spec.md` L2 plan (templated Python over `run_script`) was gated on the plumbing being stable. Still gated.
- The `vs_index.json` knowledge index (3071 signatures, arity checking). MIT, trivially importable.
- Plugin credentials (still "Unknown Developer"). ReeceKalmar documents the request process in `docs/PLUGIN_CREDENTIALS.md`.
- Any test harness. ReeceKalmar's offline/live fixture model is the bar.

**What none of them have, which is why Gantry plus TH_VWX still matters:**
- Data Visualization access. Confirmed zero mentions across all ten projects (vicquick's roadmap mentions only `ExcludeFromDataVis`). The binary-patch path in TH_VWX remains the only route.
- A `.vwx` parser. `D:\CODE\vwx-parser` is still the only one; GitHub search for "vwx parser" / "vectorworks file format" returns nothing.
- Spotlight depth. Only Gribiche64's XML bridge touches lighting data, and it is read-mostly through Lightwright exchange. vicquick lists Spotlight as out of scope.

## 5. Options

| Option | Cost | What you get | What you lose |
|---|---|---|---|
| **A. Fix Gantry in place** — wire `MainThreadDispatch` through `ScriptContext_Begin`/`RunEx` + undo bracket + SEH guard, copy BhaveshY's pattern | ~1–2 days C++ + a live test on VW2026 | Keeps in-process capture and latency; unblocks the L2 tool plan and the FUSE record work; makes `BuildResourceList`/`NewField` usable | Unverified on 2026; BhaveshY's evidence is VW2024/2025. If it fails, fall back to B. |
| **B. Adopt the menu-command runner** — add a "Gantry Run" Python menu command and have the C++ plugin call `gSDK->DoMenuName` from the main-thread window (ReeceKalmar's broker) | ~2–3 days | Proven-safe path on three codebases, no keystrokes needed because we are already native | Loses direct stdout capture (script writes JSON to a result file instead); adds a workspace-install step. |
| **C. Hard-fork vicquick/vwx-mcp** (the Sept 11 recommendation) | ~1 day to smoke-test, then ongoing divergence | 248 tools, knowledge index, proven safety on day one | Keystroke/focus dependency, FastMCP/Python server, German-flavoured codebase, Spotlight out of scope upstream, Gantry's C++ work shelved. ReeceKalmar's 2027 fork is the better base if going this way. |
| **D. Use MattA-Official/vwmcp as-is for day-to-day, keep Gantry for the DV/binary work** | hours | Zero-build Windows runner, `run_python`, screenshots, library tools, already on 2027 | Thin tool set, keystroke trigger, v0.3 maturity. |
| **E. Add Gribiche64's LW-XML bridge alongside whatever else** | hours | Crash-free Spotlight fixture Q&A and type swaps for FUSE | Read-mostly; Mac file-watcher assumptions need checking on Windows. |

Recommendation: **A first, time-boxed, with B as the fallback.** A is the only option that preserves what makes Gantry distinct, and the missing piece is now a known API call with a reference implementation rather than a research problem. Regardless of A/B, import vicquick's `vs_index.json` (MIT) and plan to track the VW2027 SDK (3200) since two of the four active projects already moved.

## 6. Things that changed since the 2026-09-11 survey

- Vectorworks 2027 shipped; two projects (ReeceKalmar, MattA) already target SDK 3200. Tom is on 2026.
- MattA-Official/vwmcp is new (created 2026-10-11) and is the first plugin-free Windows runner.
- BhaveshY went from "VW2024 timer experiment" to a 1.0 with transactions, SEH containment, and the `ScriptContext` finding. This is the most relevant change for Gantry.
- Gribiche64's Spotlight bridge is new (2026-09-05).
- vicquick added native resource tooling (2026-09-15) and otherwise went quiet after declaring the roadmap complete.
- ReeceKalmar's `NewField` live passes contradict the earlier note that `NewField` "hangs even on vicquick's safe path".

## Sources

- https://github.com/vicquick/vwx-mcp (README, docs/ROADMAP.md, docs/ARCHITECTURE.md, commits 2026-09-11..15)
- https://github.com/ReeceKalmar/vwx-mcp (docs/ARCHITECTURE.md, VECTORWORKS_2027.md, ROADMAP.md, API_COVERAGE_2027.json)
- https://github.com/MattA-Official/vwmcp (README, docs/explanation/architecture.md)
- https://github.com/BhaveshY/vectorworks-mcp (README, native_bridge/src/ScriptEngine.cpp, BridgeRuntime.cpp, NativeTransaction.cpp, SDK_REQUIREMENTS.json)
- https://github.com/SRVR-JOE/showcad-mcp
- https://github.com/Gribiche64/vectorworks-bridge
- https://github.com/upipibi/claude-vectorworks-mcp
- https://github.com/chronista-club/vectorworks-mcp
- https://github.com/togawamanabu/vectorworks-mcp
- https://github.com/JeromyJSmith/lattice-platform
- https://forum.vectorworks.net/topic/134256-official-model-context-protocol/ (403 to fetch; content via search snippets)
- https://forum.vectorworks.net/topic/134481-mcp-for-ai-claude/ (403 to fetch; content via search snippets)
- https://app-help.vectorworks.net/2027/eng/VW2027_Guide/LandingPage/New_features.htm
- https://www.vectorworks.net/en-US/public-roadmap/updates
- https://aecmag.com/bim/vectorworks-2027-targets-real-world-workflows/
- https://blog.cadalyst.com/architecture-infrastructure-construction-solutions/mcp-a-new-standard-for-connecting-ai-to-aec-software
- Local: `D:\CODE\TH_VWX\VWX_SDK\2026-NNA-eng-win-SDK\SDK\SDKVW(832364)\SDKLib\Include\Interfaces\VectorWorks\Scripting\IPythonScriptEngine.h`
