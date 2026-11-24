//! Centralized log severities, ANSI terminal color styles, source origins, and the non-blocking
//! `2048`-event bounded semantic logging pipeline (`docs/logging.md`) shared across Simoder.

use std::cell::Cell;
use std::collections::VecDeque;
use std::str::FromStr;

pub use anstyle::{AnsiColor, Color, Effects, Style};
use parking_lot::Mutex;
use serde::{Deserialize, Deserializer, Serialize, de};
use strum::{EnumString, IntoStaticStr};

use crate::redaction::redact_sensitive_text;

/// Maximum number of log events buffered in the central non-blocking queue (`docs/logging.md`).
pub const MAX_LOG_QUEUE_CAPACITY: usize = 2048;

/// Maximum number of queued log events drained per worker flush batch (`docs/logging.md`).
pub const LOG_DRAIN_BATCH_SIZE: usize = 64;

/// Maximum byte length copied from a single `OutputDebugStringA/W` or engine log message (`docs/logging.md`).
pub const MAX_CAPTURED_LOG_BYTES: usize = 4096;

const MAX_RECENT_TAIL_LINES: usize = 64;

thread_local! {
    static ODS_RECURSION_GUARD: Cell<bool> = const { Cell::new(false) };
}

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
        Self::from_str(&raw).map_err(|_| de::Error::custom(format!("unknown log level '{raw}'")))
    }
}

/// Semantic producer classification for centralized log events (`docs/logging.md`).
#[derive(
    Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, EnumString, IntoStaticStr,
)]
#[strum(serialize_all = "UPPERCASE", ascii_case_insensitive)]
#[serde(rename_all = "UPPERCASE")]
pub enum LogSourceType {
    /// Game engine (`EAWebKit` / `OutputDebugString` / simulation logs).
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

/// Sink filter parameters controlling which queued log records are mirrored to the external DeveloperConsole (`docs/logging.md`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct LogConsoleFilter {
    /// Whether the external DeveloperConsole sink is enabled.
    pub enabled: bool,
    /// Whether `GAME` records are shown in the console.
    pub capture_game_logs: bool,
    /// Whether `LOADER` records are shown in the console.
    pub capture_loader_logs: bool,
    /// Whether `MOD` records are shown in the console.
    pub capture_mod_logs: bool,
    /// Minimum severity threshold rendered to the console.
    pub level: LogLevel,
}

impl Default for LogConsoleFilter {
    fn default() -> Self {
        Self {
            enabled: false,
            capture_game_logs: true,
            capture_loader_logs: true,
            capture_mod_logs: true,
            level: LogLevel::Info,
        }
    }
}

/// One semantic log event in the central bounded logging pipeline (`docs/logging.md`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LogRecord {
    /// Monotonic or wall-clock millisecond timestamp.
    pub timestamp_ms: u64,
    /// Producer category (`GAME`, `LOADER`, `MOD`).
    pub source_type: LogSourceType,
    /// Specific producer identifier (for example `"OutputDebugStringA"`, `"EAWebKit::DebugLog"`, or mod ID).
    pub source_name: String,
    /// Log severity.
    pub level: LogLevel,
    /// OS thread identifier of the emitting thread.
    pub thread_id: u32,
    /// Redacted and length-bounded message text (`<= 4096` bytes).
    pub message: String,
}

impl LogRecord {
    /// Format according to the canonical `docs/logging.md` pipeline schema:
    /// `timestamp | source type | source name | level | thread | message`.
    pub fn format_pipeline_line(&self) -> String {
        format!(
            "{} | {} | {} | {} | {} | {}",
            self.timestamp_ms,
            self.source_type.as_str(),
            self.source_name,
            self.level.as_str(),
            self.thread_id,
            self.message
        )
    }

    /// Check whether this record passes the configured external console filter.
    ///
    /// The diagnostic log file sink (`sc13modloader.log`) always receives every record regardless of console filters.
    pub fn passes_console_filter(&self, filter: impl Into<LogConsoleFilter>) -> bool {
        let cfg = filter.into();
        if !cfg.enabled {
            return false;
        }
        let source_allowed = match self.source_type {
            LogSourceType::Game => cfg.capture_game_logs,
            LogSourceType::Loader => cfg.capture_loader_logs,
            LogSourceType::Mod => cfg.capture_mod_logs,
        };
        source_allowed && self.level.passes_minimum(cfg.level)
    }
}

struct PipelineInner {
    queue: VecDeque<LogRecord>,
    recent_tail: VecDeque<String>,
    dropped_since_last_drain: u64,
    total_dropped: u64,
}

/// Batch drained from [`DiagnosticLogPipeline::drain_batch`].
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct DrainedLogBatch {
    /// Complete records for the always-on diagnostic file sink (`sc13modloader.log`).
    pub file_records: Vec<LogRecord>,
    /// Subset of `file_records` passing the active [`LogConsoleFilter`] for console output.
    pub console_records: Vec<LogRecord>,
    /// Number of overflow events dropped prior to this batch drain.
    pub dropped_before_batch: u64,
}

/// Central non-blocking `2048`-event bounded logging queue draining up to `64` records per batch (`docs/logging.md`).
pub struct DiagnosticLogPipeline {
    capacity: usize,
    batch_size: usize,
    inner: Mutex<PipelineInner>,
}

impl Default for DiagnosticLogPipeline {
    fn default() -> Self {
        Self::new()
    }
}

impl DiagnosticLogPipeline {
    /// Create a pipeline with the contract capacity (`2048`) and batch drain size (`64`).
    pub fn new() -> Self {
        Self::with_limits(MAX_LOG_QUEUE_CAPACITY, LOG_DRAIN_BATCH_SIZE)
    }

    /// Create a pipeline with custom capacity and batch size (clamped to `>= 1`).
    pub fn with_limits(capacity: usize, batch_size: usize) -> Self {
        let cap = capacity.max(1);
        Self {
            capacity: cap,
            batch_size: batch_size.max(1),
            inner: Mutex::new(PipelineInner {
                queue: VecDeque::with_capacity(cap.min(2048)),
                recent_tail: VecDeque::with_capacity(MAX_RECENT_TAIL_LINES),
                dropped_since_last_drain: 0,
                total_dropped: 0,
            }),
        }
    }

    /// Enqueue a bounded, redacted [`LogRecord`] without blocking; drops new events when full (`docs/logging.md`).
    pub fn push(
        &self,
        timestamp_ms: u64,
        source_type: LogSourceType,
        source_name: impl Into<String>,
        level: LogLevel,
        thread_id: u32,
        raw_message: &str,
    ) -> bool {
        let truncated = truncate_utf8_str(raw_message, MAX_CAPTURED_LOG_BYTES);
        let clean_message = redact_sensitive_text(truncated);
        let record = LogRecord {
            timestamp_ms,
            source_type,
            source_name: source_name.into(),
            level,
            thread_id,
            message: clean_message,
        };

        let tail_line = format!(
            "[{}] [{}] [{}] {}",
            record.source_type.as_str(),
            record.source_name,
            record.level.as_str(),
            record.message
        );

        let mut inner = self.inner.lock();
        if inner.recent_tail.len() >= MAX_RECENT_TAIL_LINES {
            inner.recent_tail.pop_front();
        }
        inner.recent_tail.push_back(tail_line);

        if inner.queue.len() >= self.capacity {
            inner.dropped_since_last_drain = inner.dropped_since_last_drain.saturating_add(1);
            inner.total_dropped = inner.total_dropped.saturating_add(1);
            return false;
        }
        inner.queue.push_back(record);
        true
    }

    /// Non-blocking snapshot of up to `max_lines` recent redacted log lines for crash capture.
    ///
    /// Returns `None` if the pipeline mutex is currently held (preventing deadlocks inside a crash handler).
    pub fn try_recent_tail(&self, max_lines: usize) -> Option<Vec<String>> {
        let inner = self.inner.try_lock()?;
        let take_count = max_lines.min(inner.recent_tail.len());
        let start = inner.recent_tail.len().saturating_sub(take_count);
        Some(inner.recent_tail.iter().skip(start).cloned().collect())
    }

    /// Capture an `OutputDebugStringA` or `OutputDebugStringW` message with a thread-local recursion guard
    /// and `4096`-byte bound (`docs/logging.md`).
    pub fn capture_output_debug_string(
        &self,
        timestamp_ms: u64,
        is_wide: bool,
        thread_id: u32,
        raw_text: &str,
    ) -> bool {
        let already_in_hook = ODS_RECURSION_GUARD.with(|cell| {
            if cell.get() {
                true
            } else {
                cell.set(true);
                false
            }
        });
        if already_in_hook {
            return false;
        }
        struct OdsReset;
        impl Drop for OdsReset {
            fn drop(&mut self) {
                ODS_RECURSION_GUARD.with(|cell| cell.set(false));
            }
        }
        let _reset = OdsReset;

        let api_name = if is_wide {
            "OutputDebugStringW"
        } else {
            "OutputDebugStringA"
        };
        let trimmed = raw_text.trim_end_matches(['\r', '\n']);
        self.push(
            timestamp_ms,
            LogSourceType::Game,
            api_name,
            LogLevel::Debug,
            thread_id,
            trimmed,
        )
    }

    /// Drain up to `64` (`batch_size`) records in one flush batch, prepending a dropped-count warning
    /// if overflow occurred since the last drain (`docs/logging.md`).
    pub fn drain_batch(
        &self,
        now_ms: u64,
        console_filter: impl Into<LogConsoleFilter>,
    ) -> DrainedLogBatch {
        let filter = console_filter.into();
        let mut inner = self.inner.lock();
        let dropped = inner.dropped_since_last_drain;
        inner.dropped_since_last_drain = 0;

        let mut file_records = Vec::with_capacity(self.batch_size + usize::from(dropped > 0));
        if dropped > 0 {
            file_records.push(LogRecord {
                timestamp_ms: now_ms,
                source_type: LogSourceType::Loader,
                source_name: "simoder.logger".to_string(),
                level: LogLevel::Warning,
                thread_id: 0,
                message: format!(
                    "Log queue overflow: dropped {dropped} event(s) (capacity={})",
                    self.capacity
                ),
            });
        }

        for _ in 0..self.batch_size {
            let Some(rec) = inner.queue.pop_front() else {
                break;
            };
            file_records.push(rec);
        }
        drop(inner);

        let console_records = file_records
            .iter()
            .filter(|rec| rec.passes_console_filter(filter))
            .cloned()
            .collect();

        DrainedLogBatch {
            file_records,
            console_records,
            dropped_before_batch: dropped,
        }
    }

    /// Return the total number of dropped events since pipeline creation.
    pub fn total_dropped(&self) -> u64 {
        self.inner.lock().total_dropped
    }

    /// Return the current number of buffered records waiting to be drained.
    pub fn len(&self) -> usize {
        self.inner.lock().queue.len()
    }

    /// Return `true` when no records are currently queued.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// Truncate a UTF-8 string slice to at most `max_bytes` on a character boundary.
pub fn truncate_utf8_str(input: &str, max_bytes: usize) -> &str {
    if input.len() <= max_bytes {
        return input;
    }
    let mut end = max_bytes;
    while end > 0 && !input.is_char_boundary(end) {
        end -= 1;
    }
    &input[..end]
}
