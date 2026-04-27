# vectorworks-mcp — Fork Notes (last updated 2026-04-26)

**Upstream:** `https://github.com/mako-357/vectorworks-mcp` (1 star, first public release 2026-04-24)
**Local clone:** `D:\CODE\vectorworks-mcp\`
**Status: ✅ FULLY OPERABLE — all smoke tests green on VW2026, 2026-04-26.**

## Confirmed working end-to-end

Tom restarted VW2026 → workspace editor showed the plugin under "Unknown Developer" → he dragged it into a menu → clicked it → "MCP Bridge started at 127.0.0.1:27231" alert fired. Then:

| Test | Result |
|---|---|
| Direct TCP `ping` → `pong` | ✅ |
| `get_drawing_info` → structured drawing state | ✅ |
| `list_layers` → array of layer names | ✅ |
| Python `print('hello')` → stdout captured | ✅ |
| Python `vs.GetFName()` etc. — VW SDK access | ✅ |
| Python `raise Exception` → traceback in stderr, success=false, **no error dialog** | ✅ |
| Python JSON-via-stdout → parses cleanly | ✅ |
| Rust MCP server stdio → MCP `initialize` → `tools/call run_script` → structured response | ✅ |

The whole pipeline (Claude → stdio → Rust server → TCP → C++ plugin → IPythonScriptEngine → response) works. REMOTE_PY can retire.

## Update 2026-04-26 (afternoon): Palette + auto-start added

Tom asked for a Web Palette UI for the bridge with status display, start/stop toggle, and an auto-start-on-launch checkbox. Implemented and deployed.

**New files:**
- `vw-plugin/src/BridgeCore.{h,cpp}` — extracted command handler + persistent settings; one source of truth shared by menu and palette
- `vw-plugin/src/ExtMcpPalette.{h,cpp}` — `CMcpStatusPalette` (extends `VWExtensionWebPalette`) + `CMcpJSProvider` (extends `VWExtensionPaletteJSProvider`)
- `vw-plugin/vectorworks-mcp.vwr/Strings/Default.vwstrings` — UTF-16 LE BOM, fixes menu showing as literal "title"
- `vw-plugin/vectorworks-mcp.vwr/html/index.html` + `main.js` + `style.css` — palette UI
- Persistent settings at `%APPDATA%\Nemetschek\Vectorworks\<year>\vectorworks-mcp-config.json` — `{"auto_start": bool, "version": int}`

**Palette features:**
- Status dot (green = running, grey = stopped)
- Endpoint display: `127.0.0.1:<port>`
- VW version line: `VW2026 (SDK 3100)`
- Start/Stop toggle button (changes label + color based on state)
- Auto-start-on-launch checkbox (persists immediately when toggled)
- 2-second JS interval re-fetches status so external start/stop syncs back to the UI

**Auto-start mechanism:**
- `plugin_module_main` checks settings once per process load
- If `auto_start: true`, calls `vwmcp::StartBridgeIfNeeded()` silently (no alert)
- Will fire on the **next** VW launch, not retroactively when toggled

**JS↔C++ bridge functions** (palette→plugin):
- `requestStatus()` → triggers `updateStatus({...})` push to JS
- `startBridge()` / `stopBridge()`
- `setAutoStart(bool)` → persists to config file

**Menu name fix:**
- Was: literal "title" (showed under "Unknown Developer" → "title")
- Now: "MCP Bridge" under "Tools" category, with proper help text
- Resource bundle `vectorworks-mcp.vwr/Strings/Default.vwstrings` resolves the lookups

**Confirmed via live VW2026 session (final, after three follow-up fixes):**
- Palette window appears with title "MCP Bridge" ✅
- Settings file saved when checkbox toggled ✅
- Bridge starts/stops via palette toggle ✅
- **Auto-start fires after VW init settles** (28s into launch) ✅
- ping/pong rapid-fire (10 pings, ~10ms each, no drops) ✅
- run_script with print + `vs.*` SDK access + exception capture all green ✅

## Three follow-up fixes that landed during smoke testing

### 1. Singleton state-wipe bug

`SocketBridge::Instance()` was unconditionally resetting `m_serverFd = SOCK_INVALID` on every call. The first Start() bound a real socket; the next call to Instance() (from any other code path — PushStatus, the menu, etc.) wiped the FD value, leaving `m_running == true` (so the palette dot stayed green) but the worker thread doing `select()` on an invalid FD. Listener silently dead.

Fix: removed the reset, default-initialized `m_serverFd = (SocketFd)-1` in the header.

Tom's symptom that pinpointed it: "the dot stayed green from when I started it, until I stopped the bridge" — but TCP refused. State-says-running, listener-actually-dead is exactly what an FD-wipe race produces.

### 2. Auto-start was too eager → IPythonScriptEngine SEH-crashed VW

Auto-start fired on the first `plugin_module_main` call, which is BEFORE VW's scripting runtime is fully ready. Calling `IPythonScriptEngine::ExecuteScript` from a worker thread that early caused a Windows SEH exception that VW caught + cleanly shut itself down ("LastShutdown=Normal" in the log).

Fix: deferred auto-start until the 4th `plugin_module_main` invocation. VW calls main() multiple times during init for VCOM negotiation, so by call #4 the scripting runtime is generally ready.

### 3. Defensive no-document guard for `run_script`

Even with deferred auto-start, calling `run_script` on a fresh VW with no document open could still crash. Added a guard at the top of the run_script handler: if `gSDK->GetDrawingHeader()` returns null, return a structured error with `success: false` and a clear stderr message ("No active document — script execution refused. Open a document in Vectorworks before calling run_script.").

This is defense-in-depth — even if the timing changes in some future VW version, scripts can't crash the host.

## Build / deploy steps used in this session

1. Edited C++ source (added BridgeCore, ExtMcpPalette, refactored ExtMcpBridge to call into BridgeCore)
2. Updated `vectorworks-mcp.vcxproj` to include the new `.cpp`/`.h` and `.vwr` files
3. MSBuild Release|x64 against VW2026 SDK
4. Killed running VW2026 (it had the .vlb loaded)
5. Copied `vectorworks-mcp.vlb` to `%APPDATA%\...\Vectorworks\2026\Plug-ins\`
6. Copied `vectorworks-mcp.vwr/` directory to same location
7. Re-launched VW2026
8. Dismissed "Unknown Developer Plug-ins" dialog (Enable + Continue, one-time)
9. Opened palette via Window → Palettes → MCP Bridge
10. Clicked Start; bridge alert fired with the new endpoint string

## Known cosmetics still pending

- **"Unknown Developer Plug-ins" dialog on VW2026 launch** — fixable by generating a Plugin Credentials file per Vectorworks' developer scripting docs. Once credentialed, the dialog stops appearing.
- **No DevTools auto-open** — to debug palette JS, right-click inside the palette → Inspect (Chromium DevTools).
- **No icon on the palette title bar** — purely cosmetic.

## TL;DR — what's on disk right now

- **Plugin (VW2025 build)** at `%APPDATA%\Nemetschek\Vectorworks\2025\Plug-ins\vectorworks-mcp.vlb` — 664KB, MD5 `4b7bcf0b...`
- **Plugin (VW2026 build)** at `%APPDATA%\Nemetschek\Vectorworks\2026\Plug-ins\vectorworks-mcp.vlb` — 661KB, MD5 `8e525c71...`
- **Rust MCP server** at `D:\CODE\vectorworks-mcp\mcp-server\target\release\vectorworks-mcp-server.exe` — 3.6MB
- All builds clean, no warnings (one cosmetic post-build pwsh.exe message — ignorable)

---

## Architecture (revised)

```
Claude  --stdio-->  vectorworks-mcp-server.exe  --TCP localhost:port-->  vectorworks-mcp.vlb (in VW)
                    (Rust, rmcp crate)                                    (C++, VCOM SDK plugin)
```

**Why TCP not Unix socket?** Tokio's `UnixStream` is POSIX-only. Rather than fight Windows AF_UNIX in Rust (would need raw `windows-sys` + manual async glue), we use TCP localhost on both sides. Tokio TCP works everywhere, C++ TCP works everywhere, perf cost is negligible at our message rate.

**Per-version port allocation:**
- VW2025 plugin (SDK 3000) listens on `127.0.0.1:27230`
- VW2026 plugin (SDK 3100) listens on `127.0.0.1:27231`
- Formula: `27200 + (SDK_VERSION/100)`

Both VW versions can run concurrently with their own bridges, no collision.

**Server binary takes a `--vw-version` flag:**
```
vectorworks-mcp-server --vw-version 2025   # talks to port 27230
vectorworks-mcp-server --vw-version 2026   # talks to port 27231 (default)
```
Accepts either year (2025/2026) or short tag (30/31).

---

## Final steps to make it operable (Tom's hands needed)

### 1. Activate VW2025 (one-time)

VW2025 is installed but the license isn't activated on this machine. Launching VW2025 produces a `NetworkAlertQuestion` dialog asking for internet activation. I attempted a launch during this session; activation requires Tom's portal credentials and was deferred. Process was cleaned up; nothing left running.

If you want to run VW2025 alongside VW2026 for testing, walk through the activation prompt once. Per the LDF files we looked at earlier, your `G1DW29` serial covers VW2024/2025/2026 install slots — but online activation is per-version and may require sign-in.

If you decide skipping VW2025 and going straight to VW2026 is fine, that works too — the VW2026 plugin is already in place.

### 2. First-time menu setup (one VW restart needed, per VW)

On the next launch of either VW:

1. **Tools → Workspaces → Edit Workspace…**
2. In the left "Plug-ins" list, find **"MCP Bridge"** under the "title" category (the literal string "title" because we skipped the .vwstrings resource — cosmetic, will fix later)
3. Drag it into a menu (e.g. drop it under "Tools" or "Plug-ins")
4. Click OK, save the workspace

This is a one-time per-version setup. The menu stays put after that.

### 3. Click "MCP Bridge" — bridge starts

A successful start shows an alert: `MCP Bridge started at 127.0.0.1:27231` (or :27230 for VW2025). Click again to stop. The bridge runs on a background thread; VW UI stays responsive.

### 4. Configure `.mcp.json` for Claude Code

In any project where you want Claude to drive VW, add to `.mcp.json` at project root:

```json
{
  "mcpServers": {
    "vectorworks-2026": {
      "command": "D:/CODE/vectorworks-mcp/mcp-server/target/release/vectorworks-mcp-server.exe",
      "args": ["--vw-version", "2026"]
    },
    "vectorworks-2025": {
      "command": "D:/CODE/vectorworks-mcp/mcp-server/target/release/vectorworks-mcp-server.exe",
      "args": ["--vw-version", "2025"]
    }
  }
}
```

Restart Claude Code session. Tools `mcp__vectorworks-2026__run_script` etc. become available.

### 5. Smoke tests (in order of growing confidence)

**Test 1 — server reaches the plugin:**
With the bridge menu clicked in VW, run from a shell:
```
"D:/CODE/vectorworks-mcp/mcp-server/target/release/vectorworks-mcp-server.exe" --vw-version 2026
```
On stderr you should see:
```
Vectorworks MCP Server starting (vw_short=31)
Endpoint: 127.0.0.1:27231
Vectorworks bridge: connected
Vectorworks MCP Server running on stdio
```
The "connected" line is the proof point. Hit Ctrl+C to exit.

**Test 2 — Python execution with output capture (the big one):**
From Claude Code with the MCP server configured:
```
mcp__vectorworks-2026__run_script(
  script="print('hello from VW Python')",
  language="python"
)
```
Expected response: `{"stdout": "hello from VW Python\n", "stderr": "", "error_code": 0}`. The whole reason we did this work — Python with stdout capture replaces REMOTE_PY's saved-view trigger.

**Test 3 — error capture:**
```
mcp__vectorworks-2026__run_script(
  script="raise Exception('test from MCP')",
  language="python"
)
```
Expected: `stderr` contains the traceback, `success` is false.

**Test 4 — VW state read:**
```
mcp__vectorworks-2026__run_script(
  script="import vs; print(vs.GetFName()); print(vs.ClassNum())",
  language="python"
)
```
Expected: stdout has the current filename and class count.

If all four pass, REMOTE_PY can retire.

---

## What was actually built and how to rebuild

### C++ plugin
```
"F:\Visual_Studio\MSBuild\Current\Bin\MSBuild.exe" \
  "D:\CODE\vectorworks-mcp\vw-plugin\vectorworks-mcp.vcxproj" \
  -p:Configuration=Release -p:Platform=x64
```
Default builds for VW2025 (SDK at the path baked into the .vcxproj). For VW2026:
```
... -p:VWSDKRoot="D:\CODE\TH_VWX\VWX_SDK\2026-NNA-eng-win-SDK\SDK\SDKVW(832364)" \
    -p:OutDir="D:\CODE\vectorworks-mcp\vw-plugin\build\Release-VW2026\\" \
    -p:IntDir="D:\CODE\vectorworks-mcp\vw-plugin\build\Release-VW2026\obj\\"
```

Toolset: v145 (the modern VS2022 toolset on this box; required because the SDK lib uses `__CxxFrameHandler4` which v141 doesn't have).
Windows SDK: 10.0.26100.0 (only one installed; supports AF_INET fine).

### Rust server
```
cd D:\CODE\vectorworks-mcp\mcp-server
cargo build --release
```
Output at `target/release/vectorworks-mcp-server.exe`. Tokio + rmcp; no Windows-specific deps (TCP works everywhere).

---

## Source-level changes I made to upstream

1. **C++ plugin: Windows portability** (`SocketBridge.{h,cpp}`, `ExtMcpBridge.cpp`)
   - Switched from POSIX AF_UNIX to AF_INET TCP localhost on both POSIX and Windows
   - Inline platform shims (`socket_close`/`socket_read`/`socket_write`) so call sites stay readable
   - WSAStartup/WSACleanup wrapped around lifecycle for Windows
   - Bind explicitly to `127.0.0.1` (never the network interface)
   - SO_REUSEADDR for fast stop/start cycles

2. **C++ plugin: Python execution via IPythonScriptEngine** (`ExtMcpBridge.cpp`)
   - Upstream's `run_script` Python handler was a stub that returned "use VectorScript"
   - Replaced with `IPythonScriptEngine::ExecuteScript(script, &CDefaultPythonLogger())` — the SDK's proper API
   - Response now returns `{stdout, stderr, error_code}` from the logger
   - VS path also upgraded to `IVectorScriptEngine::ExecuteScript` (returns error code; output capture still SDK-blocked on that interface)

3. **C++ plugin: SDK macro corrections** (`ExtMcpBridge.h`)
   - Upstream used `DEFINE_VWMenuExtensionClass` and `DEFINE_VWMenuEventSinkClass` — these macros don't exist in the modern SDK
   - Correct macros: `DEFINE_VWMenuExtension` (no Class suffix, on the menu extension only — not on the event sink)
   - This was the headline bug that prevented the upstream code from compiling against the current SDK

4. **C++ plugin: API signature fixes** (`ExtMcpBridge.cpp`)
   - `gSDK->CreateOval(WorldPt, WorldPt)` → `gSDK->CreateOval(WorldRect)` (1-arg, current SDK)
   - `TXString::Append(...)` → `+=` operator (Append doesn't exist on TXString in current SDK)

5. **C++ plugin: dual-target VW25/VW26**
   - Menu `VersionCreated` dropped from 26 to 25 so the same source loads on both versions
   - `SDK_VERSION` (auto-defined per the SDK headers) computes the per-version port

6. **C++ plugin: ModuleMain namespace fix**
   - `using namespace Extensions` was missing — `REGISTER_Extension<CMcpBridge>` couldn't find the class

7. **C++ plugin: project scaffolding** (new, `vectorworks-mcp.vcxproj` + `src/StdAfx.h`)
   - Upstream had no .vcxproj — only source files
   - Created from scratch using VGSSample as a template
   - Standalone (not nested in SDK tree) using absolute paths to SDK includes/libs
   - PCH disabled (simpler, no first-include rule)
   - `<TreatWChar_tAsBuiltInType>true</TreatWChar_tAsBuiltInType>` (else `wchar_t` collides with `UCChar` typedef → all TXString methods fail to compile)

8. **Rust server: Windows port via TCP**
   - Replaced `std::os::unix::net::UnixStream` with `tokio::net::TcpStream`
   - Made `bridge::send` async; cascaded through `server.rs::call`
   - Added `--vw-version` CLI flag for per-VW addressing

9. **Build system**
   - VS toolset v145, Windows SDK 10.0.26100.0 (only one installed)
   - Linker deps: `VWSDK.lib;VWMM.lib;GSWinSDK.lib;USP10.lib;Rpcrt4.lib;ws2_32.lib`

---

## Known cosmetic issues (worth fixing later, none blocking)

1. **Menu name shows as literal "title"** — we skipped the .vwr/.vwstrings resource step. The plugin works; the menu name is just ugly. Fix: create `vectorworks-mcp.vwr/Strings/CMcpBridge.vwstrings` and run the SDK's `buildvwr.exe` post-build, or hardcode the strings in `gMenuDef`.

2. **Cosmetic post-build error**: `'pwsh.exe' is not recognized` — comes from a PowerShell post-build event in the v145 toolset's NNA_BuildInstrumentation_ALL.targets. Doesn't affect the build output. Fix: add `-p:DisableNNABuildInstrumentation=true` or remove the import.

3. **VS Project is local-pathed** — the absolute path to the SDK is hardcoded in the `.vcxproj`. If you move the SDK or check this in for someone else, override with `-p:VWSDKRoot=...`.

4. **No automatic workspace menu install** — first-time setup requires manual drag in Tools → Workspaces → Edit Workspace. Could be solved by editing the .vww XML directly, but the format is fragile and a botched edit can break Tom's whole workspace.

---

## What still needs to happen for a green smoke-test

1. (Tom) Restart VW2026 to load the new plugin (next natural restart is fine; no rush)
2. (Tom) Tools → Workspaces → Edit Workspace, drag MCP Bridge into a menu
3. (Tom) Click MCP Bridge — alert should say "MCP Bridge started at 127.0.0.1:27231"
4. (Tom) Add `.mcp.json` entry per the snippet above
5. (Tom or Claude) Run the four smoke tests above
6. (Tom) If all green → start retiring REMOTE_PY in favor of MCP `run_script`

---

## Files in this fork (D:\CODE\vectorworks-mcp\)

```
.gitignore
LICENSE                                  (upstream MIT)
README.md                                (upstream)
FORK_NOTES.md                            (this file)
vw-plugin/
├── src/
│   ├── ModuleMain.cpp                   (modified — namespace fix)
│   ├── ExtMcpBridge.h                   (modified — macro fix, struct cleanup)
│   ├── ExtMcpBridge.cpp                 (modified — Python via IPythonScriptEngine, port-based startup, API signature fixes)
│   ├── SocketBridge.h                   (rewritten — cross-platform TCP)
│   ├── SocketBridge.cpp                 (rewritten — cross-platform TCP)
│   └── StdAfx.h                         (NEW — minimal precompiled-header substitute)
├── vectorworks-mcp.vcxproj              (NEW — VS 2022 build configuration)
└── build/
    ├── Release/vectorworks-mcp.vlb              (VW2025 build)
    └── Release-VW2026/vectorworks-mcp.vlb       (VW2026 build)
mcp-server/
├── Cargo.toml                           (unchanged)
└── src/
    ├── main.rs                          (modified — --vw-version flag, async)
    ├── bridge.rs                        (rewritten — TCP, async, version-aware)
    └── server.rs                        (rewritten — async tools, version-aware)
```
