use std::{
    fs,
    path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct AppConfig {
    #[serde(default = "default_bind_addr")]
    pub bind_addr: String,
    #[serde(default = "default_static_dir")]
    pub static_dir: PathBuf,
    #[serde(default = "default_status_dir")]
    pub status_dir: PathBuf,
    #[serde(default = "default_log_dir")]
    pub log_dir: PathBuf,
    #[serde(default = "default_readonly")]
    pub readonly: bool,
    #[serde(default = "default_snapshot_stale_ms")]
    pub snapshot_stale_ms: u64,
    #[serde(default = "default_log_sources")]
    pub log_sources: Vec<String>,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            bind_addr: default_bind_addr(),
            static_dir: default_static_dir(),
            status_dir: default_status_dir(),
            log_dir: default_log_dir(),
            readonly: default_readonly(),
            snapshot_stale_ms: default_snapshot_stale_ms(),
            log_sources: default_log_sources(),
        }
    }
}

impl AppConfig {
    pub fn load_optional(path: Option<&Path>) -> Result<Self, String> {
        let Some(path) = path else {
            return Ok(Self::default());
        };

        if !path.exists() {
            return Ok(Self::default());
        }

        let text =
            fs::read_to_string(path).map_err(|err| format!("read {}: {err}", path.display()))?;
        toml::from_str(&text).map_err(|err| format!("parse {}: {err}", path.display()))
    }
}

fn default_bind_addr() -> String {
    "0.0.0.0:8080".to_string()
}

fn default_static_dir() -> PathBuf {
    PathBuf::from("/opt/put/web/dist")
}

fn default_status_dir() -> PathBuf {
    PathBuf::from("/run/put/status")
}

fn default_log_dir() -> PathBuf {
    PathBuf::from("/var/log/put")
}

fn default_readonly() -> bool {
    true
}

fn default_snapshot_stale_ms() -> u64 {
    5_000
}

fn default_log_sources() -> Vec<String> {
    vec![
        "linux_app".to_string(),
        "web".to_string(),
        "system".to_string(),
        "ipc".to_string(),
        "router".to_string(),
        "adapter".to_string(),
    ]
}

#[cfg(test)]
mod tests {
    use super::AppConfig;

    #[test]
    fn defaults_match_design_doc() {
        let config = AppConfig::default();
        assert_eq!(config.bind_addr, "0.0.0.0:8080");
        assert_eq!(config.static_dir.to_string_lossy(), "/opt/put/web/dist");
        assert_eq!(config.status_dir.to_string_lossy(), "/run/put/status");
        assert_eq!(config.log_dir.to_string_lossy(), "/var/log/put");
        assert!(config.readonly);
        assert_eq!(config.snapshot_stale_ms, 5_000);
        assert_eq!(
            config.log_sources,
            vec![
                "linux_app".to_string(),
                "web".to_string(),
                "system".to_string(),
                "ipc".to_string(),
                "router".to_string(),
                "adapter".to_string(),
            ]
        );
    }

    #[test]
    fn missing_config_uses_defaults() {
        let missing = tempfile::tempdir().unwrap().path().join("missing.toml");
        assert_eq!(
            AppConfig::load_optional(Some(&missing)).unwrap(),
            AppConfig::default()
        );
    }
}
