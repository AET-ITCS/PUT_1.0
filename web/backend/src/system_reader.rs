use std::{
    collections::HashMap,
    ffi::CString,
    fs,
    os::unix::ffi::OsStrExt,
    path::{Path, PathBuf},
};

use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
pub struct ResourcesResponse {
    pub cpu: CpuInfo,
    pub memory: MemoryInfo,
    pub uptime: UptimeInfo,
    pub disks: Vec<DiskInfo>,
    pub networks: Vec<NetworkInfo>,
}

#[derive(Debug, Clone, Serialize)]
pub struct CpuInfo {
    pub state: String,
    pub usage_percent: Option<f64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct MemoryInfo {
    pub state: String,
    pub total_kb: Option<u64>,
    pub available_kb: Option<u64>,
    pub used_kb: Option<u64>,
    pub usage_percent: Option<f64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct UptimeInfo {
    pub state: String,
    pub uptime_seconds: Option<u64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct DiskInfo {
    pub mount_point: String,
    pub filesystem: String,
    pub total_bytes: u64,
    pub available_bytes: u64,
    pub used_bytes: u64,
    pub usage_percent: f64,
}

#[derive(Debug, Clone, Serialize)]
pub struct NetworkInfo {
    pub name: String,
    pub state: String,
    pub rx_bytes: u64,
    pub tx_bytes: u64,
}

pub fn read_resources() -> ResourcesResponse {
    ResourcesResponse {
        cpu: read_cpu(),
        memory: read_memory(),
        uptime: read_uptime(),
        disks: read_disks(),
        networks: read_networks(),
    }
}

fn read_cpu() -> CpuInfo {
    match fs::read_to_string("/proc/stat")
        .ok()
        .and_then(|text| parse_cpu_usage(&text))
    {
        Some(usage_percent) => CpuInfo {
            state: "ok".to_string(),
            usage_percent: Some(usage_percent),
        },
        None => CpuInfo {
            state: "unknown".to_string(),
            usage_percent: None,
        },
    }
}

fn read_memory() -> MemoryInfo {
    match fs::read_to_string("/proc/meminfo")
        .ok()
        .and_then(|text| parse_memory(&text))
    {
        Some(info) => info,
        None => MemoryInfo {
            state: "unknown".to_string(),
            total_kb: None,
            available_kb: None,
            used_kb: None,
            usage_percent: None,
        },
    }
}

fn read_uptime() -> UptimeInfo {
    match fs::read_to_string("/proc/uptime")
        .ok()
        .and_then(|text| parse_uptime_seconds(&text))
    {
        Some(uptime_seconds) => UptimeInfo {
            state: "ok".to_string(),
            uptime_seconds: Some(uptime_seconds),
        },
        None => UptimeInfo {
            state: "unknown".to_string(),
            uptime_seconds: None,
        },
    }
}

fn read_disks() -> Vec<DiskInfo> {
    let mounts = fs::read_to_string("/proc/mounts")
        .ok()
        .map(|text| parse_mounts(&text))
        .unwrap_or_else(|| vec![PathBuf::from("/")]);

    let mut disks = Vec::new();
    for mount in mounts {
        if let Some(info) = stat_mount(&mount) {
            disks.push(info);
        }
    }
    disks
}

fn read_networks() -> Vec<NetworkInfo> {
    let states = read_network_states();
    fs::read_to_string("/proc/net/dev")
        .ok()
        .map(|text| parse_networks(&text, &states))
        .unwrap_or_default()
}

fn parse_cpu_usage(text: &str) -> Option<f64> {
    let line = text.lines().find(|line| line.starts_with("cpu "))?;
    let values: Vec<u64> = line
        .split_whitespace()
        .skip(1)
        .filter_map(|value| value.parse::<u64>().ok())
        .collect();
    if values.len() < 4 {
        return None;
    }
    let idle = values.get(3).copied().unwrap_or(0) + values.get(4).copied().unwrap_or(0);
    let total: u64 = values.iter().sum();
    if total == 0 {
        return None;
    }
    Some(round2(
        ((total.saturating_sub(idle)) as f64 / total as f64) * 100.0,
    ))
}

fn parse_memory(text: &str) -> Option<MemoryInfo> {
    let mut total = None;
    let mut available = None;

    for line in text.lines() {
        if let Some(value) = line.strip_prefix("MemTotal:") {
            total = value.split_whitespace().next()?.parse::<u64>().ok();
        } else if let Some(value) = line.strip_prefix("MemAvailable:") {
            available = value.split_whitespace().next()?.parse::<u64>().ok();
        }
    }

    let total = total?;
    let available = available?;
    let used = total.saturating_sub(available);
    Some(MemoryInfo {
        state: "ok".to_string(),
        total_kb: Some(total),
        available_kb: Some(available),
        used_kb: Some(used),
        usage_percent: Some(round2((used as f64 / total as f64) * 100.0)),
    })
}

fn parse_uptime_seconds(text: &str) -> Option<u64> {
    Some(text.split_whitespace().next()?.parse::<f64>().ok()? as u64)
}

fn parse_mounts(text: &str) -> Vec<PathBuf> {
    let mut mounts = Vec::new();
    for line in text.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() < 3 || is_virtual_fs(parts[2]) {
            continue;
        }
        let mount = PathBuf::from(parts[1].replace("\\040", " "));
        if !mounts.contains(&mount) {
            mounts.push(mount);
        }
    }
    if mounts.is_empty() {
        mounts.push(PathBuf::from("/"));
    }
    mounts
}

fn is_virtual_fs(fs_type: &str) -> bool {
    matches!(
        fs_type,
        "proc"
            | "sysfs"
            | "devtmpfs"
            | "devpts"
            | "tmpfs"
            | "cgroup"
            | "cgroup2"
            | "overlay"
            | "squashfs"
            | "securityfs"
            | "debugfs"
            | "tracefs"
            | "mqueue"
            | "hugetlbfs"
            | "fusectl"
    )
}

fn stat_mount(path: &Path) -> Option<DiskInfo> {
    let c_path = CString::new(path.as_os_str().as_bytes()).ok()?;
    let mut stat = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    let rc = unsafe { libc::statvfs(c_path.as_ptr(), stat.as_mut_ptr()) };
    if rc != 0 {
        return None;
    }
    let stat = unsafe { stat.assume_init() };
    let block_size = stat.f_frsize as u64;
    let total = stat.f_blocks as u64 * block_size;
    let available = stat.f_bavail as u64 * block_size;
    let used = total.saturating_sub(available);
    Some(DiskInfo {
        mount_point: path.to_string_lossy().to_string(),
        filesystem: "statvfs".to_string(),
        total_bytes: total,
        available_bytes: available,
        used_bytes: used,
        usage_percent: if total == 0 {
            0.0
        } else {
            round2((used as f64 / total as f64) * 100.0)
        },
    })
}

fn read_network_states() -> HashMap<String, String> {
    let mut states = HashMap::new();
    let Ok(entries) = fs::read_dir("/sys/class/net") else {
        return states;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        let state = fs::read_to_string(entry.path().join("operstate"))
            .map(|value| value.trim().to_string())
            .unwrap_or_else(|_| "unknown".to_string());
        states.insert(name, state);
    }
    states
}

fn parse_networks(text: &str, states: &HashMap<String, String>) -> Vec<NetworkInfo> {
    let mut networks = Vec::new();
    for line in text.lines().skip(2) {
        let Some((name, counters)) = line.split_once(':') else {
            continue;
        };
        let name = name.trim().to_string();
        let fields: Vec<&str> = counters.split_whitespace().collect();
        if fields.len() < 16 {
            continue;
        }
        networks.push(NetworkInfo {
            name: name.clone(),
            state: states
                .get(&name)
                .cloned()
                .unwrap_or_else(|| "unknown".to_string()),
            rx_bytes: fields[0].parse().unwrap_or(0),
            tx_bytes: fields[8].parse().unwrap_or(0),
        });
    }
    networks
}

fn round2(value: f64) -> f64 {
    (value * 100.0).round() / 100.0
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use super::*;

    #[test]
    fn parses_cpu_usage_from_proc_stat() {
        let usage = parse_cpu_usage("cpu  100 0 100 800 0 0 0 0 0 0\n").unwrap();
        assert_eq!(usage, 20.0);
    }

    #[test]
    fn parses_memory_from_proc_meminfo() {
        let memory = parse_memory("MemTotal:       1000 kB\nMemAvailable:    250 kB\n").unwrap();
        assert_eq!(memory.used_kb, Some(750));
        assert_eq!(memory.usage_percent, Some(75.0));
    }

    #[test]
    fn parses_uptime_seconds() {
        assert_eq!(parse_uptime_seconds("42.91 100.0\n"), Some(42));
    }

    #[test]
    fn parses_network_interfaces() {
        let mut states = HashMap::new();
        states.insert("eth0".to_string(), "up".to_string());
        let text = "Inter-| Receive | Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n eth0: 123 0 0 0 0 0 0 0 456 0 0 0 0 0 0 0\n";
        let networks = parse_networks(text, &states);
        assert_eq!(networks.len(), 1);
        assert_eq!(networks[0].rx_bytes, 123);
        assert_eq!(networks[0].tx_bytes, 456);
        assert_eq!(networks[0].state, "up");
    }
}
