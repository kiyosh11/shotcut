use std::{
    env,
    io,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::Duration,
};

use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use thiserror::Error;
use tokio::{
    io::{AsyncBufReadExt, AsyncRead, AsyncWrite, AsyncWriteExt, BufReader},
    time,
};

const DEFAULT_ENDPOINT: &str = "shotcut-mcp";
const DEFAULT_TIMEOUT_SECONDS: u64 = 30;
const MAX_MESSAGE_BYTES: usize = 16 * 1024 * 1024;

#[derive(Debug, Error)]
pub enum BridgeError {
    #[error("bridge configuration error: {0}")]
    Configuration(String),
    #[error("Shotcut bridge I/O error: {0}")]
    Io(#[from] io::Error),
    #[error("Shotcut bridge timed out after {0} seconds")]
    Timeout(u64),
    #[error("invalid Shotcut bridge response: {0}")]
    InvalidResponse(String),
    #[error("Shotcut bridge error {code}: {message}{data}")]
    Remote {
        code: i32,
        message: String,
        data: DisplayData,
    },
    #[error("failed to encode or decode bridge JSON: {0}")]
    Json(#[from] serde_json::Error),
}

#[derive(Debug, Default)]
pub struct DisplayData(Option<Value>);

impl std::fmt::Display for DisplayData {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match &self.0 {
            Some(value) => write!(formatter, " ({value})"),
            None => Ok(()),
        }
    }
}

#[derive(Debug, Clone)]
pub struct BridgeClient {
    endpoint: String,
    token: String,
    next_id: Arc<AtomicU64>,
    timeout: Duration,
}

#[derive(Debug, Serialize)]
struct RpcRequest<'a> {
    jsonrpc: &'static str,
    id: u64,
    method: &'a str,
    params: Value,
}

#[derive(Debug, Deserialize)]
struct RpcResponse {
    jsonrpc: String,
    id: Value,
    #[serde(default)]
    result: Option<Value>,
    #[serde(default)]
    error: Option<RpcError>,
}

#[derive(Debug, Deserialize)]
struct RpcError {
    code: i32,
    message: String,
    #[serde(default)]
    data: Option<Value>,
}

impl BridgeClient {
    pub fn from_env() -> Result<Self, BridgeError> {
        let token = env::var("SHOTCUT_MCP_TOKEN").map_err(|_| {
            BridgeError::Configuration(
                "SHOTCUT_MCP_TOKEN must be set in both Shotcut and this MCP server".into(),
            )
        })?;
        if token.len() < 32 {
            return Err(BridgeError::Configuration(
                "SHOTCUT_MCP_TOKEN must contain at least 32 characters".into(),
            ));
        }

        let endpoint =
            env::var("SHOTCUT_MCP_ENDPOINT").unwrap_or_else(|_| DEFAULT_ENDPOINT.to_owned());
        let timeout_seconds = env::var("SHOTCUT_MCP_TIMEOUT_SECONDS")
            .ok()
            .and_then(|value| value.parse::<u64>().ok())
            .filter(|value| *value > 0)
            .unwrap_or(DEFAULT_TIMEOUT_SECONDS);

        Ok(Self {
            endpoint,
            token,
            next_id: Arc::new(AtomicU64::new(1)),
            timeout: Duration::from_secs(timeout_seconds),
        })
    }

    pub async fn call<P>(&self, method: &str, params: P) -> Result<Value, BridgeError>
    where
        P: Serialize,
    {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let request = self.build_request(id, method, params)?;
        let payload = serde_json::to_vec(&request)?;
        if payload.len() > MAX_MESSAGE_BYTES {
            return Err(BridgeError::Configuration(format!(
                "request exceeds the {MAX_MESSAGE_BYTES}-byte bridge limit"
            )));
        }

        let response_text = time::timeout(self.timeout, transact(&self.endpoint, &payload))
            .await
            .map_err(|_| BridgeError::Timeout(self.timeout.as_secs()))??;
        if response_text.len() > MAX_MESSAGE_BYTES {
            return Err(BridgeError::InvalidResponse(format!(
                "response exceeds the {MAX_MESSAGE_BYTES}-byte bridge limit"
            )));
        }

        let response: RpcResponse = serde_json::from_str(&response_text)?;
        if response.jsonrpc != "2.0" {
            return Err(BridgeError::InvalidResponse(
                "jsonrpc must equal 2.0".into(),
            ));
        }
        if response.id != Value::from(id) {
            return Err(BridgeError::InvalidResponse(format!(
                "response id {} does not match request id {id}",
                response.id
            )));
        }
        if let Some(error) = response.error {
            return Err(BridgeError::Remote {
                code: error.code,
                message: error.message,
                data: DisplayData(error.data),
            });
        }
        response
            .result
            .ok_or_else(|| BridgeError::InvalidResponse("missing result and error".into()))
    }

    fn build_request<P>(
        &self,
        id: u64,
        method: &str,
        params: P,
    ) -> Result<RpcRequest<'_>, BridgeError>
    where
        P: Serialize,
    {
        let value = serde_json::to_value(params)?;
        let mut object = match value {
            Value::Object(object) => object,
            Value::Null => Map::new(),
            _ => {
                return Err(BridgeError::Configuration(
                    "bridge params must serialize to a JSON object".into(),
                ));
            }
        };
        object.insert("token".into(), Value::String(self.token.clone()));

        Ok(RpcRequest {
            jsonrpc: "2.0",
            id,
            method,
            params: Value::Object(object),
        })
    }
}

async fn exchange<S>(stream: S, payload: &[u8]) -> io::Result<String>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let mut stream = BufReader::new(stream);
    stream.get_mut().write_all(payload).await?;
    stream.get_mut().write_all(b"\n").await?;
    stream.get_mut().flush().await?;

    let mut response = Vec::new();
    stream.read_until(b'\n', &mut response).await?;
    if response.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "Shotcut closed the bridge without a response",
        ));
    }
    if response.len() > MAX_MESSAGE_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Shotcut bridge response is too large",
        ));
    }
    if response.last() == Some(&b'\n') {
        response.pop();
    }
    if response.last() == Some(&b'\r') {
        response.pop();
    }
    String::from_utf8(response)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))
}

#[cfg(unix)]
async fn transact(endpoint: &str, payload: &[u8]) -> io::Result<String> {
    use std::path::{Path, PathBuf};
    use tokio::net::UnixStream;

    let path = if Path::new(endpoint).is_absolute() {
        PathBuf::from(endpoint)
    } else {
        env::temp_dir().join(endpoint)
    };
    let stream = UnixStream::connect(path).await?;
    exchange(stream, payload).await
}

#[cfg(windows)]
async fn transact(endpoint: &str, payload: &[u8]) -> io::Result<String> {
    use tokio::net::windows::named_pipe::ClientOptions;

    let pipe_name = if endpoint.starts_with(r"\\.\pipe\") {
        endpoint.to_owned()
    } else {
        format!(r"\\.\pipe\{endpoint}")
    };

    let mut last_error = None;
    for _ in 0..20 {
        match ClientOptions::new().open(&pipe_name) {
            Ok(stream) => return exchange(stream, payload).await,
            Err(error) if error.raw_os_error() == Some(231) => {
                last_error = Some(error);
                time::sleep(Duration::from_millis(50)).await;
            }
            Err(error) => return Err(error),
        }
    }
    Err(last_error.unwrap_or_else(|| {
        io::Error::new(io::ErrorKind::TimedOut, "named pipe remained busy")
    }))
}

#[cfg(not(any(unix, windows)))]
async fn transact(_endpoint: &str, _payload: &[u8]) -> io::Result<String> {
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "Shotcut MCP supports Windows and Unix local sockets",
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn client() -> BridgeClient {
        BridgeClient {
            endpoint: DEFAULT_ENDPOINT.into(),
            token: "0123456789abcdef0123456789abcdef".into(),
            next_id: Arc::new(AtomicU64::new(1)),
            timeout: Duration::from_secs(1),
        }
    }

    #[test]
    fn auth_token_is_injected_into_params() {
        let request = client()
            .build_request(7, "editor.status", serde_json::json!({}))
            .unwrap();
        assert_eq!(request.id, 7);
        assert_eq!(request.params["token"], "0123456789abcdef0123456789abcdef");
    }

    #[test]
    fn non_object_params_are_rejected() {
        assert!(client().build_request(1, "bad", vec![1, 2]).is_err());
    }
}
