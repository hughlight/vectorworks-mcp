"""Live write-safety ladder for Gantry's run_script.

Talks to the plugin's TCP listener directly (newline-delimited JSON, see
vw-plugin/src/SocketBridge.cpp) so no MCP server or client is involved.
Runs the handoff's test ladder in order, checks after every step that the
Vectorworks main process is still alive, writes a JSON record and prints a
markdown table.

    python scripts/gantry_ladder.py                      # new path (py_mode=context)
    python scripts/gantry_ladder.py --py-mode execute    # control run: expected to crash VW
    python scripts/gantry_ladder.py --port 27230         # VW2025

Exit 0 when every step passed, 1 otherwise. Pure stdlib.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import socket
import subprocess
import sys
import time
import uuid

HOST = "127.0.0.1"
RECORD_NAME = "MCP_Test"


# ---------------------------------------------------------------- transport

class VwGone(Exception):
    """The TCP side failed in a way that means VW (or the bridge) is gone."""


def rpc(port: int, method: str, params: dict, timeout: float = 40.0) -> dict:
    req = {"id": "ladder_" + uuid.uuid4().hex[:8], "method": method, "params": params}
    try:
        with socket.create_connection((HOST, port), timeout=timeout) as s:
            s.settimeout(timeout)
            s.sendall((json.dumps(req) + "\n").encode("utf-8"))
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(65536)
                if not chunk:
                    raise VwGone("connection closed before a response line arrived")
                buf += chunk
    except (ConnectionRefusedError, ConnectionResetError, ConnectionAbortedError) as e:
        raise VwGone(f"{type(e).__name__}: {e}") from e
    except socket.timeout as e:
        raise VwGone(f"timeout after {timeout}s (VW hung or blocked on a dialog)") from e
    return json.loads(buf.split(b"\n", 1)[0].decode("utf-8", "replace"))


# ---------------------------------------------------------------- process checks

def vw_main_pid() -> int | None:
    """PID of the Vectorworks2026 process that owns the main window (CEF helpers excluded)."""
    cmd = ["powershell", "-NoProfile", "-Command",
           "Get-Process Vectorworks2026 -ErrorAction SilentlyContinue | "
           "Where-Object MainWindowTitle | Select-Object -First 1 -ExpandProperty Id"]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout.strip()
        return int(out) if out else None
    except (subprocess.SubprocessError, ValueError):
        return None


def pid_alive(pid: int) -> bool:
    out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH", "/FO", "CSV"],
                         capture_output=True, text=True).stdout
    return f'"{pid}"' in out


# ---------------------------------------------------------------- ladder

class Ladder:
    def __init__(self, port: int, py_mode: str):
        self.port = port
        self.py_mode = py_mode
        self.rows: list[dict] = []
        self.main_pid = vw_main_pid()
        self.engine_poisoned = False

    # -- helpers --------------------------------------------------------
    def run(self, script: str, **extra) -> dict:
        params = {"script": script, "language": "python", "py_mode": self.py_mode}
        params.update(extra)
        return rpc(self.port, "run_script", params)

    def data(self, resp: dict) -> dict:
        d = resp.get("data")
        return d if isinstance(d, dict) else {"raw": d}

    def count_all(self) -> int:
        resp = self.run("import vs; print(vs.Count('ALL'))", transaction=False)
        if not resp.get("success"):
            raise RuntimeError(f"count failed: {self.data(resp)}")
        return int(float(self.data(resp)["stdout"].strip()))   # vs.Count prints e.g. "0.0"

    def step(self, name: str, fn):
        t0 = time.monotonic()
        row = {"step": name, "passed": False, "note": "", "response": None}
        try:
            if self.engine_poisoned:
                # After a contained fault every call must be refused.
                resp = self.run("print('after poison')")
                d = self.data(resp)
                row["response"] = resp
                row["passed"] = (not resp.get("success")) and d.get("error") == "engine_poisoned"
                row["note"] = "skipped body; checked engine_poisoned refusal"
            else:
                passed, note, resp = fn()
                row.update(passed=bool(passed), note=note, response=resp)
                d = self.data(resp) if isinstance(resp, dict) else {}
                if d.get("error") == "script_crashed":
                    self.engine_poisoned = True
                    row["note"] += f" | CONTAINED FAULT {d.get('fault_code')} -> engine poisoned"
        except VwGone as e:
            row["note"] = f"VW GONE: {e}"
            row["vw_gone"] = True
        except Exception as e:  # noqa: BLE001 - we want every failure in the table
            row["note"] = f"{type(e).__name__}: {e}"
        row["ms"] = round((time.monotonic() - t0) * 1000)
        row["vw_alive"] = bool(self.main_pid and pid_alive(self.main_pid))
        if not row["vw_alive"]:
            row["passed"] = False
            row["note"] = (row["note"] + " | VW PROCESS DIED").strip(" |")
        self.rows.append(row)
        mark = "PASS" if row["passed"] else "FAIL"
        print(f"[{mark}] {name:<34} {row['ms']:>6} ms  alive={row['vw_alive']}  {row['note']}")
        return row

    # -- steps ----------------------------------------------------------
    def s_ping(self):
        resp = rpc(self.port, "ping", {})
        return resp.get("data") == "pong", "", resp

    def s_print(self):
        resp = self.run("print('hi')")
        d = self.data(resp)
        return resp.get("success") and d.get("stdout", "").strip() == "hi", "", resp

    def s_fname(self):
        resp = self.run("import vs; print(vs.GetFName())")
        d = self.data(resp)
        return bool(resp.get("success")), f"doc={d.get('stdout', '').strip()!r}", resp

    def s_rect(self):
        resp = self.run("import vs; vs.Rect(0,0,100,100); print('rect')")
        return bool(resp.get("success")), "", resp

    def s_newfield(self):
        resp = self.run(f"import vs; print(vs.NewField('{RECORD_NAME}','f1','x',4,0))")
        d = self.data(resp)
        return bool(resp.get("success")), f"NewField -> {d.get('stdout', '').strip()}", resp

    def s_buildresourcelist(self):
        script = (
            "import vs\n"
            "lid, n = vs.BuildResourceList(47, 0, '')\n"
            "names = [vs.GetNameFromResourceList(lid, i) for i in range(1, n + 1)]\n"
            "print(n); print(names)\n"
        )
        resp = self.run(script)
        d = self.data(resp)
        out = d.get("stdout", "")
        return resp.get("success") and RECORD_NAME in out, f"stdout={out.strip()[:160]!r}", resp

    def s_raise(self):
        resp = self.run("raise Exception('x')")
        d = self.data(resp)
        err = d.get("stderr", "")
        ok = (not resp.get("success")) and ("Traceback" in err or "Exception: x" in err) \
            and d.get("error") is None
        return ok, f"stderr={err.strip()[-120:]!r}", resp

    def s_rollback(self):
        before = self.count_all()
        resp = self.run("import vs; vs.Rect(0,0,50,50); raise Exception('rollback')")
        d = self.data(resp)
        after = self.count_all()
        ok = (not resp.get("success")) and d.get("rolled_back") is True and before == after
        return ok, f"count {before} -> {after}, rolled_back={d.get('rolled_back')}", resp

    def s_ten_undo_steps(self):
        before = self.count_all()
        last = None
        for i in range(10):
            last = self.run(f"import vs; vs.Rect({i*10},0,{i*10+5},5)", undo_label=f"ladder {i+1}")
            if not last.get("success"):
                return False, f"call {i+1} failed: {self.data(last)}", last
        mid = self.count_all()
        undo = self.run("import vs; vs.DoMenuTextByName('Undo', 0)", transaction=False)
        after = self.count_all()
        ok = (mid == before + 10) and (after == mid - 1)
        note = f"count {before} -> {mid} after 10 calls -> {after} after one Undo"
        return ok, note, {"last_call": last, "undo": undo}

    def run_all(self):
        print(f"ladder py_mode={self.py_mode} port={self.port} main_pid={self.main_pid}")
        self.step("1 ping", self.s_ping)
        self.step("2 print('hi')", self.s_print)
        self.step("3 vs.GetFName()", self.s_fname)
        self.step("4 vs.Rect (object create)", self.s_rect)
        self.step("5 vs.NewField (record format)", self.s_newfield)
        self.step("6 BuildResourceList(47) + names", self.s_buildresourcelist)
        self.step("7 raise -> success:false", self.s_raise)
        self.step("8 mutate+raise rolled back", self.s_rollback)
        self.step("9 ten calls, one Undo each", self.s_ten_undo_steps)
        return all(r["passed"] for r in self.rows)


def markdown(rows: list[dict]) -> str:
    lines = ["| Step | Result | ms | VW alive | Note |", "|---|---|---|---|---|"]
    for r in rows:
        note = r["note"].replace("|", "\\|")
        lines.append(f"| {r['step']} | {'PASS' if r['passed'] else 'FAIL'} | {r['ms']} | "
                     f"{'yes' if r['vw_alive'] else 'NO'} | {note} |")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=27231)
    ap.add_argument("--py-mode", choices=["context", "execute"], default="context")
    ap.add_argument("--out-dir", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "docs", "test-runs"))
    args = ap.parse_args()

    ladder = Ladder(args.port, args.py_mode)
    all_ok = ladder.run_all()

    os.makedirs(args.out_dir, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y-%m-%d-%H%M%S")
    path = os.path.join(args.out_dir, f"ladder-{args.py_mode}-{stamp}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"py_mode": args.py_mode, "port": args.port, "main_pid": ladder.main_pid,
                   "timestamp": stamp, "all_passed": all_ok, "rows": ladder.rows},
                  f, indent=2, default=str)
    print()
    print(markdown(ladder.rows))
    print(f"\nrecord: {path}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
