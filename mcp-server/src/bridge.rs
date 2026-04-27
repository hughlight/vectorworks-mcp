use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::TcpStream;
use tokio::time::timeout;

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

/// Map a VW short tag (30 = VW2025, 31 = VW2026, ...) to a localhost TCP port.
/// We use 27200 + short_tag so VW2025 lives at 27230, VW2026 at 27231, etc.
/// Matches the C++ plugin's port computation in SocketBridge::GetDefaultPort.
pub fn port_for(vw_short: u32) -> u16 {
    27200 + (vw_short as u16)
}

/// Map a user-facing year (2025 / 2026) to the short tag used in the port
/// computation. Numbers >= 2000 are treated as years; everything else is
/// passed through as a short tag.
pub fn year_to_short(vw_year: u32) -> u32 {
    if vw_year >= 2000 {
        // VW2025 -> 30, VW2026 -> 31, ... -- internal version code (year - 1995).
        vw_year - 1995
    } else {
        vw_year
    }
}

pub async fn send(
    vw_short: u32,
    method: &str,
    params: serde_json::Value,
) -> Result<Response> {
    let port = port_for(vw_short);
    let addr = format!("127.0.0.1:{}", port);
    let mut stream = TcpStream::connect(&addr)
        .await
        .with_context(|| format!(
            "Could not connect to {}. Is the VW MCP Bridge plugin loaded and started?",
            addr
        ))?;

    let req = Request {
        id: format!("r_{}", COUNTER.fetch_add(1, Ordering::Relaxed)),
        method: method.to_string(),
        params,
    };

    let mut msg = serde_json::to_string(&req)?;
    msg.push('\n');
    stream.write_all(msg.as_bytes()).await?;
    stream.flush().await?;

    // 30-second hard ceiling on a single VW operation; matches the plugin's
    // SO_RCVTIMEO. Long Renderworks publishes will exceed this — caller's
    // responsibility to use a longer-running pattern in that case.
    let mut reader = BufReader::new(&mut stream);
    let mut line = String::new();
    timeout(Duration::from_secs(30), reader.read_line(&mut line))
        .await
        .context("Timed out waiting for response from Vectorworks")??;

    serde_json::from_str(line.trim()).context("Failed to parse response from VW plugin")
}

pub async fn is_connected(vw_short: u32) -> bool {
    send(vw_short, "ping", serde_json::Value::Null).await.is_ok()
}
