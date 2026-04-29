# Fuse VW Gantry

Vectorworks plugin + MCP server bridging Claude (or any MCP client) to Vectorworks 2025/2026 on Windows.

> **Lineage:** This repo is a substantially rewritten fork of [mako-357/vectorworks-mcp](https://github.com/mako-357/vectorworks-mcp). Upstream contributed the architectural sketch (C++ plugin owns a local socket, Rust process bridges to MCP) and the wire-protocol tool shapes. The implementation has been ported to Windows, hardened for production use, extended with a status palette + auto-start, and dual-targeted at VW2025 + VW2026. See `FORK_NOTES.md` for details. The repo name remains `vectorworks-mcp` for upstream alignment; the plugin itself is Fuse VW Gantry.

## Architecture

```
Claude  --stdio-->  fuse-vw-gantry-server.exe  --TCP localhost-->  FuseVWGantry.vlb (in VW)
                    (Rust, rmcp crate)                              (C++, VCOM SDK plugin)
```

VW2025 plugin listens on `127.0.0.1:27230`, VW2026 on `127.0.0.1:27231` (formula: `27200 + SDK_VERSION/100`). Both VW versions can run concurrently.

## MCP tools

| Tool | Description |
|------|-------------|
| `get_drawing_info` | Document state — has-document, layer count |
| `list_layers` | Layer names in current document |
| `create_line` | Draw a line segment |
| `create_rect` | Draw a rectangle |
| `create_circle` | Draw a circle |
| `run_script` | Execute Python or VectorScript with stdout/stderr capture |

## Prerequisites

- Vectorworks 2025 or 2026 (Windows)
- [Vectorworks SDK](https://github.com/Vectorworks/developer-sdk) for the matching VW version
- Visual Studio 2022 (toolset v145, Windows 10 SDK 10.0.26100.0)
- [Rust](https://rustup.rs) for the MCP server

## Build

### Rust MCP server

```bash
cd mcp-server
cargo build --release
```

Output: `mcp-server/target/release/fuse-vw-gantry-server.exe`.

### C++ plugin

Default build targets VW2025:

```bash
"F:/Visual_Studio/MSBuild/Current/Bin/MSBuild.exe" \
  "D:/CODE/vectorworks-mcp/vw-plugin/vectorworks-mcp.vcxproj" \
  -p:Configuration=Release -p:Platform=x64
```

For VW2026, override `VWSDKRoot`:

```bash
... -p:VWSDKRoot="D:/CODE/TH_VWX/VWX_SDK/2026-NNA-eng-win-SDK/SDK/SDKVW(832364)" \
    -p:OutDir="D:/CODE/vectorworks-mcp/vw-plugin/build/Release-VW2026/" \
    -p:IntDir="D:/CODE/vectorworks-mcp/vw-plugin/build/Release-VW2026/obj/"
```

Output: `vw-plugin/build/Release/FuseVWGantry.vlb` (or `Release-VW2026/`).

## Install

Copy to the VW plugin folder for the matching version:

```
%APPDATA%/Nemetschek/Vectorworks/<year>/Plug-ins/
├── FuseVWGantry.vlb
└── FuseVWGantry.vwr/    (entire directory — strings + palette HTML/JS)
```

For shared-team distribution via a workgroup folder (e.g. Dropbox), drop the same files into `<WorkgroupRoot>/<year>/Plug-ins/` instead.

## Setup

1. Restart Vectorworks. Dismiss the "Unknown Developer Plug-ins" dialog (one-time, until plugin credentials registration).
2. Open the palette: **Window → Palettes → Fuse VW Gantry**.
3. Click **Start bridge**. The status dot turns green; endpoint shows `127.0.0.1:27231` (VW2026).
4. Optional: tick **Start bridge on Vectorworks launch** to auto-start on next VW boot.
5. (Optional) Add the menu command via **Tools → Workspaces → Edit Workspace** — drag "Fuse VW Gantry" from the Fuse category into a menu.

## Claude Code configuration

Add to `.mcp.json` at any project root:

```json
{
  "mcpServers": {
    "fuse-vw-gantry-2026": {
      "command": "D:/CODE/vectorworks-mcp/mcp-server/target/release/fuse-vw-gantry-server.exe",
      "args": ["--vw-version", "2026"]
    },
    "fuse-vw-gantry-2025": {
      "command": "D:/CODE/vectorworks-mcp/mcp-server/target/release/fuse-vw-gantry-server.exe",
      "args": ["--vw-version", "2025"]
    }
  }
}
```

Restart the Claude Code session. Tools `mcp__fuse-vw-gantry-2026__run_script` etc. become available.

## Settings

Auto-start preference is persisted at:
`%APPDATA%/Nemetschek/Vectorworks/<year>/fuse-vw-gantry-config.json`

## License

MIT (inherited from upstream).
