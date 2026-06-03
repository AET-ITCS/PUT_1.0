mod api;
mod config;
mod demo;
mod error;
mod log_reader;
mod status_snapshot;
mod system_reader;

use std::{
    ffi::OsString,
    fs::{self, OpenOptions},
    io::{self, Write},
    net::SocketAddr,
    path::{Path, PathBuf},
    sync::Arc,
};

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
use tracing_subscriber::{fmt::MakeWriter, layer::SubscriberExt, util::SubscriberInitExt};

#[derive(Clone)]
pub struct AppState {
    pub config: Arc<AppConfig>,
    pub runtime_mode: RuntimeMode,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RuntimeMode {
    Normal,
    Demo,
}

impl RuntimeMode {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Normal => "normal",
            Self::Demo => "demo",
        }
    }

    pub fn demo_scenario(self) -> Option<&'static str> {
        match self {
            Self::Normal => None,
            Self::Demo => Some(demo::SCENARIO_ETHERNET_TO_CAN),
        }
    }

    pub fn is_demo(self) -> bool {
        self == Self::Demo
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct CliArgs {
    config_path: Option<PathBuf>,
    demo_mode: bool,
}

#[tokio::main]
async fn main() {
    let cli = parse_cli_args();
    let config = match AppConfig::load_optional(cli.config_path.as_deref()) {
        Ok(config) => config,
        Err(err) => {
            eprintln!("failed to load config: {err}");
            std::process::exit(2);
        }
    };

    init_tracing(&config.log_dir);

    let bind_addr: SocketAddr = match config.bind_addr.parse() {
        Ok(addr) => addr,
        Err(err) => {
            eprintln!("invalid bind_addr '{}': {err}", config.bind_addr);
            std::process::exit(2);
        }
    };

    let state = AppState {
        config: Arc::new(config),
        runtime_mode: if cli.demo_mode {
            RuntimeMode::Demo
        } else {
            RuntimeMode::Normal
        },
    };

    let app = build_router(state.clone());
    info!(
        bind_addr = %bind_addr,
        config = ?state.config,
        mode = state.runtime_mode.as_str(),
        demo_scenario = ?state.runtime_mode.demo_scenario(),
        "starting put-webd"
    );

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

fn init_tracing(log_dir: &Path) {
    let log_path = log_dir.join("web.log");
    if let Some(parent) = log_path.parent() {
        if let Err(err) = fs::create_dir_all(parent) {
            eprintln!(
                "failed to create log directory '{}': {err}",
                parent.display()
            );
        }
    }

    let filter = tracing_subscriber::EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| "put_webd=info,tower_http=info".into());
    let stdout_layer = tracing_subscriber::fmt::layer();
    let file_layer = tracing_subscriber::fmt::layer()
        .with_ansi(false)
        .with_writer(FileLogWriter::new(log_path));

    tracing_subscriber::registry()
        .with(filter)
        .with(stdout_layer)
        .with(file_layer)
        .init();
}

#[derive(Clone)]
struct FileLogWriter {
    path: Arc<PathBuf>,
}

impl FileLogWriter {
    fn new(path: PathBuf) -> Self {
        Self {
            path: Arc::new(path),
        }
    }
}

impl<'a> MakeWriter<'a> for FileLogWriter {
    type Writer = FileLogGuard;

    fn make_writer(&'a self) -> Self::Writer {
        FileLogGuard::new(&self.path)
    }
}

struct FileLogGuard {
    file: Option<fs::File>,
}

impl FileLogGuard {
    fn new(path: &Path) -> Self {
        Self {
            file: OpenOptions::new().create(true).append(true).open(path).ok(),
        }
    }
}

impl Write for FileLogGuard {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self.file.as_mut() {
            Some(file) => file.write(buf),
            None => Ok(buf.len()),
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        match self.file.as_mut() {
            Some(file) => file.flush(),
            None => Ok(()),
        }
    }
}

fn parse_cli_args() -> CliArgs {
    parse_cli_args_from(std::env::args_os().skip(1))
}

fn parse_cli_args_from<I, S>(args: I) -> CliArgs
where
    I: IntoIterator<Item = S>,
    S: Into<OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    let mut config_seen = false;
    let mut config_path = None;
    let mut demo_mode = false;

    while let Some(arg) = args.next() {
        if arg == "--config" || arg == "-c" {
            config_seen = true;
            config_path = args.next().map(PathBuf::from);
        } else if arg == "--demo-mode" {
            demo_mode = true;
        }
    }

    if !config_seen {
        config_path = Some(PathBuf::from("/etc/put/web_config.toml"));
    }

    CliArgs {
        config_path,
        demo_mode,
    }
}

#[cfg(test)]
mod tests {
    use std::io::Write;

    use tracing_subscriber::fmt::MakeWriter;

    use super::{parse_cli_args_from, FileLogWriter};

    #[test]
    fn parse_args_default_config_and_normal_mode() {
        let args = parse_cli_args_from(Vec::<&str>::new());
        assert_eq!(
            args.config_path.as_deref(),
            Some(std::path::Path::new("/etc/put/web_config.toml"))
        );
        assert!(!args.demo_mode);
    }

    #[test]
    fn parse_args_accepts_config_and_demo_mode() {
        let args =
            parse_cli_args_from(["--config", "web/config/web_config.dev.toml", "--demo-mode"]);
        assert_eq!(
            args.config_path.as_deref(),
            Some(std::path::Path::new("web/config/web_config.dev.toml"))
        );
        assert!(args.demo_mode);
    }

    #[test]
    fn parse_args_accepts_short_config_after_demo_mode() {
        let args = parse_cli_args_from(["--demo-mode", "-c", "demo.toml"]);
        assert_eq!(
            args.config_path.as_deref(),
            Some(std::path::Path::new("demo.toml"))
        );
        assert!(args.demo_mode);
    }

    #[test]
    fn file_log_writer_appends_web_log() {
        let dir = tempfile::tempdir().unwrap();
        let writer = FileLogWriter::new(dir.path().join("web.log"));
        {
            let mut log = writer.make_writer();
            writeln!(log, "snapshot parse failed").unwrap();
        }

        let text = std::fs::read_to_string(dir.path().join("web.log")).unwrap();
        assert!(text.contains("snapshot parse failed"));
    }
}
