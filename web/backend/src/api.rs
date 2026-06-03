use axum::{
    extract::{Query, State},
    routing::get,
    Json, Router,
};
use serde::{Deserialize, Serialize};

use crate::{
    demo,
    error::ApiError,
    log_reader::{self, LogQuery},
    status_snapshot, system_reader, AppState,
};

#[derive(Debug, Serialize)]
struct HealthResponse {
    service: &'static str,
    status: &'static str,
    readonly: bool,
    version: &'static str,
    architecture: &'static str,
    mode: &'static str,
    demo_scenario: Option<&'static str>,
}

#[derive(Debug, Deserialize)]
struct LimitQuery {
    limit: Option<usize>,
}

pub fn router(state: AppState) -> Router {
    Router::new()
        .route("/api/health", get(health))
        .route("/api/modules", get(modules))
        .route("/api/resources", get(resources))
        .route("/api/ipc-status", get(ipc_status))
        .route("/api/route-status", get(route_status))
        .route("/api/events", get(events))
        .route("/api/logs", get(logs))
        .with_state(state)
}

async fn health(State(state): State<AppState>) -> Json<HealthResponse> {
    Json(HealthResponse {
        service: "put-webd",
        status: "ok",
        readonly: true,
        version: env!("CARGO_PKG_VERSION"),
        architecture: "anymsg-v2",
        mode: state.runtime_mode.as_str(),
        demo_scenario: state.runtime_mode.demo_scenario(),
    })
}

async fn modules(State(state): State<AppState>) -> Json<status_snapshot::ModulesResponse> {
    let response =
        status_snapshot::read_modules(&state.config.status_dir, state.config.snapshot_stale_ms);
    Json(if state.runtime_mode.is_demo() {
        demo::modules(response)
    } else {
        response
    })
}

async fn resources(State(state): State<AppState>) -> Json<system_reader::ResourcesResponse> {
    let response = system_reader::read_resources();
    Json(if state.runtime_mode.is_demo() {
        demo::resources(response)
    } else {
        response
    })
}

async fn ipc_status(State(state): State<AppState>) -> Json<status_snapshot::IpcStatusResponse> {
    let response =
        status_snapshot::read_ipc_status(&state.config.status_dir, state.config.snapshot_stale_ms);
    Json(if state.runtime_mode.is_demo() {
        demo::ipc_status(response)
    } else {
        response
    })
}

async fn route_status(
    State(state): State<AppState>,
) -> Json<status_snapshot::RouteStatusResponse> {
    let response = status_snapshot::read_route_status(
        &state.config.status_dir,
        state.config.snapshot_stale_ms,
    );
    Json(if state.runtime_mode.is_demo() {
        demo::route_status(response)
    } else {
        response
    })
}

async fn events(
    State(state): State<AppState>,
    Query(query): Query<LimitQuery>,
) -> Json<status_snapshot::EventsResponse> {
    let limit = query.limit.unwrap_or(50);
    let response = status_snapshot::read_events(&state.config.status_dir, limit);
    Json(if state.runtime_mode.is_demo() {
        demo::events(response, limit)
    } else {
        response
    })
}

async fn logs(
    State(state): State<AppState>,
    Query(query): Query<LogQuery>,
) -> Result<Json<log_reader::LogsResponse>, ApiError> {
    log_reader::read_logs(&state.config.log_dir, &state.config.log_sources, query).map(Json)
}

#[cfg(test)]
mod tests {
    use std::{fs, sync::Arc};

    use super::*;
    use crate::{config::AppConfig, RuntimeMode};

    #[tokio::test]
    async fn demo_logs_read_real_log_file() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("adapter.log"),
            "2026-06-03T10:00:00Z [info] real adapter log from device\n",
        )
        .unwrap();

        let state = AppState {
            config: Arc::new(AppConfig {
                log_dir: dir.path().to_path_buf(),
                status_dir: dir.path().join("status"),
                static_dir: dir.path().join("dist"),
                ..AppConfig::default()
            }),
            runtime_mode: RuntimeMode::Demo,
        };

        let Json(response) = logs(
            State(state),
            Query(LogQuery {
                source: "adapter".to_string(),
                level: String::new(),
                keyword: "adapter".to_string(),
                cursor: String::new(),
                limit: 10,
            }),
        )
        .await
        .unwrap();

        assert_eq!(response.lines.len(), 1);
        assert!(response.lines[0].text.contains("real adapter log"));
        assert!(!response.lines[0].text.contains("demo-eth-can-001"));
    }
}
