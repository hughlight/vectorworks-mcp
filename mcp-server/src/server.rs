use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{ServerCapabilities, ServerInfo};
use rmcp::{schemars, tool, tool_handler, tool_router, ServerHandler};
use schemars::JsonSchema;
use serde::Deserialize;
use serde_json::json;

use crate::bridge;

#[derive(Clone)]
pub struct VwMcpServer {
    #[allow(dead_code)]
    tool_router: ToolRouter<Self>,
    /// Short VW version tag (30 / 31) used to address the right plugin socket.
    vw_short: u32,
}

impl VwMcpServer {
    pub fn new(vw_short: u32) -> Self {
        let tool_router = Self::tool_router();
        Self { tool_router, vw_short }
    }
}

// --- Input schemas ---

#[derive(Debug, Deserialize, JsonSchema)]
pub struct EmptyInput {}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct CreateLineInput {
    pub x1: f64,
    pub y1: f64,
    pub x2: f64,
    pub y2: f64,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct CreateRectInput {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct CreateCircleInput {
    pub cx: f64,
    pub cy: f64,
    pub radius: f64,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct RunScriptInput {
    #[schemars(description = "VectorScript or Python source")]
    pub script: String,
    #[schemars(description = "python or vectorscript (default: python)")]
    pub language: Option<String>,
    #[schemars(description = "Wrap the script in one undo event and roll it back if it fails (default: true)")]
    pub transaction: Option<bool>,
    #[schemars(description = "Name shown in Vectorworks' Edit > Undo for this step (default: 'MCP run_script', max 120 chars)")]
    pub undo_label: Option<String>,
    #[schemars(description = "Diagnostic only. 'context' (default) runs Python via the SDK script context; 'execute' uses the legacy bare ExecuteScript path, which crashes Vectorworks on document mutations.")]
    pub py_mode: Option<String>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct VsSignatureInput {
    #[schemars(description = "vs.* function name without the 'vs.' prefix, e.g. 'NewField'. Case-insensitive; a prefix lists matches.")]
    pub name: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct ListRecordFormatsInput {
    #[schemars(description = "Include hidden formats whose name starts with '__' (plug-in parameter records). Default false.")]
    pub include_hidden: Option<bool>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct ReadRecordsInput {
    #[schemars(description = "Record format name, e.g. 'Lighting Device' or a custom format")]
    pub format: String,
    #[schemars(description = "Restrict to objects on this design layer (exact name). Default: all layers.")]
    pub layer: Option<String>,
    #[schemars(description = "Maximum rows to return (default 500)")]
    pub limit: Option<u32>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct ExportIfcInput {
    #[schemars(description = "Absolute output path ending in .ifc. Uses the document's current IFC export settings, no dialog.")]
    pub path: String,
}

// --- L2 tools: templated Python bodies over run_script ---
//
// Each body prints exactly one JSON document to stdout. The server runs it
// with transaction=false (reads and exports do not need an undo event) and
// pretty-prints the parsed JSON. Parameters are injected as Python string
// literals via py_str (a JSON string literal is a valid Python literal).

fn py_str(s: &str) -> String {
    serde_json::to_string(s).unwrap_or_else(|_| "\"\"".into())
}

fn py_opt_str(s: &Option<String>) -> String {
    match s {
        Some(v) => py_str(v),
        None => "None".into(),
    }
}

const PY_LIST_CLASSES: &str = r#"import vs, json
n = vs.ClassNum()
out = []
for i in range(1, n + 1):
    name = vs.ClassList(i)
    out.append({"name": name, "visibility": vs.GetCVis(name)})
print(json.dumps({"count": len(out), "visibility_legend": {"0": "visible", "-1": "invisible", "2": "grayed"}, "classes": out}))
"#;

const PY_LIST_RECORD_FORMATS: &str = r#"import vs, json
include_hidden = __INCLUDE_HIDDEN__
lid, n = vs.BuildResourceList(47, 0, '')
out = []
for i in range(1, n + 1):
    name = vs.GetNameFromResourceList(lid, i)
    if not include_hidden and name.startswith('__'):
        continue
    h = vs.GetObject(name)
    fields = []
    if h:
        for f in range(1, vs.NumFields(h) + 1):
            fields.append({"name": vs.GetFldName(h, f), "type": vs.GetFldType(h, f)})
    out.append({"name": name, "fields": fields})
print(json.dumps({"count": len(out), "formats": out}))
"#;

const PY_READ_RECORDS: &str = r#"import vs, json
fmt = __FMT__
layer = __LAYER__
limit = __LIMIT__
h = vs.GetObject(fmt)
if not h:
    print(json.dumps({"error": "record format not found", "format": fmt}))
else:
    names = [vs.GetFldName(h, i) for i in range(1, vs.NumFields(h) + 1)]
    rows = []
    truncated = [False]
    def cb(obj):
        if len(rows) >= limit:
            truncated[0] = True
            return
        vals = {}
        for nm in names:
            vals[nm] = vs.GetRField(obj, fmt, nm)
        lh = vs.GetLayer(obj)
        rows.append({"type": vs.GetTypeN(obj), "class": vs.GetClass(obj),
                     "layer": vs.GetLName(lh) if lh else "", "name": vs.GetName(obj), "values": vals})
    crit = "(R IN ['" + fmt.replace("'", "''") + "'])"
    if layer:
        crit += " & (L='" + layer.replace("'", "''") + "')"
    vs.ForEachObject(cb, crit)
    print(json.dumps({"format": fmt, "fields": names, "count": len(rows), "truncated": truncated[0], "rows": rows}))
"#;

const PY_EXPORT_IFC: &str = r#"import vs, json, os
path = __PATH__
ok = vs.IFC_ExportNoUI(path)
print(json.dumps({"ok": bool(ok), "path": path, "exists": os.path.exists(path)}))
"#;

/// Run a Python body that prints one JSON document; return it pretty-printed.
async fn run_python_json(vw_short: u32, script: String, label: &str) -> String {
    let params = json!({
        "script": script,
        "language": "python",
        "transaction": false,
        "undo_label": label,
    });
    match bridge::send(vw_short, "run_script", params).await {
        Err(e) => format!("Connection error: {e}"),
        Ok(r) => {
            let stdout = r.data.get("stdout").and_then(|v| v.as_str()).unwrap_or("").to_string();
            let stderr = r.data.get("stderr").and_then(|v| v.as_str()).unwrap_or("").to_string();
            if !r.success {
                return if stderr.is_empty() { format!("Error: {}", r.data) } else { format!("Error: {stderr}") };
            }
            match serde_json::from_str::<serde_json::Value>(stdout.trim()) {
                Ok(v) => serde_json::to_string_pretty(&v).unwrap_or(stdout),
                Err(_) => stdout,
            }
        }
    }
}

// --- vs.* signature index (vicquick/vwx-mcp, MIT; see NOTICE) ---

/// 3071 VectorScript/Python `vs.*` signatures: {name: {args, arity, required, ret, cat, doc}}.
static VS_INDEX_JSON: &str = include_str!("../vs_index.json");

fn vs_index() -> &'static serde_json::Map<String, serde_json::Value> {
    static INDEX: std::sync::OnceLock<serde_json::Map<String, serde_json::Value>> =
        std::sync::OnceLock::new();
    INDEX.get_or_init(|| {
        serde_json::from_str::<serde_json::Value>(VS_INDEX_JSON)
            .ok()
            .and_then(|v| v.as_object().cloned())
            .unwrap_or_default()
    })
}

/// Exact (case-insensitive) lookup first; otherwise up to 15 names that start
/// with or contain the query, so a half-remembered name still gets the caller
/// somewhere. Returns pretty JSON either way.
pub fn vs_signature_lookup(name: &str) -> String {
    let index = vs_index();
    let query = name.trim().trim_start_matches("vs.");
    if query.is_empty() {
        return "Error: name is required".into();
    }
    let lower = query.to_ascii_lowercase();
    if let Some((key, entry)) = index.iter().find(|(k, _)| k.to_ascii_lowercase() == lower) {
        let mut out = serde_json::Map::new();
        out.insert("name".into(), json!(key));
        out.insert("python".into(), json!(format!(
            "vs.{}({})",
            key,
            entry.get("args").and_then(|a| a.as_array())
                .map(|a| a.iter().filter_map(|x| x.as_str()).collect::<Vec<_>>().join(", "))
                .unwrap_or_default()
        )));
        if let Some(obj) = entry.as_object() {
            for (k, v) in obj {
                out.insert(k.clone(), v.clone());
            }
        }
        return serde_json::to_string_pretty(&out).unwrap_or_default();
    }
    let mut starts: Vec<&String> = index.keys().filter(|k| k.to_ascii_lowercase().starts_with(&lower)).collect();
    let mut contains: Vec<&String> = index.keys()
        .filter(|k| { let l = k.to_ascii_lowercase(); l.contains(&lower) && !l.starts_with(&lower) })
        .collect();
    starts.sort();
    contains.sort();
    let matches: Vec<&String> = starts.into_iter().chain(contains).take(15).collect();
    if matches.is_empty() {
        format!("No vs.* function named {query:?} in the index ({} entries).", index.len())
    } else {
        serde_json::to_string_pretty(&json!({
            "query": query,
            "exact": false,
            "matches": matches,
        })).unwrap_or_default()
    }
}

async fn call(vw_short: u32, method: &str, params: serde_json::Value) -> String {
    match bridge::send(vw_short, method, params).await {
        Ok(r) if r.success => {
            serde_json::to_string_pretty(&r.data).unwrap_or_else(|_| r.data.to_string())
        }
        Ok(r) => format!("Error: {}", r.data),
        Err(e) => format!("Connection error: {}", e),
    }
}

#[tool_router]
impl VwMcpServer {
    #[tool(name = "get_drawing_info", description = "Get info about the current Vectorworks drawing")]
    async fn get_drawing_info(&self, Parameters(_): Parameters<EmptyInput>) -> String {
        call(self.vw_short, "get_drawing_info", json!({})).await
    }

    #[tool(name = "list_layers", description = "List the layers in the current document")]
    async fn list_layers(&self, Parameters(_): Parameters<EmptyInput>) -> String {
        call(self.vw_short, "list_layers", json!({})).await
    }

    #[tool(name = "create_line", description = "Draw a line segment")]
    async fn create_line(&self, Parameters(i): Parameters<CreateLineInput>) -> String {
        call(self.vw_short, "create_line", json!({"x1": i.x1, "y1": i.y1, "x2": i.x2, "y2": i.y2})).await
    }

    #[tool(name = "create_rect", description = "Draw a rectangle")]
    async fn create_rect(&self, Parameters(i): Parameters<CreateRectInput>) -> String {
        call(self.vw_short, "create_rect", json!({"x": i.x, "y": i.y, "width": i.width, "height": i.height})).await
    }

    #[tool(name = "create_circle", description = "Draw a circle")]
    async fn create_circle(&self, Parameters(i): Parameters<CreateCircleInput>) -> String {
        call(self.vw_short, "create_circle", json!({"cx": i.cx, "cy": i.cy, "radius": i.radius})).await
    }

    #[tool(
        name = "run_script",
        description = "Execute Python (default) or VectorScript inside Vectorworks on its main thread. \
                       Each call is one undo event (transaction=true): if the script raises or writes \
                       anything to stderr, the event is rolled back and success is false, so print \
                       results to stdout only. Returns stdout, stderr, error_code, rolled_back. \
                       Hard limit of 25 seconds per call; a longer script times out here but may \
                       still finish inside Vectorworks. After a contained engine fault every call \
                       returns error=engine_poisoned until Vectorworks is restarted. \
                       Trusted environment only."
    )]
    async fn run_script(&self, Parameters(i): Parameters<RunScriptInput>) -> String {
        let mut params = json!({
            "script": i.script,
            "language": i.language.unwrap_or_else(|| "python".into()),
        });
        // Only forward the optional fields when given so the plugin applies
        // its own defaults (transaction=true, undo_label="MCP run_script",
        // py_mode from VWMCP_PY_MODE or "context").
        if let Some(t) = i.transaction {
            params["transaction"] = json!(t);
        }
        if let Some(l) = i.undo_label {
            params["undo_label"] = json!(l);
        }
        if let Some(m) = i.py_mode {
            params["py_mode"] = json!(m);
        }
        call(self.vw_short, "run_script", params).await
    }

    #[tool(
        name = "list_classes",
        description = "List every class in the current document with its visibility (0 visible, -1 invisible, 2 grayed)."
    )]
    async fn list_classes(&self, Parameters(_): Parameters<EmptyInput>) -> String {
        run_python_json(self.vw_short, PY_LIST_CLASSES.to_string(), "MCP list_classes").await
    }

    #[tool(
        name = "list_record_formats",
        description = "List the record formats defined in the current document with their field names and type codes. \
                       Hidden '__' plug-in parameter records are skipped unless include_hidden is true."
    )]
    async fn list_record_formats(&self, Parameters(i): Parameters<ListRecordFormatsInput>) -> String {
        let script = PY_LIST_RECORD_FORMATS.replace(
            "__INCLUDE_HIDDEN__",
            if i.include_hidden.unwrap_or(false) { "True" } else { "False" },
        );
        run_python_json(self.vw_short, script, "MCP list_record_formats").await
    }

    #[tool(
        name = "read_records_by_format",
        description = "Read every object carrying the given record format: object type, class, layer, name and all \
                       field values as strings. Optional layer filter and row limit (default 500). Read-only."
    )]
    async fn read_records_by_format(&self, Parameters(i): Parameters<ReadRecordsInput>) -> String {
        let script = PY_READ_RECORDS
            .replace("__FMT__", &py_str(&i.format))
            .replace("__LAYER__", &py_opt_str(&i.layer))
            .replace("__LIMIT__", &i.limit.unwrap_or(500).to_string());
        run_python_json(self.vw_short, script, "MCP read_records_by_format").await
    }

    #[tool(
        name = "export_ifc",
        description = "Export the current document to an IFC file at the given absolute path using the document's \
                       saved IFC export settings (vs.IFC_ExportNoUI, no dialog). Large documents may exceed the 25 s \
                       per-call limit; the export may still complete inside Vectorworks."
    )]
    async fn export_ifc(&self, Parameters(i): Parameters<ExportIfcInput>) -> String {
        let script = PY_EXPORT_IFC.replace("__PATH__", &py_str(&i.path));
        run_python_json(self.vw_short, script, "MCP export_ifc").await
    }

    #[tool(
        name = "vs_signature",
        description = "Look up a Vectorworks vs.* scripting function: arguments, arity, return type, \
                       category and one-line doc, from an offline index of 3071 signatures. Use it \
                       before calling an unfamiliar vs.* function in run_script. Exact name or a \
                       prefix/substring to list candidates."
    )]
    async fn vs_signature(&self, Parameters(i): Parameters<VsSignatureInput>) -> String {
        vs_signature_lookup(&i.name)
    }
}

#[cfg(test)]
mod tests {
    use super::vs_signature_lookup;

    #[test]
    fn exact_lookup_returns_entry_with_python_call() {
        let out = vs_signature_lookup("NewField");
        assert!(out.contains("\"name\": \"NewField\""), "{out}");
        assert!(out.contains("\"python\": \"vs.NewField("), "{out}");
        assert!(out.contains("\"arity\""), "{out}");
    }

    #[test]
    fn lookup_is_case_insensitive_and_strips_vs_prefix() {
        let out = vs_signature_lookup("vs.buildresourcelist");
        assert!(out.contains("\"name\": \"BuildResourceList\""), "{out}");
    }

    #[test]
    fn prefix_lists_candidates() {
        let out = vs_signature_lookup("GetRField");
        assert!(out.contains("\"exact\": false") || out.contains("\"name\": \"GetRField\""), "{out}");
    }

    #[test]
    fn unknown_name_says_so() {
        let out = vs_signature_lookup("DefinitelyNotAFunctionXYZ");
        assert!(out.starts_with("No vs.* function named"), "{out}");
    }
}

#[tool_handler]
impl ServerHandler for VwMcpServer {
    fn get_info(&self) -> ServerInfo {
        let mut info = ServerInfo::new(ServerCapabilities::builder().enable_tools().build());
        info.instructions = Some(format!(
            "Vectorworks MCP Server (target VW short tag: {}). Tools: drawing info, \
             layer list, basic drawing primitives, and run_script for arbitrary \
             VectorScript or Python execution with stdout/stderr capture.",
            self.vw_short
        ));
        info
    }
}
