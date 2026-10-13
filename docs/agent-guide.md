# Agent guide: Vectorworks MCP (Gantry)

For any agent or person handed this MCP server. It says what the server can do, the exact
contract of each tool, the caveats that bite, and how to recover. Every claim here was
checked by `scripts/capability_bench.py`; the generated evidence table with timings is
`docs/capabilities-matrix.md`, raw records are in `docs/test-runs/`.

Tested on Vectorworks Spotlight 2026, `Vectorworks2026.exe` file version 31.7.0.879357,
Windows 11, plugin built against SDK 3100. Python inside Vectorworks is 3.9.2.

## 1. What it is

```
agent --stdio--> vectorworks-mcp-server.exe --TCP 127.0.0.1:27231--> vectorworks-mcp.vlb (inside Vectorworks)
                 (Rust, mcp-server/)                                  (C++, vw-plugin/) -> VW SDK + Python engine
```

- Everything runs on Vectorworks' main thread, in process, with real stdout/stderr capture.
  No keystrokes, no AppleScript, no workspace edits.
- Windows only today. One port per VW version: 27200 + short tag (VW2026 = 31 -> 27231).
- The plugin auto-starts its bridge when VW launches (palette "MCP Bridge", config
  `%APPDATA%\Nemetschek\Vectorworks\2026\vectorworks-mcp-config.json`).

## 2. Connecting

```json
{
  "mcpServers": {
    "vectorworks-2026": {
      "command": "D:/CODE/vectorworks-mcp/mcp-server/target/release/vectorworks-mcp-server.exe",
      "args": ["--vw-version", "2026"]
    }
  }
}
```

Preconditions: Vectorworks is running, a document is open, the bridge is started. The server
logs `Vectorworks bridge: connected` on stderr at startup. If a tool returns
`Connection error: Could not connect to 127.0.0.1:27231`, the bridge is not up: open the
"MCP Bridge" palette in VW and click Start, or relaunch with `scripts/vw_relaunch.ps1`.

Without a client, `scripts/mcp_smoke.py --tool <name> --args '<json>'` calls any tool through
the real server over stdio.

## 3. Tools

Levels: **L1** native C++ handler, **L0** `run_script`, **L2** templated Python body run
through `run_script` with `transaction=false`.

| Tool | Level | Writes | Undo step | Notes |
|---|---|---|---|---|
| `get_drawing_info` | L1 | no | | `has_document`, `layer_count`. `layer_count` counts every top-level member of the drawing header, not only layers (bench: 2 with one layer). |
| `list_layers` | L1 | no | | Names of layer nodes (design and sheet). |
| `create_line`, `create_rect`, `create_circle` | L1 | yes | **no** | Call the SDK directly with no undo bracket; Ctrl+Z removes the previous step instead. Prefer `run_script` for anything a user may want to undo. |
| `run_script` | L0 | depends | yes, one per call | Python (default) or VectorScript. Full contract in section 4. |
| `vs_signature` | L2 | no | | Offline index of 3071 `vs.*` signatures (vicquick/vwx-mcp, MIT). Exact name, or prefix/substring to list candidates. Works without VW. |
| `list_classes` | L2 | no | | Every class with visibility (0 visible, -1 invisible, 2 grayed). |
| `list_record_formats` | L2 | no | | Formats with field names and type codes. `include_hidden=true` adds `__`-prefixed plug-in parameter records. |
| `read_records_by_format` | L2 | no | | All objects carrying a format: type, class, layer, name, values. `layer` filter (exact name), `limit` (default 500, `truncated` flag). Values are strings. Unknown format returns `{"error": ...}` as a normal result. |
| `export_ifc` | L2 | file | | `vs.IFC_ExportNoUI(path)`. **Unverified**: returned false and wrote nothing on the blank scratch document. Needs a real model with IFC data. |

## 4. `run_script` contract

Input: `script`, `language` (`python` default, `vectorscript`), `transaction` (default true),
`undo_label` (default `MCP run_script`, truncated to 120 chars), `py_mode` (leave unset; see
section 6).

Output fields: `stdout`, `stderr`, `error_code`, `rolled_back`, `transaction`, `undo_label`,
`py_mode`, and on bridge-level failures `error` and `fault_code`. The MCP text result is the
pretty JSON of these on success, or `Error: {...}` with the same fields when `success` is false.

| Situation | success | `error` | `error_code` | What happened |
|---|---|---|---|---|
| Script finished, nothing on stderr | true | | 0 | Undo event closed; one Ctrl+Z step. |
| Python exception | false | | 0 | Traceback in `stderr`, `rolled_back: true`. |
| Anything written to stderr (warnings included) | false | | 0 | Treated as failure, rolled back. `stdout` is still returned. |
| VectorScript compile or runtime error | false | | 1 | `stderr` is empty; VectorScript has no output capture at all. |
| No document open | false | `no_document` | -1 | Refused before running. |
| Hardware fault inside the engine, contained | false | `script_crashed` | -2 | `fault_code` has the exception code. Engine is now poisoned. |
| Any call after a contained fault | false | `engine_poisoned` | -3 | Scripting disabled until VW restarts. |
| Script longer than 25 s | text `Error: "Main-thread dispatch timed out after 25s..."` | | | The script keeps running inside VW; the next call queues behind it. |
| Bridge not reachable | text `Connection error: ...` | | | Start the bridge. |

Rules that follow from the contract:

- **Print results to stdout as JSON.** Nothing else reaches you. Never write to stderr; if a
  library emits warnings, silence them (`import warnings; warnings.simplefilter("ignore")`).
- **Each call is a fresh interpreter context.** Globals, imports and handles from the previous
  call are gone. Put the whole job in one script.
- **One undo step per call.** Name it with `undo_label` so the user's Edit menu says what the
  agent did. `transaction=false` skips the undo event, so a failed script leaves partial changes.
- **Batch mutations in one script.** A call costs about 25 ms; an object created inside a script
  costs about 0.4 ms (100 rectangles in 38 ms, one Undo removes all 100).
- **Stay under 20 s per call.** Split long jobs. A timed-out script still runs to completion in
  VW and holds the main thread, so the next call waits for it.
- **Never open a dialog** (`vs.AlrtDialog`, file pickers, `vs.YNDialog`). It blocks the main
  thread until a human clicks; every call times out meanwhile.
- **Look up unfamiliar functions first** with `vs_signature`. Python argument order follows the
  VectorScript signature; functions with VAR parameters return them as a tuple.
- **Verify with reads.** `vs.Count('ALL')` for object counts, `vs.LNewObj()` for the handle of
  the last created object, `read_records_by_format` for data.
- Payloads of 200 KB stdout and 250 KB script round-trip fine; Unicode round-trips fine.

Template:

```python
import vs, json
out = {"name": vs.GetFName(), "classes": vs.ClassNum()}
print(json.dumps(out))
```

## 5. Safety rules

- **Never test against a document you cannot throw away.** `scripts/vw_relaunch.ps1 -Force`
  kills VW and relaunches it on a fresh copy of the blank test file at `%TEMP%\gantry-test.vwx`.
  `capability_bench.py` refuses mutation cases on any other document name.
- **Do not pass `py_mode=execute`.** It is the legacy bare `ExecuteScript` path kept only for the
  control experiment; it faults on the first object creation and poisons the engine.
- **After `script_crashed` or `engine_poisoned`:** tell the user to Save As a new file and
  restart Vectorworks. Do not keep calling. `scripts/vw_relaunch.ps1 -Force` does the restart
  when the open document is disposable.
- **First call after a document opens can take 3 to 14 s** (engine init). A `ping` sent while VW
  is still loading a file hits the 25 s ceiling and returns a timeout error, not a hang.
- The "Unknown Developer Plug-ins" dialog appears on every VW launch (no plugin credentials).
  First launch after a new `.vlb`: its first button is Restart. Later launches: Continue.

## 6. Known gaps

- `export_ifc` unverified (section 3).
- VectorScript: no stdout or stderr capture, only an error code.
- Native `create_*` tools are not undo steps.
- 25 s per call in the plugin dispatcher, 30 s in the server; no streaming or job model yet.
- No Mac build, no VW2027 (SDK 3200).
- No `.mcp.json` committed in this repo; copy the snippet in section 2.
- The engine-poison path is only exercised by `scripts/gantry_ladder.py --py-mode execute`,
  which requires a VW restart afterwards.

## 7. Verifying

| Script | What it proves | Needs |
|---|---|---|
| `python scripts/capability_bench.py` | One case per capability, pass/fail plus latency. Regenerates `docs/capabilities-matrix.md`. `--slow` adds the 25 s ceiling probe, `--ifc PATH` tries the export, `--read-only` skips mutations. | VW on the scratch doc |
| `python scripts/gantry_ladder.py` | The nine-step write-safety ladder over raw TCP, liveness check after every step. `--py-mode execute` is the crash control run (restart VW after). | VW on the scratch doc |
| `python scripts/mcp_smoke.py` | Spawns the release server over stdio and calls the read-only tools. | VW with any doc |
| `cargo test` in `mcp-server/` | `vs_signature` unit tests. | nothing |
| `python diag/probe_tcp_bridge.py` | Raw `ping` to the plugin, no server. | VW |

Expected numbers on the tested build (median, through stdio + server + TCP + plugin):
native read 4 ms, `run_script` 25 ms, L2 read 28 ms, `vs_signature` 0.3 ms.

## 8. Where things live

- `mcp-server/src/server.rs`: tool schemas, L2 Python bodies, `vs_signature`, server instructions.
- `mcp-server/src/bridge.rs`: TCP client, 30 s timeout, port mapping.
- `vw-plugin/src/BridgeCore.cpp`: command handler, `run_script` (script context, undo bracket,
  SEH guard, poison latch). `SocketBridge.cpp`: listener and 25 s dispatch ceiling.
  `MainThreadDispatch.cpp`: main-thread hop.
- `docs/decisions/001-scriptcontext-over-menu-broker.md`: why the script context, not a menu
  broker. `docs/coverage-spec.md`: the L0/L1/L2 model and the tool roadmap.
- `FORK_NOTES.md`: history, build and deploy steps, deployment naming.
