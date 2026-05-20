use axum::{
    extract::{Query, State},
    routing::get,
    Json, Router,
};
use serde::{Deserialize, Serialize};

use crate::{
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
        .route("/api/can-status", get(can_status))
        .route("/api/ipc-status", get(ipc_status))
        .route("/api/events", get(events))
        .route("/api/logs", get(logs))
        .with_state(state)
}

async fn health(State(state): State<AppState>) -> Json<HealthResponse> {
    Json(HealthResponse {
        service: "put-webd",
        status: "ok",
        readonly: state.config.readonly,
        version: env!("CARGO_PKG_VERSION"),
    })
}

async fn modules(State(state): State<AppState>) -> Json<status_snapshot::ModulesResponse> {
    Json(status_snapshot::read_modules(
        &state.config.status_dir,
        state.config.snapshot_stale_ms,
    ))
}

async fn resources() -> Json<system_reader::ResourcesResponse> {
    Json(system_reader::read_resources())
}

async fn can_status(State(state): State<AppState>) -> Json<status_snapshot::CanStatusResponse> {
    Json(status_snapshot::read_can_status(
        &state.config.status_dir,
        state.config.snapshot_stale_ms,
    ))
}

async fn ipc_status(State(state): State<AppState>) -> Json<status_snapshot::IpcStatusResponse> {
    Json(status_snapshot::read_ipc_status(
        &state.config.status_dir,
        state.config.snapshot_stale_ms,
    ))
}

async fn events(
    State(state): State<AppState>,
    Query(query): Query<LimitQuery>,
) -> Json<status_snapshot::EventsResponse> {
    Json(status_snapshot::read_events(
        &state.config.status_dir,
        query.limit.unwrap_or(50),
    ))
}

async fn logs(
    State(state): State<AppState>,
    Query(query): Query<LogQuery>,
) -> Result<Json<log_reader::LogsResponse>, ApiError> {
    log_reader::read_logs(&state.config.log_dir, &state.config.log_sources, query).map(Json)
}
