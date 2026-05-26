use std::{
    collections::HashMap,
    ffi::CString,
    fs,
    io::ErrorKind,
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
    pub devices: Vec<DeviceNodeInfo>,
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

#[derive(Debug, Clone, Serialize)]
pub struct DeviceNodeInfo {
    pub key: String,
    pub label: String,
    pub state: String,
    pub present: Option<bool>,
    pub checked_paths: Vec<String>,
    pub matched_paths: Vec<String>,
}

pub fn read_resources() -> ResourcesResponse {
    ResourcesResponse {
        cpu: read_cpu(),
        memory: read_memory(),
        uptime: read_uptime(),
        disks: read_disks(),
        networks: read_networks(),
        devices: read_device_nodes(),
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

fn read_device_nodes() -> Vec<DeviceNodeInfo> {
    read_device_nodes_from_roots(Path::new("/dev"), Path::new("/sys"), "/dev", "/sys")
}

fn read_device_nodes_from_roots(
    dev_root: &Path,
    sys_root: &Path,
    dev_label: &str,
    sys_label: &str,
) -> Vec<DeviceNodeInfo> {
    vec![
        probe_can_device(dev_root, sys_root, dev_label, sys_label),
        probe_ethernet_device(sys_root, sys_label),
        probe_wifi_device(sys_root, sys_label),
        probe_bluetooth_device(dev_root, sys_root, dev_label, sys_label),
        probe_cellular_device(dev_root, sys_root, dev_label, sys_label),
        probe_rs485_device(dev_root, dev_label),
        probe_usb_device(dev_root, sys_root, dev_label, sys_label),
    ]
}

fn probe_can_device(
    dev_root: &Path,
    sys_root: &Path,
    dev_label: &str,
    sys_label: &str,
) -> DeviceNodeInfo {
    let mut probe = DeviceProbe::new("can", "CAN");
    probe.check_dir_prefix(sys_root, sys_label, "class/net", &["can"]);
    probe.check_exact(dev_root, dev_label, "can0");
    probe.finish()
}

fn probe_ethernet_device(sys_root: &Path, sys_label: &str) -> DeviceNodeInfo {
    let mut probe = DeviceProbe::new("ethernet", "Ethernet");
    probe.check_dir_prefix(sys_root, sys_label, "class/net", &["eth", "en"]);
    probe.finish()
}

fn probe_wifi_device(sys_root: &Path, sys_label: &str) -> DeviceNodeInfo {
    let mut probe = DeviceProbe::new("wifi", "Wi-Fi");
    probe.check_dir_prefix(sys_root, sys_label, "class/net", &["wlan", "wl"]);
    probe.check_wifi_metadata(sys_root, sys_label, "class/net");
    probe.finish()
}

fn probe_bluetooth_device(
    dev_root: &Path,
    sys_root: &Path,
    dev_label: &str,
    sys_label: &str,
) -> DeviceNodeInfo {
    let mut probe = DeviceProbe::new("bluetooth", "Bluetooth");
    probe.check_dir_any(sys_root, sys_label, "class/bluetooth");
    probe.check_exact(dev_root, dev_label, "rfkill");
    probe.finish()
}

fn probe_cellular_device(
    dev_root: &Path,
    sys_root: &Path,
    dev_label: &str,
    sys_label: &str,
) -> DeviceNodeInfo {
    let mut probe = DeviceProbe::new("4g", "4G / Cellular");
    probe.check_dir_prefix(sys_root, sys_label, "class/net", &["wwan", "usb", "ppp"]);
    probe.check_dir_prefix(dev_root, dev_label, "", &["cdc-wdm", "ttyUSB", "ttyACM"]);
    probe.finish()
}

fn probe_rs485_device(dev_root: &Path, dev_label: &str) -> DeviceNodeInfo {
    let mut probe = DeviceProbe::new("rs485", "RS485 / Serial");
    probe.check_dir_prefix(
        dev_root,
        dev_label,
        "",
        &["ttyRS485", "ttyS", "ttyAMA", "ttyUSB", "ttyACM"],
    );
    probe.finish()
}

fn probe_usb_device(
    dev_root: &Path,
    sys_root: &Path,
    dev_label: &str,
    sys_label: &str,
) -> DeviceNodeInfo {
    let mut probe = DeviceProbe::new("usb", "USB");
    probe.check_exact(dev_root, dev_label, "bus/usb");
    probe.check_dir_any(sys_root, sys_label, "bus/usb/devices");
    probe.finish()
}

struct DeviceProbe {
    key: &'static str,
    label: &'static str,
    checked_paths: Vec<String>,
    matched_paths: Vec<String>,
    read_failed: bool,
}

impl DeviceProbe {
    fn new(key: &'static str, label: &'static str) -> Self {
        Self {
            key,
            label,
            checked_paths: Vec::new(),
            matched_paths: Vec::new(),
            read_failed: false,
        }
    }

    fn check_exact(&mut self, root: &Path, root_label: &str, relative: &str) {
        let path = path_join(root, relative);
        let display = display_path(root_label, relative);
        self.checked_paths.push(display.clone());
        match path.try_exists() {
            Ok(true) => self.matched_paths.push(display),
            Ok(false) => {}
            Err(_) => self.read_failed = true,
        }
    }

    fn check_dir_prefix(
        &mut self,
        root: &Path,
        root_label: &str,
        relative: &str,
        prefixes: &[&str],
    ) {
        let base = display_path(root_label, relative);
        self.checked_paths.extend(
            prefixes
                .iter()
                .map(|prefix| format!("{base}/{}*", prefix.trim_start_matches('/'))),
        );
        self.read_dir_entries(root, root_label, relative, |entry_name, _entry_path| {
            prefixes.iter().any(|prefix| entry_name.starts_with(prefix))
        });
    }

    fn check_dir_any(&mut self, root: &Path, root_label: &str, relative: &str) {
        self.checked_paths.push(display_path(root_label, relative));
        self.read_dir_entries(root, root_label, relative, |_entry_name, _entry_path| true);
    }

    fn check_wifi_metadata(&mut self, root: &Path, root_label: &str, relative: &str) {
        self.checked_paths
            .push(format!("{}/{}/*/wireless", root_label, relative));
        self.read_dir_entries(root, root_label, relative, |_entry_name, entry_path| {
            entry_path.join("wireless").exists()
        });
    }

    fn read_dir_entries<F>(
        &mut self,
        root: &Path,
        root_label: &str,
        relative: &str,
        mut matches_entry: F,
    ) where
        F: FnMut(&str, &Path) -> bool,
    {
        let dir = path_join(root, relative);
        let entries = match fs::read_dir(&dir) {
            Ok(entries) => entries,
            Err(err) if err.kind() == ErrorKind::NotFound => return,
            Err(_) => {
                self.read_failed = true;
                return;
            }
        };

        for entry in entries {
            let Ok(entry) = entry else {
                self.read_failed = true;
                continue;
            };
            let entry_name = entry.file_name().to_string_lossy().to_string();
            let entry_path = entry.path();
            if matches_entry(&entry_name, &entry_path) {
                let relative_path = entry_path
                    .strip_prefix(root)
                    .unwrap_or(entry_path.as_path())
                    .to_string_lossy();
                self.matched_paths
                    .push(display_path(root_label, relative_path.as_ref()));
            }
        }
    }

    fn finish(self) -> DeviceNodeInfo {
        let present = if self.matched_paths.is_empty() {
            if self.read_failed {
                None
            } else {
                Some(false)
            }
        } else {
            Some(true)
        };
        let state = match present {
            Some(true) => "present",
            Some(false) => "missing",
            None => "unknown",
        };

        DeviceNodeInfo {
            key: self.key.to_string(),
            label: self.label.to_string(),
            state: state.to_string(),
            present,
            checked_paths: self.checked_paths,
            matched_paths: self.matched_paths,
        }
    }
}

fn path_join(root: &Path, relative: &str) -> PathBuf {
    if relative.is_empty() {
        root.to_path_buf()
    } else {
        root.join(relative)
    }
}

fn display_path(root_label: &str, relative: &str) -> String {
    if relative.is_empty() {
        root_label.to_string()
    } else {
        format!(
            "{}/{}",
            root_label.trim_end_matches('/'),
            relative.trim_start_matches('/')
        )
    }
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
    use std::{collections::HashMap, fs};

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

    #[test]
    fn probes_key_device_nodes_from_dev_and_sys() {
        let dir = tempfile::tempdir().unwrap();
        let dev = dir.path().join("dev");
        let sys = dir.path().join("sys");
        fs::create_dir_all(sys.join("class/net/can0")).unwrap();
        fs::create_dir_all(sys.join("class/net/eth0")).unwrap();
        fs::create_dir_all(sys.join("class/net/wlan0/wireless")).unwrap();
        fs::create_dir_all(sys.join("class/bluetooth/hci0")).unwrap();
        fs::create_dir_all(sys.join("bus/usb/devices/1-1")).unwrap();
        fs::create_dir_all(dev.join("bus/usb")).unwrap();
        fs::write(dev.join("ttyS1"), "").unwrap();
        fs::write(dev.join("cdc-wdm0"), "").unwrap();

        let devices = read_device_nodes_from_roots(&dev, &sys, "/dev", "/sys");

        for key in ["can", "ethernet", "wifi", "bluetooth", "4g", "rs485", "usb"] {
            let device = devices.iter().find(|device| device.key == key).unwrap();
            assert_eq!(device.state, "present");
            assert_eq!(device.present, Some(true));
            assert!(!device.matched_paths.is_empty());
        }
    }

    #[test]
    fn device_probe_marks_read_failures_unknown() {
        let dir = tempfile::tempdir().unwrap();
        let dev = dir.path().join("dev");
        let sys = dir.path().join("sys");
        fs::create_dir_all(&dev).unwrap();
        fs::create_dir_all(sys.join("class")).unwrap();
        fs::write(sys.join("class/net"), "not a directory").unwrap();

        let devices = read_device_nodes_from_roots(&dev, &sys, "/dev", "/sys");
        let ethernet = devices
            .iter()
            .find(|device| device.key == "ethernet")
            .unwrap();

        assert_eq!(ethernet.state, "unknown");
        assert_eq!(ethernet.present, None);
    }
}
