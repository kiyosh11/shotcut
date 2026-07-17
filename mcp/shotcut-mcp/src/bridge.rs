use std::{
    env, io,
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
    io::{AsyncBufRead, AsyncBufReadExt, AsyncRead, AsyncWrite, AsyncWriteExt, BufReader},
    time,
};

const DEFAULT_ENDPOINT: &str = "shotcut-mcp";
const DEFAULT_TIMEOUT_SECONDS: u64 = 300;
const MAX_MESSAGE_BYTES: usize = 16 * 1024 * 1024;
const BRIDGE_PROTOCOL_VERSION: u32 = 2;

fn framed_message_fits(payload_bytes: usize, max_bytes: usize) -> bool {
    payload_bytes
        .checked_add(1)
        .is_some_and(|framed_bytes| framed_bytes <= max_bytes)
}

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

impl BridgeError {
    pub fn structured(&self) -> Value {
        match self {
            Self::Remote {
                code,
                message,
                data,
            } => serde_json::json!({
                "kind": "remote",
                "code": code,
                "message": message,
                "data": data.0.clone(),
            }),
            Self::Configuration(_) => {
                serde_json::json!({"kind": "configuration", "message": self.to_string()})
            }
            Self::Io(_) => serde_json::json!({"kind": "io", "message": self.to_string()}),
            Self::Timeout(seconds) => serde_json::json!({
                "kind": "timeout",
                "timeout_seconds": seconds,
                "message": self.to_string(),
            }),
            Self::InvalidResponse(_) => {
                serde_json::json!({"kind": "invalid_response", "message": self.to_string()})
            }
            Self::Json(_) => serde_json::json!({"kind": "json", "message": self.to_string()}),
        }
    }
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
    bridge_protocol: Option<u32>,
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

fn decode_response(response_text: &str, expected_id: u64) -> Result<Value, BridgeError> {
    let response: RpcResponse = serde_json::from_str(response_text)?;
    if response.bridge_protocol != Some(BRIDGE_PROTOCOL_VERSION) {
        return Err(BridgeError::InvalidResponse(format!(
            "incompatible bridge protocol: expected {BRIDGE_PROTOCOL_VERSION}, received {}",
            response
                .bridge_protocol
                .map_or_else(|| "missing".to_owned(), |value| value.to_string())
        )));
    }
    if response.jsonrpc != "2.0" {
        return Err(BridgeError::InvalidResponse(
            "jsonrpc must equal 2.0".into(),
        ));
    }
    if response.id != expected_id {
        return Err(BridgeError::InvalidResponse(format!(
            "response id {} does not match request id {expected_id}",
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
        if !framed_message_fits(payload.len(), MAX_MESSAGE_BYTES) {
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

        decode_response(&response_text, id)
    }

    fn build_request<'a, P>(
        &self,
        id: u64,
        method: &'a str,
        params: P,
    ) -> Result<RpcRequest<'a>, BridgeError>
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
        object.insert(
            "bridge_protocol".into(),
            Value::Number(BRIDGE_PROTOCOL_VERSION.into()),
        );

        Ok(RpcRequest {
            jsonrpc: "2.0",
            id,
            method,
            params: Value::Object(object),
        })
    }
}

async fn read_bounded_line<R>(reader: &mut R, max_bytes: usize) -> io::Result<String>
where
    R: AsyncBufRead + Unpin,
{
    let mut response = Vec::new();
    loop {
        let buffer = reader.fill_buf().await?;
        if buffer.is_empty() {
            let message = if response.is_empty() {
                "Shotcut closed the bridge without a response"
            } else {
                "Shotcut bridge response ended before the line terminator"
            };
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, message));
        }

        let newline = buffer.iter().position(|byte| *byte == b'\n');
        let chunk_len = newline.map_or(buffer.len(), |index| index + 1);
        if chunk_len > max_bytes.saturating_sub(response.len()) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Shotcut bridge response is too large",
            ));
        }
        response.extend_from_slice(&buffer[..chunk_len]);
        reader.consume(chunk_len);
        if newline.is_some() {
            break;
        }
    }

    response.pop();
    if response.last() == Some(&b'\r') {
        response.pop();
    }
    String::from_utf8(response).map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))
}

async fn exchange<S>(stream: S, payload: &[u8]) -> io::Result<String>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let mut stream = BufReader::new(stream);
    stream.get_mut().write_all(payload).await?;
    stream.get_mut().write_all(b"\n").await?;
    stream.get_mut().flush().await?;
    read_bounded_line(&mut stream, MAX_MESSAGE_BYTES).await
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
    Err(last_error
        .unwrap_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "named pipe remained busy")))
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
        let client = client();
        let request = client
            .build_request(7, "editor.status", serde_json::json!({}))
            .unwrap();
        assert_eq!(request.id, 7);
        assert_eq!(request.params["token"], "0123456789abcdef0123456789abcdef");
        assert_eq!(request.params["bridge_protocol"], BRIDGE_PROTOCOL_VERSION);
    }

    #[test]
    fn non_object_params_are_rejected() {
        assert!(client().build_request(1, "bad", vec![1, 2]).is_err());
    }

    #[test]
    fn request_limit_includes_newline_framing() {
        assert!(framed_message_fits(
            MAX_MESSAGE_BYTES - 1,
            MAX_MESSAGE_BYTES
        ));
        assert!(!framed_message_fits(MAX_MESSAGE_BYTES, MAX_MESSAGE_BYTES));
        assert!(!framed_message_fits(usize::MAX, MAX_MESSAGE_BYTES));
    }

    #[test]
    fn response_requires_the_exact_bridge_protocol() {
        let missing = r#"{"jsonrpc":"2.0","id":1,"result":{}}"#;
        assert!(matches!(
            decode_response(missing, 1),
            Err(BridgeError::InvalidResponse(message)) if message.contains("received missing")
        ));

        let wrong = r#"{"jsonrpc":"2.0","id":1,"bridge_protocol":1,"result":{}}"#;
        assert!(matches!(
            decode_response(wrong, 1),
            Err(BridgeError::InvalidResponse(message)) if message.contains("received 1")
        ));

        let current = serde_json::json!({
            "jsonrpc": "2.0",
            "id": 1,
            "bridge_protocol": BRIDGE_PROTOCOL_VERSION,
            "result": {"ok": true},
        })
        .to_string();
        assert_eq!(decode_response(&current, 1).unwrap()["ok"], true);
    }

    async fn read_fixture(input: &[u8], limit: usize) -> io::Result<String> {
        let (mut writer, reader) = tokio::io::duplex(input.len().max(1));
        writer.write_all(input).await?;
        writer.shutdown().await?;
        drop(writer);
        read_bounded_line(&mut BufReader::new(reader), limit).await
    }

    #[tokio::test]
    async fn bounded_reader_accepts_a_complete_line_at_the_limit() {
        let response = read_fixture(b"123456\r\n", 8).await.unwrap();
        assert_eq!(response, "123456");
    }

    #[tokio::test]
    async fn bounded_reader_rejects_an_oversized_line() {
        let error = read_fixture(b"12345678\n", 8).await.unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidData);
    }

    #[tokio::test]
    async fn bounded_reader_rejects_a_partial_line_at_eof() {
        let error = read_fixture(b"partial", 8).await.unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::UnexpectedEof);
    }

    #[tokio::test]
    async fn exchange_uses_newline_framing() {
        let (client_stream, server_stream) = tokio::io::duplex(256);
        let server = tokio::spawn(async move {
            let mut server = BufReader::new(server_stream);
            let mut request = String::new();
            server.read_line(&mut request).await.unwrap();
            assert_eq!(request, "{\"ping\":true}\n");
            server
                .get_mut()
                .write_all(b"{\"ok\":true}\r\n")
                .await
                .unwrap();
        });

        let response = exchange(client_stream, b"{\"ping\":true}").await.unwrap();
        server.await.unwrap();
        assert_eq!(response, "{\"ok\":true}");
    }
}
