//! Typed loader and developer-console configuration codecs for `config.toon` and TOML.

use std::fs;
use std::path::Path;

use serde::{Deserialize, Serialize};
use simoder_core::LogLevel;

use crate::FormatError;
use crate::toon::{load_toon_file_typed, parse_toon_typed};

/// Canonical bootstrap configuration filename created inside the `simoder/` directory.
pub const CONFIG_TOON_FILE_NAME: &str = "config.toon";

/// Default relative mods folder inside the active Simoder profile directory.
pub const DEFAULT_MODS_DIR: &str = "mods";

/// Default BCP-47 fallback locale tag when no game locale override is specified.
pub const DEFAULT_LOCALE_TAG: &str = "en-US";

/// Centralized developer-console sink settings stored under `developerConsole` in `config.toon`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase", deny_unknown_fields)]
pub struct DeveloperConsoleConfig {
    /// Whether the centralized developer console sink is active at startup.
    pub enabled: bool,
    /// Whether `EAWebKit` and game simulation logs are forwarded to the console sink.
    #[serde(alias = "capture_game_logs")]
    pub capture_game_logs: bool,
    /// Whether Simoder bootstrap and loader diagnostic messages are recorded.
    #[serde(alias = "capture_loader_logs")]
    pub capture_loader_logs: bool,
    /// Whether Lua/Luau/JS mod log messages are captured.
    #[serde(alias = "capture_mod_logs")]
    pub capture_mod_logs: bool,
    /// Minimum log severity threshold (`"trace"`, `"debug"`, `"info"`, `"warn"`, or `"error"`).
    pub level: String,
}

impl Default for DeveloperConsoleConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            capture_game_logs: true,
            capture_loader_logs: true,
            capture_mod_logs: true,
            level: "info".to_string(),
        }
    }
}

/// Top-level loader configuration supporting both `config.toon` bootstrap settings and TOML overrides.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct LoaderConfig {
    /// Master switch for activating Simoder runtime hooks.
    pub enabled: bool,
    /// Fallback BCP-47 locale tag used when resolving mod localization tables.
    pub default_locale: String,
    /// Directory path containing installed mods relative to the Simoder root.
    pub mods_dir: String,
    /// Enables additional diagnostic checks and developer overlays when `true`.
    pub dev_mode: bool,
    /// Developer console routing and verbosity configuration.
    #[serde(rename = "developer_console", alias = "developerConsole")]
    pub developer_console: DeveloperConsoleConfig,
}

impl Default for LoaderConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            default_locale: DEFAULT_LOCALE_TAG.to_string(),
            mods_dir: DEFAULT_MODS_DIR.to_string(),
            dev_mode: false,
            developer_console: DeveloperConsoleConfig::default(),
        }
    }
}

/// Strict TOON wrapper matching the required `developerConsole` root key of `config.toon`.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ToonBootstrapConfig {
    #[serde(rename = "developerConsole")]
    developer_console: DeveloperConsoleConfig,
}

/// Return the canonical default `config.toon` content seeded when no configuration file exists yet.
pub const fn default_config_text() -> &'static str {
    "developerConsole:\n\
     \x20 enabled: false\n\
     \x20 captureGameLogs: true\n\
     \x20 captureLoaderLogs: true\n\
     \x20 captureModLogs: true\n\
     \x20 level: info\n"
}

impl LoaderConfig {
    /// Parse and validate loader configuration from a UTF-8 TOML document.
    pub fn from_toml(source: &str) -> Result<Self, FormatError> {
        let cfg: Self = toml::from_str(source)?;
        validate_console_level(&cfg.developer_console.level)?;
        Ok(cfg)
    }

    /// Parse and validate `config.toon` bootstrap settings from strict TOON source text.
    ///
    /// Requires a `developerConsole` object and validates [`DeveloperConsoleConfig::level`].
    pub fn from_toon(source: &str) -> Result<Self, FormatError> {
        let root: ToonBootstrapConfig = parse_toon_typed(source)?;
        Self::from_toon_bootstrap(root)
    }

    fn from_toon_bootstrap(root: ToonBootstrapConfig) -> Result<Self, FormatError> {
        validate_console_level(&root.developer_console.level)?;
        Ok(Self {
            developer_console: root.developer_console,
            ..Self::default()
        })
    }
}

/// Ensure `config.toon` exists inside `simoder_directory` (creating defaults atomically when absent),
/// then deserialize and validate it via [`serde_toon`].
pub fn load_or_create_config(simoder_directory: &Path) -> Result<LoaderConfig, FormatError> {
    fs::create_dir_all(simoder_directory)?;
    let path = simoder_directory.join(CONFIG_TOON_FILE_NAME);
    if !path.exists() {
        write_atomic(&path, default_config_text().as_bytes())?;
    }
    let root: ToonBootstrapConfig = load_toon_file_typed(&path)?;
    LoaderConfig::from_toon_bootstrap(root)
}

fn validate_console_level(level: &str) -> Result<(), FormatError> {
    if matches!(level, "trace" | "debug" | "info" | "warn" | "error")
        && LogLevel::parse(level).is_some()
    {
        return Ok(());
    }
    Err(FormatError::ConfigValidation(
        "developerConsole.level must be trace, debug, info, warn, or error".to_string(),
    ))
}

fn write_atomic(path: &Path, bytes: &[u8]) -> Result<(), FormatError> {
    let temp_name = format!("{CONFIG_TOON_FILE_NAME}.tmp");
    let temp_path = path.with_file_name(temp_name);
    fs::write(&temp_path, bytes)?;
    if let Err(err) = fs::rename(&temp_path, path) {
        let _ = fs::remove_file(&temp_path);
        return Err(err.into());
    }
    Ok(())
}
