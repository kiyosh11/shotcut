mod bridge;
mod schema;
mod server;

use anyhow::Context;
use rmcp::{ServiceExt, transport::stdio};
use server::ShotcutServer;
use tracing_subscriber::EnvFilter;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| EnvFilter::new("shotcut_mcp=info")),
        )
        .with_writer(std::io::stderr)
        .init();

    let server = ShotcutServer::from_env().context("failed to configure Shotcut MCP")?;
    let service = server
        .serve(stdio())
        .await
        .context("failed to start MCP stdio transport")?;
    service
        .waiting()
        .await
        .context("Shotcut MCP transport stopped unexpectedly")?;
    Ok(())
}
