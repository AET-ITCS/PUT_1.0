mod api;
mod config;
mod error;
mod log_reader;
mod status_snapshot;
mod system_reader;

use std::{net::SocketAddr, path::PathBuf, sync::Arc};

use axum::{
    http::StatusCode,
    response::{IntoResponse, Response},
    routing::get,
    Router,
};
use config::AppConfig;
use tower_http::{
    services::{ServeDir, ServeFile},
    trace::TraceLayer,
};
use tracing::{info, warn};
use tracing_subscriber::{layer::SubscriberExt, util::SubscriberInitExt};

#[derive(Clone)]
pub struct AppState {
    pub config: Arc<AppConfig>,
}

#[tokio::main]
async fn main() {
    init_tracing();

    let config_path = parse_config_arg();
    let config = match AppConfig::load_optional(config_path.as_deref()) {
        Ok(config) => config,
        Err(err) => {
            eprintln!("failed to load config: {err}");
            std::process::exit(2);
        }
    };

    let bind_addr: SocketAddr = match config.bind_addr.parse() {
        Ok(addr) => addr,
        Err(err) => {
            eprintln!("invalid bind_addr '{}': {err}", config.bind_addr);
            std::process::exit(2);
        }
    };

    let state = AppState {
        config: Arc::new(config),
    };

    let app = build_router(state.clone());
    info!(bind_addr = %bind_addr, config = ?state.config, "starting put-webd");

    let listener = tokio::net::TcpListener::bind(bind_addr)
        .await
        .expect("bind put-webd listener");
    axum::serve(listener, app).await.expect("serve put-webd");
}

fn build_router(state: AppState) -> Router {
    let static_dir = state.config.static_dir.clone();
    let mut router = api::router(state).layer(TraceLayer::new_for_http());

    if static_dir.exists() {
        let index = static_dir.join("index.html");
        router = router
            .fallback_service(ServeDir::new(static_dir).not_found_service(ServeFile::new(index)));
    } else {
        warn!(path = %static_dir.display(), "static directory missing; API remains available");
        router = router.fallback(get(static_missing));
    }

    router
}

async fn static_missing() -> Response {
    (
        StatusCode::NOT_FOUND,
        "frontend dist directory is not available on this device\n",
    )
        .into_response()
}

fn init_tracing() {
    let filter = tracing_subscriber::EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| "put_webd=info,tower_http=info".into());
    tracing_subscriber::registry()
        .with(filter)
        .with(tracing_subscriber::fmt::layer())
        .init();
}

fn parse_config_arg() -> Option<PathBuf> {
    let mut args = std::env::args_os().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--config" || arg == "-c" {
            return args.next().map(PathBuf::from);
        }
    }
    Some(PathBuf::from("/etc/put/web_config.toml"))
}

#[cfg(test)]
mod tests {
    use super::parse_config_arg;

    #[test]
    fn parse_config_default_is_some() {
        assert!(parse_config_arg().is_some());
    }
}
