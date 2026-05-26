# vectorworks-mcp — Tool Coverage & Abstraction Spec

**Status:** draft, 2026-05-22
**Branch:** `claude/lattice-platform-mcp-TM9xi`
**Purpose:** decide what `vs.*` surface to expose as MCP tools, at what abstraction level, and how to keep iteration cheap without regressing the C++ plumbing.

## 1. Why this spec exists

Today the server exposes 6 tools: `get_drawing_info`, `list_layers`, `create_line`, `create_rect`, `create_circle`, `run_script`. The first five are thin: a typed schema in `server.rs` and a matching handler in `BridgeCore`. `run_script` is the escape hatch — full `vs.*` reach, untyped.

After surveying `JeromyJSmith/lattice-platform`'s `vw-python/mcp_bridge.py` (a 230-line FastMCP server that drives VW2026 over AppleScript with `vs.*` script bodies inlined per tool), three questions surfaced:

1. **Is the current C++ stack actually more robust, or am I cargo-culting?** (Section 2.)
2. **What VW capabilities should be MCP tools, vs. left to `run_script`?** (Section 3.)
3. **At what abstraction layer should new tools live — typed C++ handlers, or templated Python bodies the Rust server interpolates?** (Section 4.)

## 2. Robustness assessment (honest)

The C++ stack is more robust **as plumbing**. Lattice's approach is more iterable **as a tool surface**. These are different axes.

### Where the C++ stack wins (clearly)

| Concern | C++ stack | lattice `mcp_bridge.py` |
|---|---|---|
| Per-call latency | ~ms (in-process VCOM) | 100–500ms (AppleScript launch + temp-file IO per call) |
| stdout/stderr capture | Real, via `IPythonScriptEngine` + `CDefaultPythonLogger` | None — relies on `vs.Message` and JSON temp files |
| Error surfacing | Structured `{success, data, stdout, stderr, error_code}` | Best-effort; AppleScript swallows tracebacks |
| Platform | Windows today, Mac SDK path open | Mac-only (AppleScript) |
| Connection model | Persistent TCP, per-VW-version port (27200 + short tag) | One `osascript` subprocess per call |
| Defensive guards | `gSDK->GetDrawingHeader()` null-check before `run_script`; deferred auto-start to dodge SEH crash on VW init | None — `_ensure_document` paper-overs via GUI keystrokes |
| Lifecycle | Web Palette with status dot, start/stop, persistent auto-start config in APPDATA | N/A — runs as a separate MCP process |
| Concurrent VW versions | Yes (port-per-version) | No |

These are not theoretical — most were earned by debugging real failure modes (singleton state-wipe, IPythonScriptEngine init-order SEH, no-document crash). They would re-bite anyone re-implementing on a Python/AppleScript transport.

### Where lattice's approach wins

| Concern | C++ stack | lattice `mcp_bridge.py` |
|---|---|---|
| Iteration speed to add a tool | Edit `server.rs` + `BridgeCore.cpp`, rebuild Rust, rebuild C++ plugin, kill VW, copy `.vlb`, relaunch VW, drag menu | Edit one `.py`, save, restart MCP server (5s) |
| Tool body readability | C++ string-builds VW SDK calls | Real Python with `vs.*`, copy-pasteable into VW's Run Script dialog |
| Distribution | Two binaries + SDK + workspace setup | One `.py` |
| Onboarding cost for a new tool author | High (C++ SDK, MSBuild, VCOM macros) | Low (write Python that works in Run Script, decorate with `@mcp.tool()`) |

**Conclusion:** the C++ plumbing is correct. The flat "every new tool = a new C++ handler" authoring model is the part worth questioning.

## 3. Coverage gap analysis

`vs.*` is large (~thousands of functions). The bar for a dedicated MCP tool should be: **frequently called, benefits from typed input/output, or unlocks a workflow that's painful to express in `run_script`.** Everything else lives behind `run_script`.

Grouped by AEC/landscape/BIM relevance (the user's actual workflow per `FORK_NOTES.md`):

### A. Document & navigation (read)
| Tool | Today | Priority | Notes |
|---|---|---|---|
| `get_drawing_info` | ✅ | — | Consider adding: `ifc_scheme` (`IFC_GetIFCScheme`), `vw_version` (`GetVWVersion`), `class_count` (`ClassNum`), `unit_system` |
| `list_layers` | ✅ | — | Returns name + visibility today |
| `list_classes` | ❌ | High | Mirrors `list_layers`; iterate `ClassList` / `ClassNum` |
| `get_active_layer` / `get_active_class` | ❌ | Med | `vs.ActLayer()`, `vs.ActiveClass()` |
| `list_saved_views` | ❌ | Med | Useful for REMOTE_PY-style triggers |
| `list_design_files` / referenced files | ❌ | Low | |

### B. Object query (read)
| Tool | Today | Priority | Source |
|---|---|---|---|
| `list_objects_on_layer(layer)` | ❌ | High | `mcp_bridge.py` has it; entity_type + name + num_records |
| `get_object_bounds(layer, name)` | ❌ | High | `vs.GetBBox`, returns min/max XY (and Z for 3D) |
| `get_object_info(handle_or_name)` | ❌ | Med | Entity type, class, layer, attributes |
| `get_selection` | ❌ | Med | Iterate via `vs.FSActLayer`-style selection cursors |
| `find_objects_by_class(class_name)` | ❌ | Med | Cross-layer query |
| `count_objects_by_type(layer)` | ❌ | Low | Histogram for sanity-checking imports |

### C. Records / parametric data (read)
This is the unique-value area for landscape/BIM Claude workflows. Records are how VW stores per-object structured data.

| Tool | Today | Priority | Source |
|---|---|---|---|
| `list_record_formats` | ❌ | High | Doc-level discovery |
| `get_record_data(layer, name, record)` | ❌ | High | `mcp_bridge.py` has it |
| `read_plant_records` | ❌ | High | `vw-python/examples/read_plant_records.py` — Plant Style instances → species, coords |
| `read_records_by_format(format_name)` | ❌ | High | Generalized version; iterates document for all attached instances |
| `set_record_field(handle, record, field, value)` | ❌ | Med | Write side — careful, destructive |

### D. Geometry creation (write)
| Tool | Today | Priority | Notes |
|---|---|---|---|
| `create_line` | ✅ | — | |
| `create_rect` | ✅ | — | |
| `create_circle` | ✅ | — | |
| `create_polyline` | ❌ | Med | Array of vertices |
| `create_polygon` | ❌ | Med | Closed polyline |
| `create_text(x, y, text, [style])` | ❌ | Med | High-utility annotation |
| `create_symbol_instance(name, x, y, [rotation])` | ❌ | High | Place library symbols — primary VW workflow |
| `create_locus(x, y, [z])` | ❌ | Low | Useful for landscape pin points |

### E. Edits (write — destructive, gate carefully)
| Tool | Today | Priority | Notes |
|---|---|---|---|
| `set_layer_visibility(layer, visible)` | ❌ | Med | `vs.SetLVis` |
| `set_active_layer(layer)` | ❌ | Low | |
| `create_layer(name, [class])` | ❌ | Med | |
| `delete_object(handle)` | ❌ | Low | Probably leave to `run_script` |

### F. Exports
| Tool | Today | Priority | Notes |
|---|---|---|---|
| `export_ifc([path])` | ❌ | **Highest** | `vw-python/examples/export_ifc_trigger.py` — IFC 4.3, no UI prompts |
| `export_dwg([path])` | ❌ | Med | Common interop need |
| `export_pdf([path], [layers])` | ❌ | Med | |
| `export_image([path], [view])` | ❌ | Low | Snapshots / thumbnails |

### G. Worksheets & reports
| Tool | Today | Priority | Notes |
|---|---|---|---|
| `list_worksheets` | ❌ | Low | |
| `get_worksheet_data(name)` | ❌ | Med | Quantity takeoff workflows |
| `recalc_worksheet(name)` | ❌ | Low | |

### H. Script execution (escape hatch)
| `run_script` | ✅ | — | Untouched. The only intentionally-untyped tool. |

### I. Selection & geometry math (read-mostly, low priority for now)
| `get_centroid(handle)` / `get_area(handle)` / `get_length(handle)` | ❌ | Low | All trivially expressible in `run_script` |

**Cuts (explicitly *not* tools):** anything tightly coupled to GUI state (active tool, mouse position), anything modal (dialogs, file prompts), anything best expressed as a multi-step transaction.

## 4. Abstraction layers — the actual design question

Three viable layers for a tool to live in:

### L0 — `run_script` (escape hatch)
- **What:** raw Python or VectorScript, executed plugin-side via `IPythonScriptEngine`.
- **Pros:** zero new code per capability; full `vs.*` surface.
- **Cons:** untyped input/output; Claude has to remember `vs.*` names; brittle to author.
- **Use for:** ad-hoc exploration, one-shots, prototypes, anything that doesn't earn its way into L1 or L2.

### L1 — Typed native tool (current path for the 6 baked-in tools)
- **What:** Rust schema in `server.rs` + a C++ handler in `BridgeCore`, dispatched via the TCP `method` field.
- **Pros:** type-safe input via `schemars`; structured output; lowest per-call cost; first-class Claude UX.
- **Cons:** adding one is two-language touch + plugin rebuild + VW restart. Real friction.
- **Use for:** stable, high-value primitives only (the 6 we have, plus maybe `create_text`, `create_symbol_instance`).

### L2 — Templated Python tool (proposed new layer)
- **What:** tool definition lives entirely in the Rust server as `{name, schema, python_body_template, response_shape}`. The server interpolates parameters into the template, ships the script through the existing `run_script` plumbing, and parses the result.
- **Pros:** adding a tool = editing one `.rs` file (or one `.toml`/`.json` if we externalize). No C++ rebuild. No VW restart. The tool body is real, readable VW Python. Reuses every bit of robustness work already done in the C++ run_script handler (logger, stdout capture, no-doc guard).
- **Cons:** parameters are templated as strings (mitigation: serialize all params as JSON, parse inside the script — same pattern lattice uses); one extra IPC round-trip vs. a native handler (negligible — we already pay it for run_script).
- **Use for:** everything in Section 3 that isn't L1.

**Recommendation:** adopt L2 as the default for new tools. L1 remains for the existing 6 + any future tool that proves it needs native speed or stricter typing. L0 (`run_script`) stays as-is.

### Sketch of L2 mechanics

```rust
// server.rs
struct PyTool {
    name: &'static str,
    description: &'static str,
    schema: serde_json::Value,        // JSON Schema for inputs
    body: &'static str,               // Python source with {{params}} marker
    response_kind: ResponseKind,      // JsonObject | JsonArray | Scalar | Raw
}

const TOOLS: &[PyTool] = &[
    PyTool {
        name: "list_objects_on_layer",
        description: "List every object on a named design layer.",
        schema: schema!({ "layer": { "type": "string" } }),
        body: include_str!("../tools/list_objects_on_layer.py"),
        response_kind: ResponseKind::JsonArray,
    },
    // ...
];
```

```python
# tools/list_objects_on_layer.py
import vs, json
params = json.loads(__PARAMS__)
target = None
lyr = vs.FActLayer()
while lyr:
    if vs.GetLName(lyr) == params["layer"]:
        target = lyr
        break
    lyr = vs.NextLayer(lyr)
out = []
if target:
    h = vs.FSObject(target)
    while h:
        out.append({
            "entity_type": vs.GetEntityType(h),
            "name": vs.GetName(h),
            "num_records": vs.NumRecords(h),
        })
        h = vs.NextSObj(h)
print(json.dumps(out))
```

The Rust server substitutes `__PARAMS__` with `json!(params).to_string()`, ships the resulting body through the existing run_script path, captures stdout, parses as JSON. Done.

### L3 (deferred) — Declarative rule-driven tools
lattice's `placeholder_rules.json` (category → shape/color/dimensions) is a real third pattern: the tool body is fixed; users edit a JSON config to change behavior. Worth revisiting once we have ≥3 tools that share a structure (e.g. "place objects from a CSV", "tag objects by class with a record"). Premature today.

## 5. Proposed coverage roadmap

Phased so each step ships value standalone.

### Phase 1 — L2 plumbing + the obvious wins (1 PR)
- Add the L2 `PyTool` machinery to `server.rs`. No new C++ code; everything routes through existing `run_script`.
- Land tools: `export_ifc`, `list_objects_on_layer`, `get_object_bounds`, `get_record_data`, `list_classes`.
- Extend `get_drawing_info` output to include `ifc_scheme`, `vw_version`, `class_count`.

### Phase 2 — Records / data workflows (1 PR)
- `list_record_formats`, `read_records_by_format`, `read_plant_records`.
- Decide: do we ship a generic `read_records_by_format(name)` and skip `read_plant_records`? Probably yes — `read_plant_records` is just `read_records_by_format("Plant Record")`.

### Phase 3 — Additional exports (1 PR)
- `export_dwg`, `export_pdf`. Mirror IFC pattern, look up correct `vs.SetPref` codes per VW version.

### Phase 4 — Geometry creation completers (1 PR, optional)
- `create_polyline`, `create_polygon`, `create_text`, `create_symbol_instance`.
- These *could* go L2, but if performance ever matters (batch placement of 1000s of symbols) they're the L1 candidates.

### Non-goals (explicitly parked)
- AppleScript bridge / Mac fallback — strictly worse than the existing in-process Python path on Windows; only revisit if Mac becomes a hard requirement before a Mac SDK build is ready.
- Mac CMake build — lattice's `CMakeLists.txt` is incomplete (`TODO: target_link_libraries against the VWSDK static libs`). Not a shortcut; will need real work whenever Mac becomes a priority. Use lattice's file as a starting reference only.
- Importing lattice's full TanStack/Pixeltable/reproducibility stack — different beast, consumer of MCP servers, not a peer.
- L3 declarative tools — wait until ≥3 tools justify the pattern.

## 6. Open questions

1. **External tool definitions vs. compiled-in?** L2 tools could live in `tools/*.py` (loaded at startup from disk) instead of `include_str!`'d at build time. Disk-loaded means no rebuild even on the Rust side; tradeoff is one more thing to ship. Lean: compiled-in for v1, disk-loaded later if iteration friction shows up.
2. **Versioning of templated tool bodies vs. VW version?** Some `vs.SetPref` codes change between VW2025 and VW2026 (e.g. IFC export prefs). Should `PyTool` carry an optional `min_vw_version` / per-version body override? Probably yes once we hit one collision; not yet.
3. **Should L1 tools be reimplemented as L2?** The 5 existing typed primitives (`create_line` etc.) would be ~10 lines of Python each. Migrating them collapses the codebase by ~200 lines of C++. But they're proven; "don't fix what isn't broken" applies. **Lean: leave as L1, route all new work through L2.**
4. **Error contract for L2 tools.** A Python exception inside an L2 body already surfaces as `success: false` + traceback in stderr (proven in `FORK_NOTES.md` smoke tests). Do we want richer structured errors (e.g. `{error: "layer_not_found", layer: "Foo"}`)? Probably yes — convention: L2 bodies that need structured errors print a JSON object with an `"error"` key and the server distinguishes that from success.
5. **Schema source of truth.** JSON Schema in Rust via `schemars` macros vs. external JSON files. Lean toward macros for type-safety, file references via `include_str!` for the Python bodies.

## 7. Recommendation summary

- The C++ plumbing is the right substrate. Keep it. Don't port to AppleScript.
- Stop using C++ as the *authoring* layer for new tools. Add L2 (templated Python over the existing `run_script` rail) and put all new tools there.
- Start with Phase 1: `export_ifc` is the single highest-leverage tool to add; the L2 machinery needed for it gets you the next dozen tools cheaply.
- Treat lattice's `vw-python/examples/` as a reference snippets library, not code to import wholesale.
