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
}

impl VwMcpServer {
    pub fn new() -> Self {
        let tool_router = Self::tool_router();
        Self { tool_router }
    }
}

// --- 入力スキーマ ---

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
    #[schemars(description = "VectorScript または Python スクリプト")]
    pub script: String,
    #[schemars(description = "python or vectorscript (デフォルト: vectorscript)")]
    pub language: Option<String>,
}

fn call(method: &str, params: serde_json::Value) -> String {
    match bridge::send(method, params) {
        Ok(r) if r.success => {
            serde_json::to_string_pretty(&r.data).unwrap_or_else(|_| r.data.to_string())
        }
        Ok(r) => format!("Error: {}", r.data),
        Err(e) => format!("Connection error: {}", e),
    }
}

#[tool_router]
impl VwMcpServer {
    #[tool(name = "get_drawing_info", description = "Vectorworks の図面情報を取得")]
    async fn get_drawing_info(&self, Parameters(_): Parameters<EmptyInput>) -> String {
        call("get_drawing_info", json!({}))
    }

    #[tool(name = "list_layers", description = "レイヤー一覧を取得")]
    async fn list_layers(&self, Parameters(_): Parameters<EmptyInput>) -> String {
        call("list_layers", json!({}))
    }

    #[tool(name = "create_line", description = "線分を描画")]
    async fn create_line(&self, Parameters(i): Parameters<CreateLineInput>) -> String {
        call("create_line", json!({"x1": i.x1, "y1": i.y1, "x2": i.x2, "y2": i.y2}))
    }

    #[tool(
        name = "create_rect",
        description = "四角形を描画"
    )]
    async fn create_rect(&self, Parameters(i): Parameters<CreateRectInput>) -> String {
        call("create_rect", json!({"x": i.x, "y": i.y, "width": i.width, "height": i.height}))
    }

    #[tool(name = "create_circle", description = "円を描画")]
    async fn create_circle(&self, Parameters(i): Parameters<CreateCircleInput>) -> String {
        call("create_circle", json!({"cx": i.cx, "cy": i.cy, "radius": i.radius}))
    }

    #[tool(
        name = "run_script",
        description = "VectorScript または Python スクリプトを実行。信頼された環境専用"
    )]
    async fn run_script(&self, Parameters(i): Parameters<RunScriptInput>) -> String {
        call(
            "run_script",
            json!({"script": i.script, "language": i.language.unwrap_or_else(|| "vectorscript".into())}),
        )
    }
}

#[tool_handler]
impl ServerHandler for VwMcpServer {
    fn get_info(&self) -> ServerInfo {
        let mut info = ServerInfo::new(ServerCapabilities::builder().enable_tools().build());
        info.instructions = Some(
            "Vectorworks MCP Server — Vectorworks を操作するための MCP サーバー。\
             図面情報取得、レイヤー管理、図形描画、スクリプト実行が可能。"
                .into(),
        );
        info
    }
}
