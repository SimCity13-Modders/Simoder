//! Centralized log severities and source origins shared across Simoder producers and sinks.

/// Stable log severities shared by loader, mods, scripting, and developer console sinks.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Patch,
}

/// Semantic producer classification for centralized log events.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum LogSourceType {
    Game,
    Loader,
    Mod,
}

impl LogLevel {
    /// Return the canonical uppercase severity label emitted across sinks
    /// (`"TRACE"`, `"DEBUG"`, `"INFO"`, `"WARN"`, `"ERROR"`, `"PATCH"`).
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Trace => "TRACE",
            Self::Debug => "DEBUG",
            Self::Info => "INFO",
            Self::Warning => "WARN",
            Self::Error => "ERROR",
            Self::Patch => "PATCH",
        }
    }

    /// Parse a configured minimum severity string (`"trace"`, `"debug"`, `"info"`, `"warn"` / `"warning"`, `"error"`).
    ///
    /// Matches C++ `sc13::logging::ParseLevel`; returns `None` when unrecognized.
    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "trace" => Some(Self::Trace),
            "debug" => Some(Self::Debug),
            "info" => Some(Self::Info),
            "warning" | "warn" => Some(Self::Warning),
            "error" => Some(Self::Error),
            _ => None,
        }
    }

    /// Report whether `self` passes a configured `minimum` threshold (`Patch` shares rank `2` with `Info`).
    pub const fn passes_minimum(self, minimum: Self) -> bool {
        self.rank() >= minimum.rank()
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
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Game => "GAME",
            Self::Loader => "LOADER",
            Self::Mod => "MOD",
        }
    }
}
