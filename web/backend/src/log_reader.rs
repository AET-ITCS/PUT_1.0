use std::{fs, path::Path};

use serde::{Deserialize, Serialize};

use crate::error::ApiError;

#[derive(Debug, Deserialize)]
pub struct LogQuery {
    #[serde(default = "default_source")]
    pub source: String,
    #[serde(default)]
    pub level: String,
    #[serde(default)]
    pub keyword: String,
    #[serde(default)]
    pub cursor: String,
    #[serde(default = "default_limit")]
    pub limit: usize,
}

#[derive(Debug, Clone, Serialize)]
pub struct LogLine {
    pub line_number: usize,
    pub source: String,
    pub level: String,
    pub text: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct LogsResponse {
    pub source: String,
    pub lines: Vec<LogLine>,
    pub next_cursor: Option<String>,
    pub has_more: bool,
}

pub fn read_logs(
    log_dir: &Path,
    allowed_sources: &[String],
    query: LogQuery,
) -> Result<LogsResponse, ApiError> {
    let source = query.source.trim();
    if !is_safe_source(source) || !allowed_sources.iter().any(|item| item == source) {
        return Err(ApiError::bad_request("invalid log source"));
    }

    let limit = query.limit.clamp(1, 500);
    let cursor = query.cursor.parse::<usize>().unwrap_or(0);
    let path = log_dir.join(format!("{source}.log"));
    let Ok(text) = fs::read_to_string(path) else {
        return Ok(LogsResponse {
            source: source.to_string(),
            lines: Vec::new(),
            next_cursor: None,
            has_more: false,
        });
    };

    let level_filter = query.level.trim().to_ascii_lowercase();
    let keyword_filter = query.keyword.trim().to_ascii_lowercase();
    let mut skipped = 0;
    let mut lines = Vec::new();
    let mut has_more = false;

    let indexed_lines: Vec<(usize, &str)> = text.lines().enumerate().collect();
    for (idx, line) in indexed_lines.into_iter().rev() {
        if !matches_filters(line, &level_filter, &keyword_filter) {
            continue;
        }
        if skipped < cursor {
            skipped += 1;
            continue;
        }
        if lines.len() >= limit {
            has_more = true;
            break;
        }
        lines.push(LogLine {
            line_number: idx + 1,
            source: source.to_string(),
            level: detect_level(line),
            text: line.to_string(),
        });
    }

    lines.reverse();
    let next_cursor = if has_more {
        Some((cursor + lines.len()).to_string())
    } else {
        None
    };

    Ok(LogsResponse {
        source: source.to_string(),
        lines,
        next_cursor,
        has_more,
    })
}

fn default_source() -> String {
    "linux_app".to_string()
}

fn default_limit() -> usize {
    200
}

fn is_safe_source(source: &str) -> bool {
    !source.is_empty()
        && source
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
}

fn matches_filters(line: &str, level: &str, keyword: &str) -> bool {
    let haystack = line.to_ascii_lowercase();
    (level.is_empty() || haystack.contains(level))
        && (keyword.is_empty() || haystack.contains(keyword))
}

fn detect_level(line: &str) -> String {
    let lower = line.to_ascii_lowercase();
    for level in ["error", "warn", "info", "debug", "trace"] {
        if lower.contains(level) {
            return level.to_string();
        }
    }
    "unknown".to_string()
}

#[cfg(test)]
mod tests {
    use std::fs;

    use super::*;

    #[test]
    fn missing_log_returns_empty_response() {
        let dir = tempfile::tempdir().unwrap();
        let response = read_logs(
            dir.path(),
            &["linux_app".to_string()],
            LogQuery {
                source: "linux_app".to_string(),
                level: String::new(),
                keyword: String::new(),
                cursor: String::new(),
                limit: 10,
            },
        )
        .unwrap();

        assert!(response.lines.is_empty());
        assert!(!response.has_more);
    }

    #[test]
    fn filters_and_pages_logs_from_newest() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("linux_app.log"),
            "[info] boot\n[warn] rs485 slow\n[error] rs485 timeout\n[info] can ok\n",
        )
        .unwrap();

        let response = read_logs(
            dir.path(),
            &["linux_app".to_string()],
            LogQuery {
                source: "linux_app".to_string(),
                level: String::new(),
                keyword: "rs485".to_string(),
                cursor: String::new(),
                limit: 1,
            },
        )
        .unwrap();

        assert_eq!(response.lines.len(), 1);
        assert_eq!(response.lines[0].level, "error");
        assert!(response.has_more);
        assert_eq!(response.next_cursor.as_deref(), Some("1"));
    }

    #[test]
    fn rejects_path_like_source() {
        let dir = tempfile::tempdir().unwrap();
        assert!(read_logs(
            dir.path(),
            &["linux_app".to_string()],
            LogQuery {
                source: "../linux_app".to_string(),
                level: String::new(),
                keyword: String::new(),
                cursor: String::new(),
                limit: 10,
            },
        )
        .is_err());
    }
}
