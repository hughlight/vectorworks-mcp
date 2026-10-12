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
