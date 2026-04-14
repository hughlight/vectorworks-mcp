use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

const SOCKET_PATH: &str = "/tmp/vw-mcp-bridge.sock";
static COUNTER: AtomicU64 = AtomicU64::new(0);

#[derive(Serialize)]
struct Request {
    id: String,
    method: String,
    params: serde_json::Value,
}

#[derive(Debug, Deserialize)]
pub struct Response {
    #[allow(dead_code)]
    pub id: String,
    pub success: bool,
    pub data: serde_json::Value,
}

pub fn send(method: &str, params: serde_json::Value) -> Result<Response> {
    let mut stream = UnixStream::connect(SOCKET_PATH)
        .context("Vectorworks に接続できません。プラグインがロードされているか確認してください")?;
    stream.set_read_timeout(Some(Duration::from_secs(15)))?;
    stream.set_write_timeout(Some(Duration::from_secs(5)))?;

    let req = Request {
        id: format!("r_{}", COUNTER.fetch_add(1, Ordering::Relaxed)),
        method: method.to_string(),
        params,
    };

    let mut msg = serde_json::to_string(&req)?;
    msg.push('\n');
    stream.write_all(msg.as_bytes())?;
    stream.flush()?;

    let mut reader = BufReader::new(&stream);
    let mut line = String::new();
    reader.read_line(&mut line)?;

    serde_json::from_str(line.trim()).context("レスポンスパース失敗")
}

pub fn is_connected() -> bool {
    send("ping", serde_json::Value::Null).is_ok()
}
