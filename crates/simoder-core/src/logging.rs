//! Centralized log severities, ANSI terminal color styles, and source origins shared across Simoder producers and sinks.

use std::str::FromStr;

pub use anstyle::{AnsiColor, Color, Effects, Style};
use serde::{Deserialize, Deserializer, Serialize, de};
use strum::{EnumString, IntoStaticStr};

/// Stable log severities shared by loader, mods, scripting, and developer console sinks.
#[derive(
    Debug,
    Default,
    Clone,
    Copy,
    PartialEq,
    Eq,
    Hash,
    Serialize,
    EnumString,
    IntoStaticStr,
)]
#[strum(serialize_all = "UPPERCASE", ascii_case_insensitive)]
#[serde(rename_all = "lowercase")]
pub enum LogLevel {
    /// Fine-grained diagnostic tracing (rendered in light gray / standard white `\x1b[37m` for dark consoles).
    Trace,
    /// Developer debugging details (rendered in cyan).
    Debug,
    /// Standard operational information (rendered in green).
    #[default]
    Info,
    /// Recoverable anomalies and configuration caveats (rendered in bold yellow).
    #[strum(to_string = "WARN", serialize = "warning")]
    #[serde(rename = "warn")]
    Warning,
    /// Failure conditions impacting mod or hook execution (rendered in bold red).
    Error,
    /// Memory/resource patch audit records (rendered in magenta; shares filtering rank with [`LogLevel::Info`]).
    Patch,
}

impl<'de> Deserialize<'de> for LogLevel {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let raw = String::deserialize(deserializer)?;
        Self::from_str(&raw)
            .map_err(|_| de::Error::custom(format!("unknown log level '{raw}'")))
    }
}

/// Semantic producer classification for centralized log events.
#[derive(
    Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, EnumString, IntoStaticStr,
)]
#[strum(serialize_all = "UPPERCASE", ascii_case_insensitive)]
#[serde(rename_all = "UPPERCASE")]
pub enum LogSourceType {
    /// Game engine (`EAWebKit` / simulation logs).
    Game,
    /// Simoder core proxy and bootstrap loader.
    Loader,
    /// Third-party Lua/Luau/JS or asset mods.
    Mod,
}

impl LogLevel {
    /// Return the canonical uppercase severity label emitted across sinks
    /// (`"TRACE"`, `"DEBUG"`, `"INFO"`, `"WARN"`, `"ERROR"`, `"PATCH"`).
    pub fn as_str(self) -> &'static str {
        self.into()
    }

    /// Return `true` when this level is valid as a configured minimum verbosity threshold
    /// (every variant except internal [`LogLevel::Patch`] event markers).
    pub const fn is_threshold_level(self) -> bool {
        !matches!(self, Self::Patch)
    }

    /// Parse a configured minimum severity string case-insensitively
    /// (`"trace"`, `"debug"`, `"info"`, `"warn"` / `"warning"`, `"error"`).
    ///
    /// Rejects unrecognized labels and non-threshold variants such as `"patch"`.
    pub fn parse(text: &str) -> Option<Self> {
        Self::from_str(text)
            .ok()
            .filter(|level| level.is_threshold_level())
    }

    /// Report whether `self` passes a configured `minimum` threshold (`Patch` shares rank `2` with `Info`).
    pub const fn passes_minimum(self, minimum: Self) -> bool {
        self.rank() >= minimum.rank()
    }

    /// Return the [`anstyle::Style`] associated with this log level for ANSI terminal rendering.
    pub const fn style(self) -> Style {
        let color = match self {
            Self::Trace => AnsiColor::White,
            Self::Debug => AnsiColor::Cyan,
            Self::Info => AnsiColor::Green,
            Self::Patch => AnsiColor::Magenta,
            Self::Warning => AnsiColor::Yellow,
            Self::Error => AnsiColor::Red,
        };
        let styled = Style::new().fg_color(Some(Color::Ansi(color)));
        match self {
            Self::Warning | Self::Error => styled.bold(),
            _ => styled,
        }
    }

    /// Render [`Self::as_str`] wrapped in its ANSI color escape sequence and trailing reset.
    pub fn colored_label(self) -> String {
        let style = self.style();
        format!("{}{}{}", style.render(), self.as_str(), style.render_reset())
    }

    /// Format a centralized log line as `[{source}][{level}] {message}`, optionally coloring the level tag.
    pub fn format_event(self, source: LogSourceType, message: &str, colorize: bool) -> String {
        if colorize {
            format!("[{}][{}] {message}", source.as_str(), self.colored_label())
        } else {
            format!("[{}][{}] {message}", source.as_str(), self.as_str())
        }
    }

    const fn rank(self) -> u8 {
        match self {
            Self::Trace => 0,
            Self::Debug => 1,
            Self::Info | Self::Patch => 2,
            Self::Warning => 3,
            Self::Error => 4,
        }
    }
}

impl LogSourceType {
    /// Return the canonical uppercase source label emitted across sinks (`"GAME"`, `"LOADER"`, `"MOD"`).
    pub fn as_str(self) -> &'static str {
        self.into()
    }
}
