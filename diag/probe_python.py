"""VW2026.5 Diagnostic Probe — Python via vs.*

Tests the IPythonScriptEngine path WITHOUT going through our plugin or
the MCP server. Use this to discriminate:

  - VectorScript probe works, this crashes      -> IPythonScriptEngine regression in VW2026.5
  - Both work                                   -> Python layer is fine; failure is in our plugin
  - Both crash                                  -> Regression is in VW core, not scripting layers

Run via VW: Tools -> Plug-Ins -> Run Script... -> pick this file.

Output: ~/.vwmcp-diag/python-probe-<timestamp>.json
  Windows: %USERPROFILE%\.vwmcp-diag\
  Mac:     $HOME/.vwmcp-diag/

Every probe step is wrapped in try/except so one failing call doesn't
abort the whole report. The errors list at the bottom is the diagnostic
payload — empty means all probes passed.
"""

import json
import os
import sys
import time
import traceback


REPORT_DIR = os.path.expanduser("~/.vwmcp-diag")
os.makedirs(REPORT_DIR, exist_ok=True)

report = {
    "probe": "python",
    "timestamp": int(time.time()),
    "py_version": sys.version,
    "py_platform": sys.platform,
    "py_executable": sys.executable,
    "errors": [],
    "results": {},
}


def step(name, fn):
    try:
        report["results"][name] = fn()
    except BaseException as e:
        report["results"][name] = None
        report["errors"].append({
            "step": name,
            "type": type(e).__name__,
            "message": str(e),
            "traceback": traceback.format_exc(),
        })


# Import vs.* first — if THIS fails, the Python engine itself is broken.
try:
    import vs  # type: ignore[import]
    report["vs_module_loaded"] = True
except BaseException as e:
    report["vs_module_loaded"] = False
    report["errors"].append({
        "step": "import vs",
        "type": type(e).__name__,
        "message": str(e),
        "traceback": traceback.format_exc(),
    })


if report.get("vs_module_loaded"):
    # Version probes — wrap each separately so we learn which signatures changed.
    step("vw_version_str", lambda: vs.GetVWVersion())
    step("vw_version_tuple", lambda: vs.GetVersion())
    step("vw_version_ex", lambda: vs.GetVersionEx())

    # Document state
    step("doc_path", lambda: vs.GetFPathName())
    step("doc_name", lambda: vs.GetFName())
    step("has_document", lambda: bool(vs.ActLayer()))

    # Counts
    step("class_count", lambda: vs.ClassNum())

    def _count_layers():
        n = 0
        h = vs.FActLayer()
        while h:
            n += 1
            h = vs.NextLayer(h)
        return n
    step("layer_count", _count_layers)

    # IFC scheme — U5-suspicious, since Morpholio Trace integration touched export paths
    step("ifc_scheme", lambda: vs.IFCGetIFCScheme())

    # Try touching the Python script engine indirectly via a no-op vs call.
    # If this raises, the engine bindings themselves are mis-mapped.
    step("vs_message_smoke", lambda: (vs.Message("vwmcp probe OK"), "ok")[1])


# Write report
out_path = os.path.join(REPORT_DIR, f"python-probe-{report['timestamp']}.json")
try:
    with open(out_path, "w") as f:
        json.dump(report, f, indent=2, default=str)
    write_ok = True
except BaseException as e:
    write_ok = False
    report["errors"].append({
        "step": "write_report",
        "type": type(e).__name__,
        "message": str(e),
    })


# Surface findings to the VW UI
err_count = len(report["errors"])
summary = f"Python probe: {err_count} errors. Report: {out_path}"

if report.get("vs_module_loaded"):
    try:
        vs.AlrtDialog(summary)
    except BaseException:
        pass

print(summary)
print(json.dumps(report, indent=2, default=str))
