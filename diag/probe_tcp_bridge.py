"""TCP probe for the MCP Bridge — no MCP server, no Claude, no Rust.

Bypasses every layer above the plugin's TCP listener. If THIS works but
mcp__vectorworks__run_script via Claude doesn't, the failure is somewhere
above the socket (Rust server, MCP protocol, IPythonScriptEngine call).
If THIS fails but the VW palette shows the bridge as running, the
listener is in a broken state (singleton wipe, FD leak, etc.).

Usage:
    python diag/probe_tcp_bridge.py                  # default: VW2026 on 27231
    python diag/probe_tcp_bridge.py --port 27230     # VW2025
    python diag/probe_tcp_bridge.py --method get_drawing_info
    python diag/probe_tcp_bridge.py --method run_script --script "print('hi')"

Pure stdlib. Works on the laptop without needing the Rust server compiled.
"""

import argparse
import json
import socket
import sys
import time
import uuid


def probe(host: str, port: int, method: str, params: dict, timeout: float):
    req = {"id": f"diag_{uuid.uuid4().hex[:8]}", "method": method, "params": params}
    msg = (json.dumps(req) + "\n").encode("utf-8")

    t0 = time.monotonic()
    try:
        with socket.create_connection((host, port), timeout=timeout) as s:
            s.settimeout(timeout)
            s.sendall(msg)
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
        rtt_ms = (time.monotonic() - t0) * 1000.0
        line = buf.split(b"\n", 1)[0].decode("utf-8", errors="replace")
        try:
            resp = json.loads(line)
        except json.JSONDecodeError:
            return {"ok": False, "stage": "parse", "rtt_ms": rtt_ms, "raw": line}
        return {"ok": True, "stage": "complete", "rtt_ms": rtt_ms, "response": resp}
    except ConnectionRefusedError:
        return {
            "ok": False,
            "stage": "connect",
            "error": "connection refused",
            "hint": "Bridge listener is not running. Open VW, then the MCP Bridge palette, then Start.",
        }
    except socket.timeout:
        return {
            "ok": False,
            "stage": "timeout",
            "error": f"timed out after {timeout}s",
            "hint": "TCP accept() worked but the plugin never wrote a response. Likely a hang inside the command handler (IPythonScriptEngine deadlock?).",
        }
    except Exception as e:
        return {"ok": False, "stage": "unknown", "error": str(e), "type": type(e).__name__}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=27231, help="VW2026=27231, VW2025=27230")
    ap.add_argument("--method", default="ping",
                    help="ping | get_drawing_info | list_layers | run_script")
    ap.add_argument("--script", default=None,
                    help="Python source, only used when --method run_script")
    ap.add_argument("--language", default="python", help="python | vectorscript")
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()

    params = {}
    if args.method == "run_script":
        params = {"script": args.script or "print('tcp probe ok')", "language": args.language}

    print(f"-> {args.host}:{args.port}  method={args.method}  params={params}")
    result = probe(args.host, args.port, args.method, params, args.timeout)
    print(json.dumps(result, indent=2, default=str))
    sys.exit(0 if result.get("ok") else 1)


if __name__ == "__main__":
    main()
