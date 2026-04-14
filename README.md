# Vectorworks MCP

Claude (or any LLM) から Vectorworks を操作するための [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) サーバー。

Vectorworks SDK C++ プラグインを Vectorworks 内にロードし、Unix Domain Socket 経由で MCP サーバーと通信することで、LLM が Vectorworks の全機能にアクセスできます。

**世界初の Vectorworks MCP サーバー。**

## Architecture

```
Claude ──stdio──> MCP Server (Rust)
                       │
                  Unix Socket (/tmp/vw-mcp-bridge.sock)
                       │
                  Vectorworks Plugin (.vwlibrary)
                       │
                  Vectorworks (VCOM SDK)
```

## MCP Tools

| Tool | Description |
|------|-------------|
| `get_drawing_info` | 図面情報を取得 |
| `list_layers` | レイヤー一覧を取得 |
| `create_line` | 線分を描画 |
| `create_rect` | 四角形を描画 |
| `create_circle` | 円を描画 |
| `run_script` | VectorScript / Python スクリプトを実行 |

## Prerequisites

- **Vectorworks 2025+** (Mac or Windows)
- **Vectorworks SDK** ([GitHub](https://github.com/Vectorworks/developer-sdk))
- **Xcode** (Mac) or **Visual Studio** (Windows)
- **Rust** (for MCP server): [rustup.rs](https://rustup.rs/)

## Build

### MCP Server (Rust)

```bash
cd mcp-server
cargo build
```

### Vectorworks Plugin (C++)

Requires Vectorworks SDK. See [SDK Examples](https://github.com/VectorworksDeveloper/SDKExamples) for build setup.

## Setup

### 1. Load the plugin in Vectorworks

Place the built plugin in the Vectorworks `Plug-ins` folder.

### 2. Start the MCP Bridge

In Vectorworks, run the "MCP Bridge" menu command to start the socket server.

### 3. Configure Claude Code

Add to your project's `.mcp.json`:

```json
{
  "mcpServers": {
    "vectorworks": {
      "command": "/path/to/vectorworks-mcp-server",
      "args": []
    }
  }
}
```

## Related

- [autocad-mcp](https://github.com/mako-357/autocad-mcp) — AutoCAD MCP server
- [gfp-cad](https://github.com/mako-357/gfp-cad) — Lightweight BIM with multi-CAD output

## License

MIT
