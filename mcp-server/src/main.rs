mod bridge;
mod server;

use anyhow::Result;
use rmcp::ServiceExt;
use tracing_subscriber::EnvFilter;

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .with_env_filter(EnvFilter::from_default_env().add_directive("info".parse()?))
        .init();

    // Parse --vw-version FLAG. Accepted values:
    //   "2025" / "2026" (year)         → mapped to short 30 / 31
    //   "30"   / "31"   (short)        → used directly
    //   missing                        → defaults to VW2026 (short 31)
    let args: Vec<String> = std::env::args().collect();
    let mut vw_arg: Option<String> = None;
    let mut iter = args.iter().skip(1);
    while let Some(a) = iter.next() {
        if a == "--vw-version" || a == "-v" {
            vw_arg = iter.next().cloned();
        } else if let Some(rest) = a.strip_prefix("--vw-version=") {
            vw_arg = Some(rest.to_string());
        }
    }
    let vw_short = match vw_arg.as_deref() {
        None => 31u32, // default: VW2026
        Some(s) => match s.parse::<u32>() {
            Ok(n) => bridge::year_to_short(n),
            Err(_) => {
                eprintln!("Invalid --vw-version value: {:?}. Defaulting to VW2026.", s);
                31u32
            }
        },
    };

    tracing::info!("Fuse VW Gantry server starting (vw_short={})", vw_short);
    tracing::info!("Endpoint: 127.0.0.1:{}", bridge::port_for(vw_short));

    if bridge::is_connected(vw_short).await {
        tracing::info!("Fuse VW Gantry bridge: connected");
    } else {
        tracing::warn!("Fuse VW Gantry bridge: not connected (plugin may not be loaded)");
    }

    let server = server::VwMcpServer::new(vw_short);
    let service = server.serve(rmcp::transport::stdio()).await?;

    tracing::info!("Fuse VW Gantry server running on stdio");
    service.waiting().await?;
    Ok(())
}
