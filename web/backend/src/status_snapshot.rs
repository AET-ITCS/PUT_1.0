use std::{
    fs,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use serde::{Deserialize, Serialize};
use tracing::warn;

const EPOCH_MS_THRESHOLD: u64 = 1_000_000_000_000;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModuleStatus {
    pub name: String,
    pub status: String,
    #[serde(default)]
    pub rx_count: u64,
    #[serde(default)]
    pub tx_count: u64,
    #[serde(default)]
    pub error_count: u64,
    #[serde(default)]
    pub last_seen_ms: u64,
    #[serde(default)]
    pub message: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct ModulesResponse {
    pub updated_at_ms: u64,
    pub state: String,
    pub modules: Vec<ModuleStatus>,
}

#[derive(Debug, Deserialize)]
struct ModulesFile {
    #[serde(default)]
    updated_at_ms: u64,
    state: Option<String>,
    #[serde(default)]
    modules: Vec<ModuleStatus>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CanStatusResponse {
    #[serde(default)]
    pub updated_at_ms: u64,
    #[serde(default = "unknown")]
    pub state: String,
    #[serde(default = "unknown")]
    pub bus_state: String,
    #[serde(default)]
    pub tx_count: u64,
    #[serde(default)]
    pub rx_count: u64,
    #[serde(default)]
    pub error_count: u64,
    #[serde(default)]
    pub drop_count: u64,
    #[serde(default = "unknown")]
    pub last_error: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IpcStatusResponse {
    #[serde(default)]
    pub updated_at_ms: u64,
    #[serde(default = "unknown")]
    pub state: String,
    #[serde(default)]
    pub online: bool,
    #[serde(default)]
    pub heartbeat_ms: u64,
    #[serde(default)]
    pub tx_ring_used: u64,
    #[serde(default)]
    pub rx_ring_used: u64,
    #[serde(default)]
    pub timeout_count: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EventRecord {
    #[serde(default)]
    pub timestamp_ms: u64,
    #[serde(default = "unknown")]
    pub level: String,
    #[serde(default = "unknown")]
    pub source: String,
    #[serde(default)]
    pub message: String,
    #[serde(default)]
    pub detail: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct EventsResponse {
    pub events: Vec<EventRecord>,
    pub parse_error_count: usize,
}

pub fn read_modules(status_dir: &Path, stale_ms: u64) -> ModulesResponse {
    let path = status_dir.join("modules.json");
    let Ok(file) = read_json::<ModulesFile>(&path) else {
        return ModulesResponse {
            updated_at_ms: 0,
            state: "unknown".to_string(),
            modules: Vec::new(),
        };
    };

    let freshness = classify_snapshot(file.updated_at_ms, stale_ms);
    ModulesResponse {
        updated_at_ms: file.updated_at_ms,
        state: match freshness.as_str() {
            "ok" => file.state.unwrap_or_else(|| "ok".to_string()),
            other => other.to_string(),
        },
        modules: file.modules,
    }
}

pub fn read_can_status(status_dir: &Path, stale_ms: u64) -> CanStatusResponse {
    let path = status_dir.join("can_status.json");
    let Ok(mut status) = read_json::<CanStatusResponse>(&path) else {
        return CanStatusResponse::unknown();
    };
    status.state = classify_snapshot(status.updated_at_ms, stale_ms);
    status
}

pub fn read_ipc_status(status_dir: &Path, stale_ms: u64) -> IpcStatusResponse {
    let path = status_dir.join("ipc_status.json");
    let Ok(mut status) = read_json::<IpcStatusResponse>(&path) else {
        return IpcStatusResponse::unknown();
    };
    status.state = classify_snapshot(status.updated_at_ms, stale_ms);
    status
}

pub fn read_events(status_dir: &Path, limit: usize) -> EventsResponse {
    let path = status_dir.join("events.jsonl");
    let limit = limit.clamp(1, 500);
    let Ok(text) = fs::read_to_string(&path) else {
        return EventsResponse {
            events: Vec::new(),
            parse_error_count: 0,
        };
    };

    let mut parse_error_count = 0;
    let mut events = Vec::new();

    for line in text.lines().rev() {
        if events.len() >= limit {
            break;
        }
        if line.trim().is_empty() {
            continue;
        }
        match serde_json::from_str::<EventRecord>(line) {
            Ok(event) => events.push(event),
            Err(err) => {
                parse_error_count += 1;
                warn!(path = %path.display(), error = %err, "skipping broken event line");
            }
        }
    }

    events.reverse();
    EventsResponse {
        events,
        parse_error_count,
    }
}

fn read_json<T: for<'de> Deserialize<'de>>(path: &PathBuf) -> Result<T, ()> {
    let text = fs::read_to_string(path).map_err(|err| {
        warn!(path = %path.display(), error = %err, "snapshot read failed");
    })?;
    serde_json::from_str(&text).map_err(|err| {
        warn!(path = %path.display(), error = %err, "snapshot parse failed");
    })
}

fn classify_snapshot(updated_at_ms: u64, stale_ms: u64) -> String {
    if updated_at_ms == 0 {
        return "unknown".to_string();
    }

    let now_ms = if updated_at_ms >= EPOCH_MS_THRESHOLD {
        wall_clock_ms()
    } else {
        uptime_ms()
    };

    match now_ms {
        Some(now) if now.saturating_sub(updated_at_ms) > stale_ms && now >= updated_at_ms => {
            "stale".to_string()
        }
        Some(_) => "ok".to_string(),
        None => "unknown".to_string(),
    }
}

fn uptime_ms() -> Option<u64> {
    let text = fs::read_to_string("/proc/uptime").ok()?;
    parse_uptime_ms(&text)
}

fn wall_clock_ms() -> Option<u64> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .ok()
        .map(|duration| duration.as_millis() as u64)
}

fn parse_uptime_ms(text: &str) -> Option<u64> {
    let seconds = text.split_whitespace().next()?.parse::<f64>().ok()?;
    Some((seconds * 1000.0) as u64)
}

fn unknown() -> String {
    "unknown".to_string()
}

impl CanStatusResponse {
    fn unknown() -> Self {
        Self {
            updated_at_ms: 0,
            state: "unknown".to_string(),
            bus_state: "unknown".to_string(),
            tx_count: 0,
            rx_count: 0,
            error_count: 0,
            drop_count: 0,
            last_error: "unknown".to_string(),
        }
    }
}

impl IpcStatusResponse {
    fn unknown() -> Self {
        Self {
            updated_at_ms: 0,
            state: "unknown".to_string(),
            online: false,
            heartbeat_ms: 0,
            tx_ring_used: 0,
            rx_ring_used: 0,
            timeout_count: 0,
        }
    }
}

#[cfg(test)]
mod tests {
    use std::fs;

    use super::*;

    #[test]
    fn missing_modules_snapshot_returns_unknown() {
        let dir = tempfile::tempdir().unwrap();
        let response = read_modules(dir.path(), 5_000);
        assert_eq!(response.state, "unknown");
        assert!(response.modules.is_empty());
    }

    #[test]
    fn broken_modules_snapshot_returns_unknown() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(dir.path().join("modules.json"), "{not-json").unwrap();
        let response = read_modules(dir.path(), 5_000);
        assert_eq!(response.state, "unknown");
    }

    #[test]
    fn boot_time_snapshot_can_be_stale() {
        assert_eq!(classify_snapshot(1, 0), "stale");
    }

    #[test]
    fn parses_recent_events_and_skips_broken_lines() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("events.jsonl"),
            "{\"timestamp_ms\":1,\"level\":\"warn\",\"source\":\"ipc\",\"message\":\"one\"}\nnot-json\n{\"timestamp_ms\":2,\"level\":\"error\",\"source\":\"can\",\"message\":\"two\"}\n",
        )
        .unwrap();

        let response = read_events(dir.path(), 10);
        assert_eq!(response.events.len(), 2);
        assert_eq!(response.parse_error_count, 1);
        assert_eq!(response.events[1].message, "two");
    }

    #[test]
    fn parses_uptime_to_ms() {
        assert_eq!(parse_uptime_ms("12.34 56.78\n"), Some(12_340));
    }
}
