use std::time::{SystemTime, UNIX_EPOCH};

use crate::{
    status_snapshot::{
        CidStats, DropReasons, EventRecord, EventsResponse, FramePoolStatus, IntegrityStatus,
        IpcStatusResponse, LatencyStats, MailboxStatus, ModuleStatus, ModulesResponse,
        PendingBitmapStatus, PriorityQueueStatus, ReclaimStatus, RingStatus, RouteStatusResponse,
        RouteTableStatus,
    },
    system_reader::{DeviceNodeInfo, NetworkInfo, ResourcesResponse},
};

pub const SCENARIO_ETHERNET_TO_CAN: &str = "ethernet_to_can";

pub fn modules(base: ModulesResponse) -> ModulesResponse {
    let now = now_ms();
    ModulesResponse {
        updated_at_ms: now,
        state: "ok".to_string(),
        modules: vec![
            module(
                "can",
                "online",
                12_288,
                24_576,
                192,
                384,
                last_io(&base, "can"),
                "CAN0 ready, receiving routed RAW_CAN frames",
            ),
            module(
                "ethernet",
                "online",
                2_097_152,
                1_048_576,
                8_192,
                4_096,
                last_io(&base, "ethernet"),
                "Ethernet ingress demo link up",
            ),
            module(
                "wifi",
                "online",
                983_040,
                884_736,
                3_120,
                3_080,
                last_io(&base, "wifi"),
                "Wi-Fi adapter present",
            ),
            module(
                "bluetooth",
                "online",
                65_536,
                61_440,
                256,
                252,
                last_io(&base, "bluetooth"),
                "Bluetooth SPP ready",
            ),
            module(
                "4g",
                "offline",
                0,
                0,
                0,
                0,
                last_io(&base, "4g"),
                "4G module intentionally not connected in demo",
            ),
            module(
                "rs485",
                "online",
                196_608,
                192_512,
                1_536,
                1_520,
                last_io(&base, "rs485"),
                "RS485 adapter present",
            ),
        ],
    }
}

pub fn resources(mut base: ResourcesResponse) -> ResourcesResponse {
    base.networks = vec![NetworkInfo {
        name: "eth0".to_string(),
        state: "up".to_string(),
        rx_bytes: 2_097_152,
        tx_bytes: 1_048_576,
    }];
    base.devices = vec![
        device("can", "CAN", true, &["/sys/class/net/can0", "/dev/can0"]),
        device("ethernet", "Ethernet", true, &["/sys/class/net/eth0"]),
        device("wifi", "Wi-Fi", true, &["/sys/class/net/wlan0/wireless"]),
        device(
            "bluetooth",
            "Bluetooth",
            true,
            &["/sys/class/bluetooth/hci0"],
        ),
        device("4g", "4G / Cellular", false, &[]),
        device("rs485", "RS485 / Serial", true, &["/dev/ttyS1"]),
    ];
    base
}

pub fn ipc_status(_base: IpcStatusResponse) -> IpcStatusResponse {
    let tick = tick();
    IpcStatusResponse {
        updated_at_ms: now_ms(),
        state: "ok".to_string(),
        rtos_online: true,
        heartbeat_ms: 1_000,
        frame_pool: FramePoolStatus {
            capacity: 256,
            used: 3 + tick % 3,
            high_watermark: 18,
            full_count: 0,
            allocated: 4_096 + tick,
            released: 4_093 + tick,
            pending_reclaim: 0,
            leaked_suspect: 0,
        },
        rx_rings: vec![RingStatus {
            interface: "ethernet".to_string(),
            capacity: 64,
            used: 1,
            high_watermark: 4,
            full_count: 0,
        }],
        tx_rings: vec![RingStatus {
            interface: "can".to_string(),
            capacity: 64,
            used: 1,
            high_watermark: 4,
            full_count: 0,
        }],
        pending_bitmap: PendingBitmapStatus {
            rx: "0x02".to_string(),
            tx: "0x01".to_string(),
        },
        mailbox: MailboxStatus {
            rx_doorbell_count: 512 + tick,
            tx_doorbell_count: 512 + tick,
            notify_fail_count: 0,
            periodic_drain_count: 0,
        },
        integrity: IntegrityStatus {
            descriptor_crc_error_count: 0,
            epoch_mismatch_count: 0,
            cache_sync_error_count: 0,
        },
        reclaim: ReclaimStatus {
            heartbeat_consumed: 32 + tick,
            invalid_frame_reclaimed: 0,
            no_route_reclaimed: 0,
            ttl_expired_reclaimed: 0,
            epoch_mismatch_reclaimed: 0,
            reclaim_ring_used: 0,
            reclaim_ack_count: 512 + tick,
        },
    }
}

pub fn route_status(_base: RouteStatusResponse) -> RouteStatusResponse {
    let tick = tick();
    let routed = 2_048 + tick;
    RouteStatusResponse {
        updated_at_ms: now_ms(),
        state: "ok".to_string(),
        route_table: RouteTableStatus {
            version: 3,
            epoch: 42,
            source: "compiled_config".to_string(),
            active_entries: 6,
        },
        priority_queues: vec![
            queue(0, 0, 16, 128 + tick / 4, 0, 3),
            queue(1, 1, 32, routed, 0, 4),
            queue(2, 0, 64, 640 + tick / 2, 0, 5),
            queue(3, 0, 64, 256 + tick / 3, 0, 6),
        ],
        cid_stats: CidStats {
            routed_frames: routed,
            heartbeat_consumed: 32 + tick,
            no_route: 0,
            invalid_cid: 0,
            reserved_cid: 0,
            broadcast_frames: 0,
        },
        drop_reasons: DropReasons {
            invalid_length: 0,
            invalid_type: 0,
            ttl_expired: 0,
            frame_pool_full: 0,
            rx_ring_full: 0,
            tx_ring_full: 0,
            target_interface_offline: 0,
            auth_failed: 0,
            integrity_failed: 0,
            replay_dropped: 0,
        },
        latency: LatencyStats {
            rx_ring_to_tx_ring_max_ms: 4,
            rx_ring_to_tx_ring_avg_ms: 2,
            linux_egress_max_ms: 3,
            end_to_end_max_ms: 7,
        },
    }
}

pub fn events(_base: EventsResponse, limit: usize) -> EventsResponse {
    let now = now_ms();
    let mut events = vec![
        event(
            now.saturating_sub(5_000),
            "web",
            "demo mode active",
            "scenario=ethernet_to_can readonly=true",
        ),
        event(
            now.saturating_sub(4_000),
            "adapter",
            "ethernet received",
            "eth0 rx_bytes=48 frame_id=demo-eth-can-001",
        ),
        event(
            now.saturating_sub(3_000),
            "adapter",
            "anyMSG decoded",
            "msg_length=48 type=RAW_CAN source_cid=0x40000001 destination_cid=0x20000011",
        ),
        event(
            now.saturating_sub(2_000),
            "ipc",
            "descriptor committed",
            "ring=ETH0_RX_RING frame_id=demo-eth-can-001 pending_bitmap=0x02",
        ),
        event(
            now.saturating_sub(1_000),
            "router",
            "route selected",
            "destination_cid=0x20000011 target=CAN0_TX_RING priority=1",
        ),
        event(
            now,
            "adapter",
            "can send complete",
            "RAW_CAN can_id=0x211 payload_len=8 fragments=1",
        ),
    ];
    let limit = limit.clamp(1, 500);
    if events.len() > limit {
        events = events.split_off(events.len() - limit);
    }
    EventsResponse {
        events,
        parse_error_count: 0,
    }
}

fn module(
    name: &str,
    status: &str,
    rx_bytes: u64,
    tx_bytes: u64,
    rx_frames: u64,
    tx_frames: u64,
    last_io: (u64, u64),
    message: &str,
) -> ModuleStatus {
    ModuleStatus {
        name: name.to_string(),
        status: status.to_string(),
        rx_bytes,
        tx_bytes,
        rx_frames,
        tx_frames,
        decode_error_count: 0,
        fragment_drop_count: 0,
        reassemble_timeout_count: 0,
        crc_error_count: 0,
        send_fail_count: 0,
        interface_offline_count: if name == "4g" { 1 } else { 0 },
        last_rx_ms: last_io.0,
        last_tx_ms: last_io.1,
        last_error: if name == "4g" {
            "module not connected".to_string()
        } else {
            "none".to_string()
        },
        message: message.to_string(),
    }
}

fn last_io(base: &ModulesResponse, name: &str) -> (u64, u64) {
    base.modules
        .iter()
        .find(|module| module.name == name)
        .map(|module| (module.last_rx_ms, module.last_tx_ms))
        .unwrap_or_default()
}

fn device(key: &str, label: &str, present: bool, matched_paths: &[&str]) -> DeviceNodeInfo {
    let checked_paths = if matched_paths.is_empty() {
        match key {
            "4g" => vec![
                "/sys/class/net/wwan*".to_string(),
                "/sys/class/net/ppp*".to_string(),
                "/dev/cdc-wdm*".to_string(),
                "/dev/ttyUSB*".to_string(),
            ],
            _ => Vec::new(),
        }
    } else {
        matched_paths
            .iter()
            .map(|path| (*path).to_string())
            .collect()
    };

    DeviceNodeInfo {
        key: key.to_string(),
        label: label.to_string(),
        state: if present { "present" } else { "missing" }.to_string(),
        present: Some(present),
        checked_paths,
        matched_paths: matched_paths
            .iter()
            .map(|path| (*path).to_string())
            .collect(),
    }
}

fn queue(
    priority: u64,
    queued: u64,
    capacity: u64,
    routed_frames: u64,
    dropped_frames: u64,
    max_latency_ms: u64,
) -> PriorityQueueStatus {
    PriorityQueueStatus {
        priority,
        queued,
        capacity,
        routed_frames,
        dropped_frames,
        max_latency_ms,
    }
}

fn event(timestamp_ms: u64, source: &str, message: &str, detail: &str) -> EventRecord {
    EventRecord {
        timestamp_ms,
        level: "info".to_string(),
        source: source.to_string(),
        message: message.to_string(),
        detail: detail.to_string(),
    }
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis() as u64)
        .unwrap_or(0)
}

fn tick() -> u64 {
    now_ms() / 1_000 % 1_000
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn demo_modules_mark_only_4g_offline() {
        let response = modules(ModulesResponse {
            updated_at_ms: 0,
            state: "unknown".to_string(),
            modules: Vec::new(),
        });

        assert_eq!(response.state, "ok");
        assert_eq!(response.modules.len(), 6);
        assert_eq!(
            response
                .modules
                .iter()
                .filter(|module| module.status == "online")
                .count(),
            5
        );
        let cellular = response
            .modules
            .iter()
            .find(|module| module.name == "4g")
            .unwrap();
        assert_eq!(cellular.status, "offline");
    }

    #[test]
    fn demo_modules_preserve_real_last_io_by_module_name() {
        let response = modules(ModulesResponse {
            updated_at_ms: 0,
            state: "ok".to_string(),
            modules: vec![
                module_snapshot("can", 1_001, 1_002),
                module_snapshot("ethernet", 2_001, 2_002),
                module_snapshot("4g", 4_001, 4_002),
            ],
        });

        let can = response
            .modules
            .iter()
            .find(|module| module.name == "can")
            .unwrap();
        assert_eq!((can.last_rx_ms, can.last_tx_ms), (1_001, 1_002));

        let ethernet = response
            .modules
            .iter()
            .find(|module| module.name == "ethernet")
            .unwrap();
        assert_eq!(
            (ethernet.last_rx_ms, ethernet.last_tx_ms),
            (2_001, 2_002)
        );

        let cellular = response
            .modules
            .iter()
            .find(|module| module.name == "4g")
            .unwrap();
        assert_eq!(cellular.status, "offline");
        assert_eq!(
            (cellular.last_rx_ms, cellular.last_tx_ms),
            (4_001, 4_002)
        );

        let wifi = response
            .modules
            .iter()
            .find(|module| module.name == "wifi")
            .unwrap();
        assert_eq!((wifi.last_rx_ms, wifi.last_tx_ms), (0, 0));
    }

    #[test]
    fn demo_events_replace_real_errors() {
        let response = events(
            EventsResponse {
                events: vec![EventRecord {
                    timestamp_ms: 1,
                    level: "error".to_string(),
                    source: "router".to_string(),
                    message: "real error".to_string(),
                    detail: "should be replaced".to_string(),
                }],
                parse_error_count: 2,
            },
            50,
        );

        assert_eq!(response.parse_error_count, 0);
        assert!(response.events.iter().all(|event| event.level == "info"));
        assert!(response
            .events
            .iter()
            .any(|event| event.detail.contains("CAN0_TX_RING")));
    }

    #[test]
    fn demo_route_has_no_drops() {
        let response = route_status(RouteStatusResponse {
            updated_at_ms: 0,
            state: "unknown".to_string(),
            route_table: RouteTableStatus {
                version: 0,
                epoch: 0,
                source: "unknown".to_string(),
                active_entries: 0,
            },
            priority_queues: Vec::new(),
            cid_stats: CidStats::default(),
            drop_reasons: DropReasons::default(),
            latency: LatencyStats::default(),
        });

        assert_eq!(response.state, "ok");
        assert_eq!(response.drop_reasons.invalid_length, 0);
        assert_eq!(response.cid_stats.no_route, 0);
        assert_eq!(response.drop_reasons.auth_failed, 0);
        assert_eq!(response.drop_reasons.replay_dropped, 0);
    }

    fn module_snapshot(name: &str, last_rx_ms: u64, last_tx_ms: u64) -> ModuleStatus {
        ModuleStatus {
            name: name.to_string(),
            status: "online".to_string(),
            rx_bytes: 0,
            tx_bytes: 0,
            rx_frames: 0,
            tx_frames: 0,
            decode_error_count: 0,
            fragment_drop_count: 0,
            reassemble_timeout_count: 0,
            crc_error_count: 0,
            send_fail_count: 0,
            interface_offline_count: 0,
            last_rx_ms,
            last_tx_ms,
            last_error: "none".to_string(),
            message: String::new(),
        }
    }
}
