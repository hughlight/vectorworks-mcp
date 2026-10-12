"""End-to-end MCP smoke test: spawn the Rust server on stdio and call tools.

Exercises the full chain Claude would use (JSON-RPC over stdio -> Rust server
-> TCP -> plugin -> VW) without needing the server registered in a client.

    python scripts/mcp_smoke.py                       # list tools + call the read-only ones
    python scripts/mcp_smoke.py --ifc "%TEMP%\\gantry-test.ifc"   # also export IFC
    python scripts/mcp_smoke.py --tool vs_signature --args "{\"name\": \"GetRField\"}"

Pure stdlib. Exit 0 if every call returned a result without "Error:".
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

SERVER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "mcp-server", "target", "release", "vectorworks-mcp-server.exe")


class Mcp:
    def __init__(self, vw_version: str):
        self.proc = subprocess.Popen(
            [SERVER, "--vw-version", vw_version],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, encoding="utf-8", bufsize=1,
        )
        self.next_id = 1
        assert self.proc.stdin is not None and self.proc.stdout is not None
        self.stdin = self.proc.stdin
        self.stdout = self.proc.stdout

    def request(self, method: str, params: dict | None = None) -> dict:
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}}
        self.next_id += 1
        self.stdin.write(json.dumps(msg) + "\n")
        self.stdin.flush()
        while True:
            line = self.stdout.readline()
            if not line:
                raise RuntimeError("server closed stdout")
            try:
                resp = json.loads(line)
            except json.JSONDecodeError:
                continue
            if resp.get("id") == msg["id"]:
                return resp

    def notify(self, method: str, params: dict | None = None) -> None:
        self.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method, "params": params or {}}) + "\n")
        self.stdin.flush()

    def call_tool(self, name: str, arguments: dict) -> str:
        resp = self.request("tools/call", {"name": name, "arguments": arguments})
        if "error" in resp:
            return "Error: " + json.dumps(resp["error"])
        parts = resp.get("result", {}).get("content", [])
        return "\n".join(p.get("text", "") for p in parts if p.get("type") == "text")

    def close(self) -> None:
        try:
            self.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            self.proc.kill()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vw-version", default="2026")
    ap.add_argument("--ifc", default=None, help="also call export_ifc with this output path")
    ap.add_argument("--tool", default=None, help="call just this tool")
    ap.add_argument("--args", default="{}", help="JSON arguments for --tool")
    ap.add_argument("--format", default="MCP_Test", help="record format for read_records_by_format")
    args = ap.parse_args()

    mcp = Mcp(args.vw_version)
    ok = True
    try:
        init = mcp.request("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "mcp_smoke", "version": "0"},
        })
        print("server:", init.get("result", {}).get("serverInfo"))
        mcp.notify("notifications/initialized")

        tools = mcp.request("tools/list").get("result", {}).get("tools", [])
        print("tools:", ", ".join(t["name"] for t in tools))

        if args.tool:
            calls = [(args.tool, json.loads(args.args))]
        else:
            calls = [
                ("get_drawing_info", {}),
                ("vs_signature", {"name": "NewField"}),
                ("list_classes", {}),
                ("list_record_formats", {}),
                ("read_records_by_format", {"format": args.format, "limit": 5}),
            ]
            if args.ifc:
                calls.append(("export_ifc", {"path": os.path.expandvars(args.ifc)}))

        for name, arguments in calls:
            out = mcp.call_tool(name, arguments)
            bad = out.startswith("Error") or out.startswith("Connection error")
            ok = ok and not bad
            print(f"\n== {name} {json.dumps(arguments)} -> {'FAIL' if bad else 'ok'}")
            print(out if len(out) <= 1500 else out[:1500] + f"\n... ({len(out)} chars)")
    finally:
        mcp.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
