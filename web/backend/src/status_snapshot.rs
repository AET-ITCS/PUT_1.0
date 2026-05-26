use std::{
    fs,
    io::ErrorKind,
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};

use serde::{Deserialize, Serialize};
use tracing::warn;

const EPOCH_MS_THRESHOLD: u64 = 1_000_000_000_000;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModuleStatus {
    #[serde(default = "unknown")]
    pub name: String,
    #[serde(default = "unknown")]
    pub status: String,
    #[serde(default)]
    pub rx_bytes: u64,
    #[serde(default)]
    pub tx_bytes: u64,
    #[serde(default)]
    pub rx_frames: u64,
    #[serde(default)]
    pub tx_frames: u64,
    #[serde(default)]
    pub decode_error_count: u64,
    #[serde(default)]
    pub fragment_drop_count: u64,
    #[serde(default)]
    pub reassemble_timeout_count: u64,
    #[serde(default)]
    pub crc_error_count: u64,
    #[serde(default)]
    pub send_fail_count: u64,
    #[serde(default)]
    pub interface_offline_count: u64,
    #[serde(default)]
    pub last_rx_ms: u64,
    #[serde(default)]
    pub last_tx_ms: u64,
    #[serde(default = "none")]
    pub last_error: String,
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
pub struct IpcStatusResponse {
    #[serde(default)]
    pub updated_at_ms: u64,
    #[serde(default)]
    pub state: String,
    #[serde(default)]
    pub rtos_online: bool,
    #[serde(default)]
    pub heartbeat_ms: u64,
    #[serde(default)]
    pub frame_pool: FramePoolStatus,
    #[serde(default)]
    pub rx_rings: Vec<RingStatus>,
    #[serde(default)]
    pub tx_rings: Vec<RingStatus>,
    #[serde(default)]
    pub pending_bitmap: PendingBitmapStatus,
    #[serde(default)]
    pub mailbox: MailboxStatus,
    #[serde(default)]
    pub integrity: IntegrityStatus,
    #[serde(default)]
    pub reclaim: ReclaimStatus,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct FramePoolStatus {
    #[serde(default)]
    pub capacity: u64,
    #[serde(default)]
    pub used: u64,
    #[serde(default)]
    pub high_watermark: u64,
    #[serde(default)]
    pub full_count: u64,
    #[serde(default)]
    pub allocated: u64,
    #[serde(default)]
    pub released: u64,
    #[serde(default)]
    pub pending_reclaim: u64,
    #[serde(default)]
    pub leaked_suspect: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RingStatus {
    #[serde(default = "unknown")]
    pub interface: String,
    #[serde(default)]
    pub capacity: u64,
    #[serde(default)]
    pub used: u64,
    #[serde(default)]
    pub high_watermark: u64,
    #[serde(default)]
    pub full_count: u64,
}

impl Default for RingStatus {
    fn default() -> Self {
        Self {
            interface: "unknown".to_string(),
            capacity: 0,
            used: 0,
            high_watermark: 0,
            full_count: 0,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PendingBitmapStatus {
    #[serde(default = "zero_hex")]
    pub rx: String,
    #[serde(default = "zero_hex")]
    pub tx: String,
}

impl Default for PendingBitmapStatus {
    fn default() -> Self {
        Self {
            rx: zero_hex(),
            tx: zero_hex(),
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct MailboxStatus {
    #[serde(default)]
    pub rx_doorbell_count: u64,
    #[serde(default)]
    pub tx_doorbell_count: u64,
    #[serde(default)]
    pub notify_fail_count: u64,
    #[serde(default)]
    pub periodic_drain_count: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct IntegrityStatus {
    #[serde(default)]
    pub descriptor_crc_error_count: u64,
    #[serde(default)]
    pub epoch_mismatch_count: u64,
    #[serde(default)]
    pub cache_sync_error_count: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ReclaimStatus {
    #[serde(default)]
    pub heartbeat_consumed: u64,
    #[serde(default)]
    pub invalid_frame_reclaimed: u64,
    #[serde(default)]
    pub no_route_reclaimed: u64,
    #[serde(default)]
    pub ttl_expired_reclaimed: u64,
    #[serde(default)]
    pub epoch_mismatch_reclaimed: u64,
    #[serde(default)]
    pub reclaim_ring_used: u64,
    #[serde(default)]
    pub reclaim_ack_count: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RouteStatusResponse {
    #[serde(default)]
    pub updated_at_ms: u64,
    #[serde(default)]
    pub state: String,
    #[serde(default)]
    pub route_table: RouteTableStatus,
    #[serde(default)]
    pub priority_queues: Vec<PriorityQueueStatus>,
    #[serde(default)]
    pub cid_stats: CidStats,
    #[serde(default)]
    pub drop_reasons: DropReasons,
    #[serde(default)]
    pub latency: LatencyStats,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RouteTableStatus {
    #[serde(default)]
    pub version: u64,
    #[serde(default)]
    pub epoch: u64,
    #[serde(default = "unknown")]
    pub source: String,
    #[serde(default)]
    pub active_entries: u64,
}

impl Default for RouteTableStatus {
    fn default() -> Self {
        Self {
            version: 0,
            epoch: 0,
            source: "unknown".to_string(),
            active_entries: 0,
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct PriorityQueueStatus {
    #[serde(default)]
    pub priority: u64,
    #[serde(default)]
    pub queued: u64,
    #[serde(default)]
    pub capacity: u64,
    #[serde(default)]
    pub routed_frames: u64,
    #[serde(default)]
    pub dropped_frames: u64,
    #[serde(default)]
    pub max_latency_ms: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct CidStats {
    #[serde(default)]
    pub routed_frames: u64,
    #[serde(default)]
    pub heartbeat_consumed: u64,
    #[serde(default)]
    pub no_route: u64,
    #[serde(default)]
    pub invalid_cid: u64,
    #[serde(default)]
    pub reserved_cid: u64,
    #[serde(default)]
    pub broadcast_frames: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DropReasons {
    #[serde(default)]
    pub invalid_length: u64,
    #[serde(default)]
    pub invalid_type: u64,
    #[serde(default)]
    pub ttl_expired: u64,
    #[serde(default)]
    pub frame_pool_full: u64,
    #[serde(default)]
    pub rx_ring_full: u64,
    #[serde(default)]
    pub tx_ring_full: u64,
    #[serde(default)]
    pub target_interface_offline: u64,
    #[serde(default)]
    pub auth_failed: u64,
    #[serde(default)]
    pub integrity_failed: u64,
    #[serde(default)]
    pub replay_dropped: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct LatencyStats {
    #[serde(default)]
    pub rx_ring_to_tx_ring_max_ms: u64,
    #[serde(default)]
    pub rx_ring_to_tx_ring_avg_ms: u64,
    #[serde(default)]
    pub linux_egress_max_ms: u64,
    #[serde(default)]
    pub end_to_end_max_ms: u64,
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
        state: state_with_freshness(file.state, &freshness),
        modules: file.modules,
    }
}

pub fn read_ipc_status(status_dir: &Path, stale_ms: u64) -> IpcStatusResponse {
    let path = status_dir.join("ipc_status.json");
    let Ok(mut status) = read_json::<IpcStatusResponse>(&path) else {
        return IpcStatusResponse::unknown();
    };
    status.state = state_with_freshness(
        Some(status.state),
        &classify_snapshot(status.updated_at_ms, stale_ms),
    );
    status
}

pub fn read_route_status(status_dir: &Path, stale_ms: u64) -> RouteStatusResponse {
    let path = status_dir.join("route_status.json");
    let Ok(mut status) = read_json::<RouteStatusResponse>(&path) else {
        return RouteStatusResponse::unknown();
    };
    status.state = state_with_freshness(
        Some(status.state),
        &classify_snapshot(status.updated_at_ms, stale_ms),
    );
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

fn read_json<T: for<'de> Deserialize<'de>>(path: &Path) -> Result<T, ()> {
    let text = fs::read_to_string(path).map_err(|err| {
        if err.kind() != ErrorKind::NotFound {
            warn!(path = %path.display(), error = %err, "snapshot read failed");
        }
    })?;
    serde_json::from_str(&text).map_err(|err| {
        warn!(path = %path.display(), error = %err, "snapshot parse failed");
    })
}

fn state_with_freshness(state: Option<String>, freshness: &str) -> String {
    match freshness {
        "ok" => state
            .filter(|value| !value.trim().is_empty())
            .unwrap_or_else(|| "ok".to_string()),
        other => other.to_string(),
    }
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

fn none() -> String {
    "none".to_string()
}

fn zero_hex() -> String {
    "0x00".to_string()
}

impl IpcStatusResponse {
    fn unknown() -> Self {
        Self {
            updated_at_ms: 0,
            state: "unknown".to_string(),
            rtos_online: false,
            heartbeat_ms: 0,
            frame_pool: FramePoolStatus::default(),
            rx_rings: Vec::new(),
            tx_rings: Vec::new(),
            pending_bitmap: PendingBitmapStatus::default(),
            mailbox: MailboxStatus::default(),
            integrity: IntegrityStatus::default(),
            reclaim: ReclaimStatus::default(),
        }
    }
}

impl RouteStatusResponse {
    fn unknown() -> Self {
        Self {
            updated_at_ms: 0,
            state: "unknown".to_string(),
            route_table: RouteTableStatus::default(),
            priority_queues: Vec::new(),
            cid_stats: CidStats::default(),
            drop_reasons: DropReasons::default(),
            latency: LatencyStats::default(),
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
    fn modules_snapshot_uses_document_fields_and_defaults() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("modules.json"),
            format!(
                r#"{{
                    "updated_at_ms": {},
                    "modules": [
                        {{
                            "name": "can",
                            "status": "online",
                            "rx_bytes": 4096,
                            "tx_frames": 12
                        }}
                    ]
                }}"#,
                wall_clock_ms().unwrap()
            ),
        )
        .unwrap();

        let response = read_modules(dir.path(), 5_000);
        assert_eq!(response.state, "ok");
        assert_eq!(response.modules[0].name, "can");
        assert_eq!(response.modules[0].rx_bytes, 4096);
        assert_eq!(response.modules[0].tx_frames, 12);
        assert_eq!(response.modules[0].crc_error_count, 0);
        assert_eq!(response.modules[0].last_error, "none");
    }

    #[test]
    fn missing_ipc_snapshot_returns_unknown_v2_shape() {
        let dir = tempfile::tempdir().unwrap();
        let response = read_ipc_status(dir.path(), 5_000);
        assert_eq!(response.state, "unknown");
        assert!(!response.rtos_online);
        assert_eq!(response.frame_pool.capacity, 0);
        assert!(response.rx_rings.is_empty());
    }

    #[test]
    fn ipc_snapshot_uses_document_fields_and_defaults() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("ipc_status.json"),
            format!(
                r#"{{
                    "updated_at_ms": {},
                    "rtos_online": true,
                    "frame_pool": {{"capacity": 256, "used": 12}},
                    "rx_rings": [{{"interface": "can", "capacity": 64, "used": 2}}],
                    "pending_bitmap": {{"rx": "0x01", "tx": "0x20"}}
                }}"#,
                wall_clock_ms().unwrap()
            ),
        )
        .unwrap();

        let response = read_ipc_status(dir.path(), 5_000);
        assert_eq!(response.state, "ok");
        assert!(response.rtos_online);
        assert_eq!(response.frame_pool.capacity, 256);
        assert_eq!(response.frame_pool.used, 12);
        assert_eq!(response.rx_rings[0].interface, "can");
        assert_eq!(response.mailbox.notify_fail_count, 0);
    }

    #[test]
    fn missing_route_snapshot_returns_unknown_v2_shape() {
        let dir = tempfile::tempdir().unwrap();
        let response = read_route_status(dir.path(), 5_000);
        assert_eq!(response.state, "unknown");
        assert_eq!(response.route_table.source, "unknown");
        assert_eq!(response.drop_reasons.auth_failed, 0);
    }

    #[test]
    fn route_snapshot_can_be_ok_stale_or_unknown() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("route_status.json"),
            format!(
                r#"{{
                    "updated_at_ms": {},
                    "state": "ok",
                    "route_table": {{"version": 3, "epoch": 12, "source": "compiled_config", "active_entries": 6}},
                    "priority_queues": [{{"priority": 0, "queued": 1, "capacity": 16}}],
                    "cid_stats": {{"routed_frames": 2048, "no_route": 1}},
                    "drop_reasons": {{"auth_failed": 2}},
                    "latency": {{"end_to_end_max_ms": 30}}
                }}"#,
                wall_clock_ms().unwrap()
            ),
        )
        .unwrap();

        let ok = read_route_status(dir.path(), 5_000);
        assert_eq!(ok.state, "ok");
        assert_eq!(ok.route_table.active_entries, 6);
        assert_eq!(ok.priority_queues[0].capacity, 16);
        assert_eq!(ok.cid_stats.no_route, 1);
        assert_eq!(ok.drop_reasons.auth_failed, 2);
        assert_eq!(ok.latency.end_to_end_max_ms, 30);

        fs::write(
            dir.path().join("route_status.json"),
            r#"{"updated_at_ms":1,"route_table":{"version":1}}"#,
        )
        .unwrap();
        assert_eq!(read_route_status(dir.path(), 0).state, "stale");

        fs::write(dir.path().join("route_status.json"), "{not-json").unwrap();
        assert_eq!(read_route_status(dir.path(), 5_000).state, "unknown");
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
            "{\"timestamp_ms\":1,\"level\":\"warn\",\"source\":\"ipc\",\"message\":\"one\"}\nnot-json\n{\"timestamp_ms\":2,\"level\":\"error\",\"source\":\"router\",\"message\":\"two\"}\n",
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
