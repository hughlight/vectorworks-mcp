"""Run a Python file inside Vectorworks through the MCP server and print the result.

    python scripts/vw_run.py path/to/script.py            # transaction on (one undo step)
    python scripts/vw_run.py path/to/script.py --no-tx    # transaction off
    python scripts/vw_run.py path/to/script.py --label "place fixtures"
    python scripts/vw_run.py --code "import vs; print(vs.GetFName())"

Prints stdout, then stderr (if any), then the remaining response fields.
Exit 0 on success:true. Pure stdlib; needs the release server built and VW running.
"""

from __future__ import annotations

# pyright: reportMissingImports=false

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_smoke import Mcp  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", help="Python file to run inside VW")
    ap.add_argument("--code", help="inline script instead of a file")
    ap.add_argument("--no-tx", action="store_true", help="transaction=false")
    ap.add_argument("--label", default=None, help="undo label")
    ap.add_argument("--vectorscript", action="store_true")
    ap.add_argument("--vw-version", default="2026")
    ap.add_argument("--raw", action="store_true", help="print the raw tool text only")
    args = ap.parse_args()

    if args.code is not None:
        script = args.code
    elif args.file:
        with open(args.file, encoding="utf-8") as f:
            script = f.read()
    else:
        ap.error("give a file or --code")

    params = {"script": script, "language": "vectorscript" if args.vectorscript else "python"}
    if args.no_tx:
        params["transaction"] = False
    if args.label:
        params["undo_label"] = args.label

    mcp = Mcp(args.vw_version)
    try:
        mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                   "clientInfo": {"name": "vw_run", "version": "0"}})
        mcp.notify("notifications/initialized")
        out = mcp.call_tool("run_script", params)
    finally:
        mcp.close()

    if args.raw:
        print(out)
        return 0 if not out.startswith(("Error", "Connection error")) else 1

    ok = not out.startswith(("Error", "Connection error"))
    body = out[len("Error: "):] if out.startswith("Error: ") else out
    try:
        data = json.loads(body)
    except json.JSONDecodeError:
        print(out)
        return 0 if ok else 1
    if not isinstance(data, dict):
        print(out)
        return 0 if ok else 1
    sys.stdout.write(data.get("stdout", ""))
    if data.get("stderr"):
        sys.stdout.write("\n--- stderr ---\n" + data["stderr"])
    rest = {k: v for k, v in data.items() if k not in ("stdout", "stderr")}
    print(f"\n--- {'ok' if ok else 'FAILED'} {json.dumps(rest)}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
