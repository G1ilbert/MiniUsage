use std::env;
use std::fs;
use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tokio::sync::Mutex;

use axum::{
    extract::State,
    http::StatusCode,
    response::Json,
    routing::get,
    Router,
};
use chrono::DateTime;
use reqwest::Client;
use serde::{Deserialize, Serialize};

// ---------- Config ----------
const POLL_INTERVAL_SECS: u64 = 60;
const LISTEN_ADDR: &str = "0.0.0.0:8765";
const CLIENT_ID: &str = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
const REFRESH_URLS: &[&str] = &[
    "https://claude.ai/api/oauth/token",
    "https://console.anthropic.com/v1/oauth/token",
    "https://auth.anthropic.com/oauth/token",
];
// refresh when token expires within this window
const REFRESH_LEEWAY_MS: i64 = 5 * 60 * 1000;

// ---------- Types ----------
#[derive(Debug, Clone, Serialize)]
struct UsageData {
    ok: bool,
    session_pct: u8,
    session_reset_in: u64,
    weekly_pct: u8,
    weekly_reset_in: u64,
}

#[derive(Debug, Clone, Serialize)]
struct ErrorData {
    ok: bool,
    error: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
struct OauthTokens {
    #[serde(rename = "accessToken")]
    access_token: String,
    #[serde(rename = "refreshToken")]
    refresh_token: String,
    #[serde(rename = "expiresAt")]
    expires_at: i64,
    // preserve any other fields Claude Code stores (scopes, subscriptionType, ...)
    #[serde(flatten)]
    extra: serde_json::Map<String, serde_json::Value>,
}

#[derive(Debug, Deserialize, Serialize)]
struct CredentialsFile {
    #[serde(rename = "claudeAiOauth")]
    claude_ai_oauth_tokens: OauthTokens,
    #[serde(flatten)]
    extra: serde_json::Map<String, serde_json::Value>,
}

#[derive(Clone)]
struct AppState {
    client: Client,
    credentials_path: PathBuf,
    creds: Arc<Mutex<OauthTokens>>,
    cache: Arc<Mutex<Option<CachedUsage>>>,
}

struct CachedUsage {
    data: UsageData,
    fetched_at: Instant,
}

#[derive(Serialize)]
struct AnthropicRequest {
    model: &'static str,
    max_tokens: u32,
    messages: Vec<Message>,
}

#[derive(Serialize)]
struct Message {
    role: &'static str,
    content: &'static str,
}

#[derive(Deserialize)]
struct RefreshResponse {
    access_token: String,
    #[serde(default)]
    refresh_token: Option<String>,
    expires_in: i64,
}

// ---------- Main ----------
#[tokio::main]
async fn main() {
    let credentials_path = credentials_path();
    println!("Reading credentials from: {}", credentials_path.display());

    let creds = load_credentials(&credentials_path)
        .expect("failed to read Claude Code credentials");

    println!("Claude usage proxy starting on {LISTEN_ADDR}");

    let client = Client::builder()
        .timeout(Duration::from_secs(10))
        .build()
        .expect("failed to build HTTP client");

    let state = AppState {
        client,
        credentials_path,
        creds: Arc::new(Mutex::new(creds)),
        cache: Arc::new(Mutex::new(None)),
    };

    let app = Router::new()
        .route("/usage", get(handle_usage))
        .route("/health", get(|| async { "ok" }))
        .with_state(state);

    let addr: SocketAddr = LISTEN_ADDR.parse().unwrap();
    println!("Listening on http://{addr}/usage");

    axum::Server::bind(&addr)
        .serve(app.into_make_service())
        .await
        .unwrap();
}

fn credentials_path() -> PathBuf {
    if let Ok(path) = env::var("CLAUDE_CREDENTIALS_PATH") {
        return PathBuf::from(path);
    }
    // Default to per-user Claude Code credentials location.
    // Override with CLAUDE_CREDENTIALS_PATH env var if needed.
    if let Ok(home) = env::var("USERPROFILE").or_else(|_| env::var("HOME")) {
        return PathBuf::from(home).join(".claude").join(".credentials.json");
    }
    PathBuf::from(".claude/.credentials.json")
}

fn load_credentials(path: &Path) -> anyhow::Result<OauthTokens> {
    let content = fs::read_to_string(path)?;
    let file: CredentialsFile = serde_json::from_str(&content)?;
    Ok(file.claude_ai_oauth_tokens)
}

fn save_credentials(path: &Path, tokens: &OauthTokens) -> anyhow::Result<()> {
    let content = fs::read_to_string(path)?;
    let mut file: CredentialsFile = serde_json::from_str(&content)?;
    file.claude_ai_oauth_tokens = tokens.clone();
    let new_content = serde_json::to_string_pretty(&file)?;
    fs::write(path, new_content)?;
    Ok(())
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as i64
}

// ---------- Handler ----------
async fn handle_usage(
    State(state): State<AppState>,
) -> Result<Json<UsageData>, (StatusCode, Json<ErrorData>)> {
    {
        let cache = state.cache.lock().await;
        if let Some(ref c) = *cache {
            if c.fetched_at.elapsed().as_secs() < POLL_INTERVAL_SECS {
                return Ok(Json(c.data.clone()));
            }
        }
    }

    match fetch_usage(&state).await {
        Ok(data) => {
            let mut cache = state.cache.lock().await;
            *cache = Some(CachedUsage {
                data: data.clone(),
                fetched_at: Instant::now(),
            });
            Ok(Json(data))
        }
        Err(e) => {
            eprintln!("fetch_usage error: {e}");
            Err((
                StatusCode::BAD_GATEWAY,
                Json(ErrorData {
                    ok: false,
                    error: e.to_string(),
                }),
            ))
        }
    }
}

// ---------- OAuth refresh ----------
async fn ensure_fresh_token(state: &AppState) -> anyhow::Result<String> {
    let mut creds = state.creds.lock().await;

    if creds.expires_at > now_ms() + REFRESH_LEEWAY_MS {
        return Ok(creds.access_token.clone());
    }

    eprintln!("Access token near expiry, refreshing...");

    let body = serde_json::json!({
        "grant_type": "refresh_token",
        "refresh_token": creds.refresh_token,
        "client_id": CLIENT_ID,
    });

    let mut maybe_refreshed: Option<RefreshResponse> = None;
    let mut last_err: Option<anyhow::Error> = None;
    for url in REFRESH_URLS {
        eprintln!("Trying refresh URL: {url}");
        let result: anyhow::Result<RefreshResponse> = async {
            let resp = state
                .client
                .post(*url)
                .header("Content-Type", "application/json")
                .header("anthropic-beta", "oauth-2025-04-20")
                .json(&body)
                .send()
                .await?
                .error_for_status()?;
            Ok(resp.json().await?)
        }
        .await;

        match result {
            Ok(r) => {
                eprintln!("  success");
                maybe_refreshed = Some(r);
                break;
            }
            Err(e) => {
                eprintln!("  failed: {e}");
                last_err = Some(e);
            }
        }
    }
    let refreshed = maybe_refreshed.ok_or_else(|| {
        last_err.unwrap_or_else(|| anyhow::anyhow!("all refresh URLs failed"))
    })?;

    creds.access_token = refreshed.access_token;
    if let Some(rt) = refreshed.refresh_token {
        creds.refresh_token = rt;
    }
    creds.expires_at = now_ms() + refreshed.expires_in * 1000;

    save_credentials(&state.credentials_path, &creds)?;
    eprintln!("Token refreshed, expires_at = {}", creds.expires_at);

    Ok(creds.access_token.clone())
}

// ---------- Fetch usage from Anthropic ----------
async fn fetch_usage(state: &AppState) -> anyhow::Result<UsageData> {
    let access_token = ensure_fresh_token(state).await?;

    let body = AnthropicRequest {
        model: "claude-haiku-4-5-20251001",
        max_tokens: 1,
        messages: vec![Message {
            role: "user",
            content: "1",
        }],
    };

    let resp = state
        .client
        .post("https://api.anthropic.com/v1/messages")
        .header("authorization", format!("Bearer {access_token}"))
        .header("anthropic-version", "2023-06-01")
        .header("content-type", "application/json")
        .json(&body)
        .send()
        .await?;

    for (key, value) in resp.headers() {
        eprintln!("HEADER: {}: {:?}", key, value);
    }

    let headers = resp.headers();

    let session_pct = parse_pct_header(headers, "anthropic-ratelimit-unified-5h-utilization");
    let session_reset_in =
        parse_reset_header(headers, "anthropic-ratelimit-unified-5h-reset").unwrap_or(0);
    let weekly_pct = parse_pct_header(headers, "anthropic-ratelimit-unified-7d-utilization");
    let weekly_reset_in =
        parse_reset_header(headers, "anthropic-ratelimit-unified-7d-reset").unwrap_or(0);

    let _ = resp.bytes().await;

    Ok(UsageData {
        ok: true,
        session_pct,
        session_reset_in,
        weekly_pct,
        weekly_reset_in,
    })
}

fn parse_pct_header(headers: &reqwest::header::HeaderMap, name: &str) -> u8 {
    headers
        .get(name)
        .and_then(|v| v.to_str().ok())
        .and_then(|s| s.parse::<f64>().ok())
        .map(|f| (f.clamp(0.0, 1.0) * 100.0).round() as u8)
        .unwrap_or(0)
}

fn parse_reset_header(headers: &reqwest::header::HeaderMap, name: &str) -> Option<u64> {
    let val = headers.get(name)?.to_str().ok()?;

    if let Ok(ts) = val.parse::<u64>() {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        return Some(ts.saturating_sub(now));
    }

    let dt = DateTime::parse_from_rfc3339(val).ok()?;
    let reset_ts = dt.timestamp().max(0) as u64;
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    Some(reset_ts.saturating_sub(now))
}
