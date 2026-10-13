"""Capability suite and benchmark for the Vectorworks MCP server (Gantry).

Drives the release server over stdio, the same path an agent uses, and runs
one case per advertised capability: a pass/fail check against what the docs
promise, timing on every call, latency samples for the hot paths, and a few
probes for the edges agents hit (payload size, Unicode, the 25 s ceiling).

Writes two things:
  docs/test-runs/bench-<stamp>.json   full record, every response kept
  docs/capabilities-matrix.md         regenerated evidence table for agents

Safety: mutation cases run only when the open document is named
gantry-test*.vwx (see scripts/vw_relaunch.ps1) or was allowed with
--allow-doc. Nothing is ever saved. The engine_poisoned path is not
exercised here because it needs a Vectorworks restart afterwards; use
scripts/gantry_ladder.py --py-mode execute for that.

    python scripts/capability_bench.py               # full suite on the scratch doc
    python scripts/capability_bench.py --read-only   # no mutation cases
    python scripts/capability_bench.py --slow        # add the 25 s ceiling probe (~30 s)
    python scripts/capability_bench.py --ifc "%TEMP%\\gantry-bench.ifc"
    python scripts/capability_bench.py --repeat 30   # latency sample size (default 20)

Exit 0 when no scored case failed. Pure stdlib.
"""

from __future__ import annotations

# pyright: reportMissingImports=false

import argparse
import datetime as dt
import json
import os
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from mcp_smoke import Mcp  # noqa: E402
from gantry_ladder import pid_alive, vw_main_pid  # noqa: E402

VW_EXE = r"C:\Program Files\Vectorworks 2026\Vectorworks2026.exe"
RECORD = "MCP_Bench"
PASS, FAIL, INFO, SKIP, UNVERIFIED = "PASS", "FAIL", "INFO", "SKIP", "UNVERIFIED"


# ---------------------------------------------------------------- helpers

def parse_tool(out: str):
    """Split a tool's text result into (ok, data).

    The server returns pretty JSON on success, "Error: <json or text>" when
    the plugin said success:false, and "Connection error: ..." when it could
    not reach Vectorworks. L2 tools return "Error: <stderr>" on failure.
    """
    if out.startswith("Connection error"):
        return False, out
    if out.startswith("Error: "):
        body = out[len("Error: "):]
        try:
            return False, json.loads(body)
        except json.JSONDecodeError:
            return False, body
    try:
        return True, json.loads(out)
    except json.JSONDecodeError:
        return True, out


def any_vw_process() -> bool:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Vectorworks2026.exe", "/NH", "/FO", "CSV"],
                         capture_output=True, text=True).stdout
    return '"Vectorworks2026.exe"' in out


def stats(samples: list[float]) -> dict:
    s = sorted(samples)
    return {
        "n": len(s),
        "min_ms": round(s[0], 1),
        "median_ms": round(statistics.median(s), 1),
        "p95_ms": round(s[min(len(s) - 1, int(len(s) * 0.95))], 1),
        "max_ms": round(s[-1], 1),
    }


def vw_exe_version() -> str:
    cmd = ["powershell", "-NoProfile", "-Command",
           f"(Get-Item '{VW_EXE}').VersionInfo.FileVersion"]
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout.strip().replace(", ", ".")
    except subprocess.SubprocessError:
        return "unknown"


def git_head() -> str:
    try:
        return subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True, timeout=10).stdout.strip()
    except subprocess.SubprocessError:
        return "unknown"


# ---------------------------------------------------------------- bench

class Bench:
    def __init__(self, mcp: Mcp, repeat: int, allow_mutation: bool):
        self.mcp = mcp
        self.repeat = repeat
        self.allow_mutation = allow_mutation
        self.rows: list[dict] = []
        self.main_pid = vw_main_pid(27200 + (31 if not hasattr(mcp, "port") else mcp.port))
        if self.main_pid is None:
            print("!! could not identify the main Vectorworks process; liveness falls back to 'any VW process exists'")

    # -- transport ------------------------------------------------------
    def call(self, tool: str, args: dict | None = None):
        """Call a tool; return (ok, data, ms, raw_text)."""
        t0 = time.perf_counter()
        raw = self.mcp.call_tool(tool, args or {})
        ms = (time.perf_counter() - t0) * 1000
        ok, data = parse_tool(raw)
        return ok, data, ms, raw

    def py(self, script: str, **extra):
        args = {"script": script, "language": "python"}
        args.update(extra)
        return self.call("run_script", args)

    def stdout(self, script: str, **extra) -> str:
        ok, data, _, raw = self.py(script, **extra)
        if not ok or not isinstance(data, dict):
            raise RuntimeError(f"run_script failed: {raw[:300]}")
        return data.get("stdout", "")

    def count_all(self) -> int:
        return int(float(self.stdout("import vs; print(vs.Count('ALL'))", transaction=False).strip()))

    def undo_once(self):
        return self.py("import vs; vs.DoMenuTextByName('Undo', 0)", transaction=False)

    def doc_name(self) -> str:
        return self.stdout("import vs; print(vs.GetFName())", transaction=False).strip()

    # -- case runner ----------------------------------------------------
    def case(self, cid: str, level: str, fn, *, mutates: bool = False, caveat: str = "",
             scored: bool = True):
        row = {"id": cid, "level": level, "mutates": mutates, "status": FAIL, "note": "",
               "ms": 0, "caveat": caveat, "scored": scored, "detail": None}
        t0 = time.perf_counter()
        if mutates and not self.allow_mutation:
            row["status"], row["note"] = SKIP, "mutation case; document not disposable or --read-only"
        else:
            try:
                status, note, detail = fn()
                row.update(status=status, note=note, detail=detail)
            except Exception as e:  # noqa: BLE001 - every failure belongs in the table
                row["status"], row["note"] = FAIL, f"{type(e).__name__}: {e}"
        row["ms"] = round((time.perf_counter() - t0) * 1000)
        row["vw_alive"] = pid_alive(self.main_pid) if self.main_pid else any_vw_process()
        if not row["vw_alive"]:
            row["status"], row["note"] = FAIL, (row["note"] + " | VW PROCESS DIED").strip(" |")
        self.rows.append(row)
        print(f"[{row['status']:<10}] {cid:<36} {row['ms']:>7} ms  {row['note'][:110]}")
        return row

    # ------------------------------------------------------------------
    # L1: native typed handlers in the C++ plugin
    # ------------------------------------------------------------------
    def c_get_drawing_info(self):
        ok, d, ms, raw = self.call("get_drawing_info")
        good = ok and isinstance(d, dict) and d.get("has_document") is True and isinstance(d.get("layer_count"), int)
        return (PASS if good else FAIL), f"{d}", raw

    def c_list_layers(self):
        ok, d, ms, raw = self.call("list_layers")
        good = ok and isinstance(d, list) and len(d) >= 1 and all("name" in x for x in d)
        return (PASS if good else FAIL), f"{len(d) if isinstance(d, list) else 0} layers", raw

    def _create_primitive(self, tool: str, args: dict):
        def fn():
            before = self.count_all()
            ok, d, ms, raw = self.call(tool, args)
            after = self.count_all()
            good = ok and after == before + 1
            return (PASS if good else FAIL), f"count {before} -> {after}, {d!r}", raw
        return fn

    def c_l1_undoable(self):
        # Make the previous undo step a 2-object run_script event. If Undo
        # after create_rect removes 1 object the L1 primitive is undoable;
        # if it removes 2, the primitive is not an undo event and Undo took
        # the earlier step instead.
        self.py("import vs; vs.Rect(0,0,1,1); vs.Rect(2,2,3,3)", undo_label="bench pair")
        before = self.count_all()
        self.call("create_rect", {"x": 500, "y": 500, "width": 10, "height": 10})
        mid = self.count_all()
        self.undo_once()
        after = self.count_all()
        if mid != before + 1:
            return FAIL, f"create_rect did not add one object ({before} -> {mid})", None
        if after == mid - 1:
            return INFO, "create_rect IS its own undo step (Undo removed 1)", {"before": before, "mid": mid, "after": after}
        if after == mid - 2:
            return INFO, "create_rect is NOT an undo step: Undo removed the previous run_script pair (2) and the L1 rect stayed", {"before": before, "mid": mid, "after": after}
        return INFO, f"unclear: {before} -> {mid} -> {after}", {"before": before, "mid": mid, "after": after}

    # ------------------------------------------------------------------
    # L0: run_script
    # ------------------------------------------------------------------
    def c_print(self):
        ok, d, ms, raw = self.py("print('hi')")
        good = ok and isinstance(d, dict) and d.get("stdout", "").strip() == "hi" and d.get("error_code") == 0
        return (PASS if good else FAIL), f"stdout={d.get('stdout') if isinstance(d, dict) else raw!r}", raw

    def c_stdout_json(self):
        out = self.stdout("import json; print(json.dumps({'a': 1, 'b': [1, 2]}))", transaction=False)
        parsed = json.loads(out)
        return (PASS if parsed == {"a": 1, "b": [1, 2]} else FAIL), out.strip(), out

    def c_unicode(self):
        text = "caf\u00e9 \u65e5\u672c \u2014 \u00fc"
        out = self.stdout(f"print({text!r})", transaction=False)
        got = out.rstrip("\r\n")
        return (PASS if got == text else FAIL), f"sent {text!r} got {got!r}", out

    def c_raise(self):
        ok, d, ms, raw = self.py("raise Exception('bench')")
        good = (not ok) and isinstance(d, dict) and "Traceback" in d.get("stderr", "") \
            and d.get("rolled_back") is True and d.get("error") is None
        return (PASS if good else FAIL), f"stderr tail={d.get('stderr', '')[-80:]!r}" if isinstance(d, dict) else raw, raw

    def c_stderr_is_failure(self):
        ok, d, ms, raw = self.py("import sys; sys.stderr.write('warn only\\n'); print('still printed')")
        good = (not ok) and isinstance(d, dict) and d.get("rolled_back") is True \
            and d.get("stdout", "").strip() == "still printed"
        return (PASS if good else FAIL), "stderr output alone fails the call and rolls back; stdout still returned", raw

    def c_rollback(self):
        before = self.count_all()
        ok, d, ms, raw = self.py("import vs; vs.Rect(0,0,50,50); raise Exception('rollback')")
        after = self.count_all()
        good = (not ok) and isinstance(d, dict) and d.get("rolled_back") is True and before == after
        return (PASS if good else FAIL), f"count {before} -> {after}", raw

    def c_no_transaction_persists(self):
        before = self.count_all()
        ok, d, ms, raw = self.py("import vs; vs.Rect(0,0,60,60); raise Exception('keep')", transaction=False)
        after = self.count_all()
        good = (not ok) and isinstance(d, dict) and d.get("rolled_back") is False and after == before + 1
        return (PASS if good else FAIL), f"transaction=false: object kept on failure, count {before} -> {after}", raw

    def c_undo_per_call(self):
        before = self.count_all()
        for i in range(3):
            ok, d, ms, raw = self.py(f"import vs; vs.Rect({i*20},100,{i*20+5},105)", undo_label=f"bench step {i+1}")
            if not ok:
                return FAIL, f"call {i+1} failed: {raw[:200]}", raw
        mid = self.count_all()
        self.undo_once()
        after = self.count_all()
        good = mid == before + 3 and after == mid - 1
        return (PASS if good else FAIL), f"count {before} -> {mid} after 3 calls -> {after} after one Undo", None

    def c_undo_label(self):
        label = "L" * 200
        ok, d, ms, raw = self.py("print(1)", undo_label=label)
        got = d.get("undo_label", "") if isinstance(d, dict) else ""
        good = ok and len(got) == 120
        return (PASS if good else FAIL), f"200-char label echoed as {len(got)} chars", raw

    def c_vectorscript(self):
        before = self.count_all()
        ok, d, ms, raw = self.call("run_script", {
            "script": "PROCEDURE M; BEGIN Rect(0,0,10,10); END; RUN(M);", "language": "vectorscript"})
        after = self.count_all()
        good = ok and after == before + 1
        note = f"count {before} -> {after}; stdout={d.get('stdout') if isinstance(d, dict) else None!r}"
        return (PASS if good else FAIL), note, raw

    def c_vectorscript_error(self):
        ok, d, ms, raw = self.call("run_script", {
            "script": "PROCEDURE M; BEGIN NoSuchProc(1); END; RUN(M);", "language": "vectorscript"})
        code = d.get("error_code") if isinstance(d, dict) else None
        return INFO, f"success={ok} error_code={code} stderr={d.get('stderr', '')[:80] if isinstance(d, dict) else ''!r}", raw

    def c_newfield(self):
        ok, d, ms, raw = self.py(f"import vs; print(vs.NewField('{RECORD}','f1','x',4,0))")
        return (PASS if ok else FAIL), f"NewField -> {d.get('stdout', '').strip() if isinstance(d, dict) else raw}", raw

    def c_buildresourcelist(self):
        out = self.stdout(
            "import vs\nlid, n = vs.BuildResourceList(47, 0, '')\n"
            "print([vs.GetNameFromResourceList(lid, i) for i in range(1, n + 1)])\n", transaction=False)
        return (PASS if RECORD in out else FAIL), out.strip()[:120], out

    def c_large_stdout(self):
        n = 200_000
        ok, d, ms, raw = self.py(f"print('x' * {n})", transaction=False)
        got = len(d.get("stdout", "").rstrip("\r\n")) if isinstance(d, dict) else 0
        return (PASS if ok and got == n else FAIL), f"{n} chars requested, {got} returned in {ms:.0f} ms", None

    def c_large_script(self):
        n = 250_000
        script = "# " + ("y" * n) + "\nprint('ok')\n"
        ok, d, ms, raw = self.py(script, transaction=False)
        good = ok and isinstance(d, dict) and d.get("stdout", "").strip() == "ok"
        return (PASS if good else FAIL), f"{len(script)} byte script in {ms:.0f} ms", raw[:300]

    def c_fresh_context(self):
        self.stdout("x_bench = 1", transaction=False)
        out = self.stdout("print(globals().get('x_bench'))", transaction=False).strip()
        return (PASS if out == "None" else FAIL), f"global set in call 1 seen in call 2 as {out!r}", out

    def c_python_version(self):
        out = self.stdout("import sys; print(sys.version.split()[0])", transaction=False).strip()
        return INFO, f"Python {out} inside Vectorworks", out

    def c_sleep_3s(self):
        ok, d, ms, raw = self.py("import time; time.sleep(3); print('done')", transaction=False)
        good = ok and 2900 <= ms <= 6000
        return (PASS if good else FAIL), f"{ms:.0f} ms wall for a 3 s script", raw[:200]

    def c_ceiling_27s(self):
        ok, d, ms, raw = self.py("import time; time.sleep(27); print('late')", transaction=False)
        timed_out = (not ok) and "timed out after 25s" in raw
        t0 = time.perf_counter()
        ok2, d2, ms2, raw2 = self.py("print('alive')", transaction=False)
        recovered = ok2 and isinstance(d2, dict) and d2.get("stdout", "").strip() == "alive"
        note = (f"27 s script: timeout response after {ms:.0f} ms (expected ~25 s); "
                f"next call succeeded={recovered} after {ms2:.0f} ms (queued behind the sleeping script)")
        return (PASS if timed_out and recovered else FAIL), note, {"first": raw[:300], "second": raw2[:300]}

    # ------------------------------------------------------------------
    # L2: templated Python tools
    # ------------------------------------------------------------------
    def c_vs_signature_exact(self):
        ok, d, ms, raw = self.call("vs_signature", {"name": "NewField"})
        good = ok and isinstance(d, dict) and d.get("name") == "NewField" and "python" in d and "arity" in d
        return (PASS if good else FAIL), d.get("python", raw) if isinstance(d, dict) else raw, raw

    def c_vs_signature_prefix(self):
        ok, d, ms, raw = self.call("vs_signature", {"name": "GetRF"})
        good = ok and isinstance(d, dict) and d.get("exact") is False and len(d.get("matches", [])) >= 1
        return (PASS if good else FAIL), f"{len(d.get('matches', [])) if isinstance(d, dict) else 0} candidates", raw

    def c_vs_signature_unknown(self):
        ok, d, ms, raw = self.call("vs_signature", {"name": "DefinitelyNotAFunctionXYZ"})
        return (PASS if isinstance(d, str) and d.startswith("No vs.* function") else FAIL), raw[:80], raw

    def c_list_classes(self):
        ok, d, ms, raw = self.call("list_classes")
        if not (ok and isinstance(d, dict)):
            return FAIL, raw[:200], raw
        names = [c["name"] for c in d.get("classes", [])]
        good = "None" in names and d.get("count") == len(names)
        return (PASS if good else FAIL), f"{len(names)} classes", raw[:400]

    def c_list_record_formats(self):
        ok, d, ms, raw = self.call("list_record_formats", {})
        names = [f["name"] for f in d.get("formats", [])] if ok and isinstance(d, dict) else []
        good = ok and RECORD in names and all(not n.startswith("__") for n in names)
        return (PASS if good else FAIL), f"{len(names)} visible formats: {names[:6]}", raw[:400]

    def c_list_record_formats_hidden(self):
        ok1, d1, _, _ = self.call("list_record_formats", {})
        ok2, d2, _, raw = self.call("list_record_formats", {"include_hidden": True})
        n1 = d1.get("count", -1) if isinstance(d1, dict) else -1
        n2 = d2.get("count", -1) if isinstance(d2, dict) else -1
        return (PASS if ok1 and ok2 and n2 >= n1 >= 0 else FAIL), f"{n1} visible, {n2} with hidden", raw[:300]

    def c_attach_record(self):
        script = (
            "import vs\n"
            f"vs.NewField('{RECORD}','f1','x',4,0)\n"
            "vs.Rect(300,300,310,310)\n"
            "h = vs.LNewObj()\n"
            f"vs.SetRecord(h, '{RECORD}')\n"
            f"vs.SetRField(h, '{RECORD}', 'f1', 'hello')\n"
            f"print(vs.GetRField(h, '{RECORD}', 'f1'))\n"
        )
        ok, d, ms, raw = self.py(script, undo_label="bench attach record")
        got = d.get("stdout", "").strip() if isinstance(d, dict) else ""
        return (PASS if ok and got == "hello" else FAIL), f"GetRField after SetRField -> {got!r}", raw[:300]

    def c_read_records(self):
        ok, d, ms, raw = self.call("read_records_by_format", {"format": RECORD, "limit": 50})
        if not (ok and isinstance(d, dict)):
            return FAIL, raw[:200], raw
        rows = d.get("rows", [])
        hit = any(r.get("values", {}).get("f1") == "hello" for r in rows)
        good = d.get("fields") == ["f1"] and hit
        return (PASS if good else FAIL), f"{len(rows)} rows, hello found={hit}", raw[:400]

    def c_read_records_layer_filter(self):
        layer = self.stdout("import vs; print(vs.GetLName(vs.ActLayer()))", transaction=False).strip()
        ok1, d1, _, _ = self.call("read_records_by_format", {"format": RECORD, "layer": layer})
        ok2, d2, _, raw = self.call("read_records_by_format", {"format": RECORD, "layer": "No Such Layer"})
        n1 = d1.get("count", -1) if isinstance(d1, dict) else -1
        n2 = d2.get("count", -1) if isinstance(d2, dict) else -1
        return (PASS if ok1 and ok2 and n1 >= 1 and n2 == 0 else FAIL), f"layer {layer!r}: {n1} rows; bogus layer: {n2}", raw[:200]

    def c_read_records_limit(self):
        ok, d, ms, raw = self.call("read_records_by_format", {"format": RECORD, "limit": 0})
        good = ok and isinstance(d, dict) and d.get("count") == 0 and d.get("truncated") is True
        return (PASS if good else FAIL), f"limit=0 -> count={d.get('count') if isinstance(d, dict) else None} truncated={d.get('truncated') if isinstance(d, dict) else None}", raw[:200]

    def c_read_records_unknown(self):
        ok, d, ms, raw = self.call("read_records_by_format", {"format": "No Such Format"})
        good = ok and isinstance(d, dict) and "error" in d
        return (PASS if good else FAIL), "unknown format returns a JSON object with an 'error' key, not an MCP error", raw[:200]

    def c_export_ifc(self, path: str):
        def fn():
            if os.path.exists(path):
                os.remove(path)
            ok, d, ms, raw = self.call("export_ifc", {"path": path})
            exists = os.path.exists(path)
            if ok and isinstance(d, dict) and d.get("ok") and exists:
                return PASS, f"wrote {os.path.getsize(path)} bytes in {ms:.0f} ms", raw
            return UNVERIFIED, f"IFC_ExportNoUI ok={d.get('ok') if isinstance(d, dict) else None}, file exists={exists}", raw[:300]
        return fn

    # ------------------------------------------------------------------
    # Latency samples
    # ------------------------------------------------------------------
    def _sample(self, tool: str, args: dict, n: int):
        samples = []
        for _ in range(n):
            ok, d, ms, raw = self.call(tool, args)
            if not ok:
                raise RuntimeError(f"{tool} failed during sampling: {raw[:200]}")
            samples.append(ms)
        return stats(samples)

    def b_tool(self, tool: str, args: dict):
        def fn():
            s = self._sample(tool, args, self.repeat)
            return INFO, f"median {s['median_ms']} ms, p95 {s['p95_ms']} ms, min {s['min_ms']} ms (n={s['n']})", s
        return fn

    def b_create_100_rects(self):
        script = "import vs\nfor i in range(100):\n    vs.Rect(i*2, 200, i*2+1, 201)\n"
        before = self.count_all()
        ok, d, ms, raw = self.py(script, undo_label="bench 100 rects")
        mid = self.count_all()
        self.undo_once()
        after = self.count_all()
        good = ok and mid == before + 100 and after == before
        return (PASS if good else FAIL), f"100 objects in one call: {ms:.0f} ms ({ms/100:.1f} ms/object); one Undo removed all 100", {"ms": ms}

    def b_create_rect_sequential(self):
        samples = []
        for i in range(self.repeat):
            ok, d, ms, raw = self.call("create_rect", {"x": i * 3, "y": 400, "width": 1, "height": 1})
            if not ok:
                raise RuntimeError(raw[:200])
            samples.append(ms)
        s = stats(samples)
        return INFO, f"median {s['median_ms']} ms per L1 create_rect (n={s['n']})", s

    # ------------------------------------------------------------------
    def run(self, args):
        doc = self.doc_name()
        print(f"document: {doc!r}  main_pid={self.main_pid}  mutation={'on' if self.allow_mutation else 'OFF'}")

        # L1
        self.case("L1.get_drawing_info", "L1", self.c_get_drawing_info)
        self.case("L1.list_layers", "L1", self.c_list_layers)
        self.case("L1.create_line", "L1", self._create_primitive("create_line", {"x1": 0, "y1": 0, "x2": 10, "y2": 10}), mutates=True)
        self.case("L1.create_rect", "L1", self._create_primitive("create_rect", {"x": 0, "y": 0, "width": 10, "height": 10}), mutates=True)
        self.case("L1.create_circle", "L1", self._create_primitive("create_circle", {"cx": 0, "cy": 0, "radius": 5}), mutates=True)
        self.case("L1.create_is_undo_step", "L1", self.c_l1_undoable, mutates=True, scored=False,
                  caveat="L1 create_* handlers call gSDK directly with no undo bracket.")

        # L0
        self.case("L0.print_stdout", "L0", self.c_print)
        self.case("L0.stdout_json_roundtrip", "L0", self.c_stdout_json)
        self.case("L0.unicode_roundtrip", "L0", self.c_unicode)
        self.case("L0.raise_gives_traceback", "L0", self.c_raise)
        self.case("L0.stderr_output_is_failure", "L0", self.c_stderr_is_failure,
                  caveat="Any stderr text fails the call and rolls it back; warnings count. Print results to stdout only.")
        self.case("L0.mutate_then_raise_rolls_back", "L0", self.c_rollback, mutates=True)
        self.case("L0.transaction_false_keeps_changes", "L0", self.c_no_transaction_persists, mutates=True,
                  caveat="transaction=false: a failed script leaves partial changes in the document.")
        self.case("L0.one_undo_step_per_call", "L0", self.c_undo_per_call, mutates=True)
        self.case("L0.undo_label_truncated_120", "L0", self.c_undo_label)
        self.case("L0.vectorscript_runs", "L0", self.c_vectorscript, mutates=True,
                  caveat="VectorScript has no stdout capture; write results to a file or use Python.")
        self.case("L0.vectorscript_error", "L0", self.c_vectorscript_error, scored=False)
        self.case("L0.newfield_creates_format", "L0", self.c_newfield, mutates=True)
        self.case("L0.buildresourcelist_47", "L0", self.c_buildresourcelist)
        self.case("L0.large_stdout_200k", "L0", self.c_large_stdout)
        self.case("L0.large_script_250k", "L0", self.c_large_script)
        self.case("L0.fresh_context_per_call", "L0", self.c_fresh_context,
                  caveat="Globals do not persist between calls; each run_script is a new interpreter context.")
        self.case("L0.python_version", "L0", self.c_python_version, scored=False)
        self.case("L0.sleep_3s", "L0", self.c_sleep_3s)
        if args.slow:
            self.case("L0.ceiling_25s", "L0", self.c_ceiling_27s,
                      caveat="Per-call ceiling: 25 s in the plugin dispatcher, 30 s in the server. The script keeps running inside VW after the timeout response.")
        else:
            self.rows.append({"id": "L0.ceiling_25s", "level": "L0", "mutates": False, "status": SKIP,
                              "note": "run with --slow", "ms": 0, "scored": False, "vw_alive": True, "detail": None,
                              "caveat": "Per-call ceiling: 25 s in the plugin dispatcher, 30 s in the server."})
        self.rows.append({"id": "L0.engine_poisoned", "level": "L0", "mutates": False, "status": SKIP,
                          "note": "needs a VW restart afterwards; see scripts/gantry_ladder.py --py-mode execute",
                          "ms": 0, "scored": False, "vw_alive": True, "detail": None,
                          "caveat": "After a contained fault every run_script returns error=engine_poisoned until VW restarts."})

        # L2
        self.case("L2.vs_signature_exact", "L2", self.c_vs_signature_exact)
        self.case("L2.vs_signature_prefix", "L2", self.c_vs_signature_prefix)
        self.case("L2.vs_signature_unknown", "L2", self.c_vs_signature_unknown)
        self.case("L2.list_classes", "L2", self.c_list_classes)
        self.case("L2.list_record_formats", "L2", self.c_list_record_formats)
        self.case("L2.list_record_formats_hidden", "L2", self.c_list_record_formats_hidden)
        self.case("L2.attach_record_via_run_script", "L0", self.c_attach_record, mutates=True)
        self.case("L2.read_records_by_format", "L2", self.c_read_records,
                  caveat="Field values come back as strings (vs.GetRField).")
        self.case("L2.read_records_layer_filter", "L2", self.c_read_records_layer_filter)
        self.case("L2.read_records_limit", "L2", self.c_read_records_limit)
        self.case("L2.read_records_unknown_format", "L2", self.c_read_records_unknown,
                  caveat="L2 structured errors are JSON objects with an 'error' key, returned as a normal result.")
        if args.ifc:
            self.case("L2.export_ifc", "L2", self.c_export_ifc(os.path.expandvars(args.ifc)), scored=False,
                      caveat="Unverified: IFC_ExportNoUI returned false on the blank scratch document.")
        else:
            self.rows.append({"id": "L2.export_ifc", "level": "L2", "mutates": False, "status": SKIP,
                              "note": "run with --ifc PATH", "ms": 0, "scored": False, "vw_alive": True, "detail": None,
                              "caveat": "Unverified: IFC_ExportNoUI returned false on the blank scratch document."})

        # Latency
        self.case("bench.vs_signature_offline", "bench", self.b_tool("vs_signature", {"name": "Rect"}), scored=False,
                  caveat="Server-only call: the floor for stdio + JSON-RPC overhead.")
        self.case("bench.L1.get_drawing_info", "bench", self.b_tool("get_drawing_info", {}), scored=False)
        self.case("bench.L0.run_script_print", "bench", self.b_tool("run_script", {"script": "print(1)", "language": "python", "transaction": False}), scored=False)
        self.case("bench.L0.run_script_print_tx", "bench", self.b_tool("run_script", {"script": "print(1)", "language": "python"}), scored=False,
                  caveat="transaction=true adds an undo event per call.")
        self.case("bench.L2.list_classes", "bench", self.b_tool("list_classes", {}), scored=False)
        self.case("bench.L1.create_rect_sequential", "bench", self.b_create_rect_sequential, mutates=True, scored=False)
        self.case("bench.L0.create_100_rects_one_call", "bench", self.b_create_100_rects, mutates=True,
                  caveat="Batch mutations in one script: per-object cost is far below per-call cost.")

        return doc


# ---------------------------------------------------------------- reporting

def matrix_markdown(meta: dict, rows: list[dict]) -> str:
    scored = [r for r in rows if r.get("scored")]
    passed = sum(1 for r in scored if r["status"] == PASS)
    failed = [r["id"] for r in scored if r["status"] == FAIL]
    lines = [
        "# Capability matrix (generated)",
        "",
        f"Generated by `scripts/capability_bench.py` on {meta['date']}. Do not edit by hand; rerun the script.",
        "",
        "| | |",
        "|---|---|",
        f"| Vectorworks | {meta['vw_exe_version']} (`Vectorworks2026.exe` file version) |",
        f"| Repo commit | `{meta['git_head']}` |",
        f"| Document | `{meta['document']}` |",
        f"| Mutation cases | {'run' if meta['mutation'] else 'skipped'} |",
        f"| Scored cases | {passed}/{len(scored)} passed{'' if not failed else ' (FAILED: ' + ', '.join(failed) + ')'} |",
        f"| Latency sample size | {meta['repeat']} |",
        "",
        "Levels: L1 = native C++ handler, L0 = `run_script`, L2 = templated Python tool over `run_script`.",
        "Status: PASS/FAIL are scored checks; INFO records a measurement; SKIP was not run; UNVERIFIED needs a real model.",
        "",
        "| Capability | Level | Status | ms | Result | Caveat |",
        "|---|---|---|---|---|---|",
    ]
    for r in rows:
        note = str(r["note"]).replace("|", "\\|").replace("\n", " ")
        cav = str(r.get("caveat", "")).replace("|", "\\|")
        lines.append(f"| `{r['id']}` | {r['level']} | {r['status']} | {r['ms']} | {note} | {cav} |")
    bench = [r for r in rows if r["level"] == "bench" and isinstance(r.get("detail"), dict) and "median_ms" in r["detail"]]
    if bench:
        lines += ["", "## Latency (ms per call, through stdio + server + TCP + plugin)", "",
                  "| Call | n | min | median | p95 | max |", "|---|---|---|---|---|---|"]
        for r in bench:
            s = r["detail"]
            lines.append(f"| `{r['id']}` | {s['n']} | {s['min_ms']} | {s['median_ms']} | {s['p95_ms']} | {s['max_ms']} |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vw-version", default="2026")
    ap.add_argument("--repeat", type=int, default=20, help="latency sample size")
    ap.add_argument("--read-only", action="store_true", help="skip every mutation case")
    ap.add_argument("--slow", action="store_true", help="include the 25 s ceiling probe")
    ap.add_argument("--ifc", default=None, help="also try export_ifc to this path")
    ap.add_argument("--allow-doc", action="append", default=[], help="document name (vs.GetFName) allowed for mutation")
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "docs", "test-runs"))
    ap.add_argument("--matrix", default=os.path.join(ROOT, "docs", "capabilities-matrix.md"))
    ap.add_argument("--no-matrix", action="store_true", help="do not rewrite docs/capabilities-matrix.md")
    args = ap.parse_args()

    mcp = Mcp(args.vw_version)
    try:
        mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                   "clientInfo": {"name": "capability_bench", "version": "0"}})
        mcp.notify("notifications/initialized")
        tools = [t["name"] for t in mcp.request("tools/list").get("result", {}).get("tools", [])]
        print("tools advertised:", ", ".join(tools))

        probe = Bench(mcp, args.repeat, allow_mutation=False)
        doc = probe.doc_name()
        disposable = doc.lower().startswith("gantry-test") or doc in args.allow_doc
        allow = disposable and not args.read_only
        if not disposable and not args.read_only:
            print(f"!! document {doc!r} is not a disposable scratch file: mutation cases will be SKIPPED "
                  f"(pass --allow-doc {doc!r} to override)")

        bench = Bench(mcp, args.repeat, allow_mutation=allow)
        bench.run(args)
    finally:
        mcp.close()

    stamp = dt.datetime.now().strftime("%Y-%m-%d-%H%M%S")
    meta = {"date": stamp, "vw_exe_version": vw_exe_version(), "git_head": git_head(), "document": doc,
            "mutation": allow, "repeat": args.repeat, "tools": tools, "main_pid": bench.main_pid,
            "server": os.path.relpath(sys.modules["mcp_smoke"].SERVER, ROOT)}
    scored = [r for r in bench.rows if r.get("scored")]
    failed = [r for r in scored if r["status"] == FAIL]

    os.makedirs(args.out_dir, exist_ok=True)
    rec = os.path.join(args.out_dir, f"bench-{stamp}.json")
    with open(rec, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"meta": meta, "all_passed": not failed, "rows": bench.rows}, f, indent=2, default=str)
    if not args.no_matrix:
        with open(args.matrix, "w", encoding="utf-8", newline="\n") as f:
            f.write(matrix_markdown(meta, bench.rows))

    print()
    print(f"scored: {len(scored) - len(failed)}/{len(scored)} passed"
          + ("" if not failed else "  FAILED: " + ", ".join(r["id"] for r in failed)))
    print(f"record: {rec}")
    if not args.no_matrix:
        print(f"matrix: {args.matrix}")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
