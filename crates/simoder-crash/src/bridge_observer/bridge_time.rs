//! Simulation clock and pause-state tracker ([`BridgeTime`]) and `EAWebKitClient::DebugLog`
//! console message parser ([`EaWebKitDebugLogMessage`]).

use std::collections::HashMap;

use serde::{Deserialize, Serialize};
use serde_json::{Value as JsonValue, json};
use serde_with::DeserializeFromStr;
use simoder_core::{
    DiagnosticLogPipeline, LogLevel, LogSourceType, MAX_CAPTURED_LOG_BYTES, redact_sensitive_text,
    truncate_utf8_str,
};
use strum::{EnumString, IntoStaticStr, VariantArray};

use crate::config::{
    BRIDGE_CLOCK_MAX_AGE_MS, BRIDGE_CLOCK_STALL_MS, RETAIL_EXE_SHARED_RET4_NOOP_RVA,
    UI_EVENT_SPEED_CHANGED_ID, UI_SPEED_VALUE_PAUSED, UI_SPEED_VALUES_RUNNING,
};
use crate::types::CrashError;

#[derive(Debug, Clone, Copy, PartialEq)]
struct ClockSample {
    time_ms: i64,
    seconds: f64,
}

/// Simulation clock and pause-state tracker ported from `Simoder-OpenSCP`.
///
/// Correlates `periodTimes/0` day-fraction results (`0.0..=1.0` -> `0.0..86400.0` seconds),
/// `gamespeed` query results, and `ReceiveGameEventFromClient` `SPEED_CHANGED` (`182429322`) notifications
/// across UI bindings and pages, distinguishing a stationary clock from a reported pause.
#[derive(Debug, Default, Clone)]
pub struct BridgeTime {
    samples: Vec<ClockSample>,
    by_page: HashMap<String, Vec<ClockSample>>,
    pending_calls: HashMap<i64, (String, String)>,
    latest_speed: Option<(i64, bool)>,
    last_seen_ms: i64,
}

impl BridgeTime {
    /// Construct an empty [`BridgeTime`] tracker.
    pub fn new() -> Self {
        Self::default()
    }

    /// Observe one raw UI bridge JSON row (`channel = "invoke" | "result" | "event"`).
    pub fn observe_json_row(&mut self, row: &JsonValue) {
        let Some(time_ms) = row["time"].as_i64() else {
            return;
        };
        self.last_seen_ms = self.last_seen_ms.max(time_ms);

        if row["eventID"].as_u64() == Some(UI_EVENT_SPEED_CHANGED_ID)
            && let Some(is_paused) = row["event"]["items"][1]["properties"]
                .as_array()
                .and_then(|props| props.iter().find(|p| p["name"] == "speed"))
                .and_then(|p| decode_speed_paused(&p["value"]["value"]))
        {
            self.observe_speed(time_ms, is_paused);
        }

        let Some(binding) = row["binding"].as_str() else {
            return;
        };
        match row["channel"].as_str() {
            Some("invoke") => {
                let req = row["arguments"][0]["text"]
                    .as_str()
                    .or_else(|| row["requestPath"].as_str())
                    .or_else(|| row["requestName"].as_str());
                if let (Some(seq), Some(request @ ("periodTimes/0" | "gamespeed"))) =
                    (row["sequence"].as_i64(), req)
                {
                    self.pending_calls
                        .insert(seq, (binding.to_string(), request.to_string()));
                }
            }
            Some("result") => {
                let Some((caller, request)) = row["callSequence"]
                    .as_i64()
                    .and_then(|id| self.pending_calls.remove(&id))
                else {
                    return;
                };
                if caller != binding || row["success"].as_bool() == Some(false) {
                    return;
                }
                let result_val = &row["result"]["value"];
                if request == "gamespeed" {
                    if let Some(is_paused) = decode_speed_paused(result_val) {
                        self.observe_speed(time_ms, is_paused);
                    }
                } else if let Some(fraction) =
                    result_val.as_f64().filter(|v| (0.0..=1.0).contains(v))
                {
                    let sample = ClockSample {
                        time_ms,
                        seconds: fraction.rem_euclid(1.0) * 86_400.0,
                    };
                    self.samples.push(sample);
                    if let Some(page_id) = extract_row_page(row) {
                        self.by_page
                            .entry(page_id.to_string())
                            .or_default()
                            .push(sample);
                    }
                }
            }
            _ => {}
        }
    }

    /// Sort collected clock samples chronologically before querying [`Self::annotate_row`] or [`Self::advancing`].
    pub fn finish(&mut self) {
        self.samples.sort_by_key(|s| s.time_ms);
        for list in self.by_page.values_mut() {
            list.sort_by_key(|s| s.time_ms);
        }
    }

    /// Return the latest observed in-game clock in seconds (`0.0..86400.0` from `periodTimes/0`).
    pub fn latest_clock(&self) -> Option<f64> {
        self.samples.last().map(|s| s.seconds)
    }

    /// Return `Some(true)` if the game reported a paused speed, `Some(false)` if running, or `None` if unknown/contradicted.
    pub fn paused(&self) -> Option<bool> {
        let (time_ms, is_paused) = self.latest_speed?;
        if is_paused
            && self.samples.last().is_some_and(|s| s.time_ms > time_ms)
            && self.advancing() == Some(true)
        {
            return None;
        }
        Some(is_paused)
    }

    /// Return `Some(true)` if `periodTimes/0` advanced within `5000 ms`, `Some(false)` if stationary for `>= 3000 ms`, or `None` if stale.
    pub fn advancing(&self) -> Option<bool> {
        let last = self.samples.last()?;
        if self.last_seen_ms.abs_diff(last.time_ms) > BRIDGE_CLOCK_MAX_AGE_MS {
            return None;
        }
        let before = self
            .samples
            .iter()
            .rev()
            .skip(1)
            .find(|s| s.time_ms < last.time_ms)?;
        if last.time_ms.abs_diff(before.time_ms) <= BRIDGE_CLOCK_MAX_AGE_MS
            && (last.seconds - before.seconds).abs() > f64::EPSILON
        {
            return Some(true);
        }
        let mut same_since = last.time_ms;
        for sample in self.samples.iter().rev().skip(1) {
            if (sample.seconds - last.seconds).abs() > f64::EPSILON
                || same_since.abs_diff(sample.time_ms) > BRIDGE_CLOCK_MAX_AGE_MS
            {
                break;
            }
            same_since = sample.time_ms;
        }
        (last.time_ms.saturating_sub(same_since) >= BRIDGE_CLOCK_STALL_MS).then_some(false)
    }

    /// Annotate a bridge JSON record with `gameClockSeconds`, `gameClockSource`, and `gameClockAgeMs`
    /// from the nearest `periodTimes/0` sample within `5000 ms`.
    pub fn annotate_row(&self, row: &mut JsonValue) {
        let Some(time_ms) = row["time"].as_i64() else {
            return;
        };
        let sample = extract_row_page(row)
            .and_then(|p| self.by_page.get(p))
            .and_then(|list| nearest_clock_sample(list, time_ms))
            .or_else(|| nearest_clock_sample(&self.samples, time_ms));
        if let Some(sample) = sample {
            row["gameClockSeconds"] = json!(sample.seconds);
            row["gameClockSource"] = json!("ui_bridge.periodTimes/0");
            row["gameClockAgeMs"] = json!(time_ms - sample.time_ms);
        }
    }

    fn observe_speed(&mut self, time_ms: i64, is_paused: bool) {
        if self
            .latest_speed
            .is_none_or(|(prev_ms, _)| time_ms >= prev_ms)
        {
            self.latest_speed = Some((time_ms, is_paused));
        }
    }
}

fn extract_row_page(row: &JsonValue) -> Option<&str> {
    row["page"].as_str().or_else(|| row["binding"].as_str())
}

fn decode_speed_paused(speed: &JsonValue) -> Option<bool> {
    let code = speed.as_u64().or_else(|| {
        speed
            .as_f64()
            .filter(|v| v.fract() == 0.0 && *v >= 0.0)
            .map(|v| v as u64)
    })?;
    if code == UI_SPEED_VALUE_PAUSED {
        Some(true)
    } else if UI_SPEED_VALUES_RUNNING.contains(&code) {
        Some(false)
    } else {
        None
    }
}

fn nearest_clock_sample(samples: &[ClockSample], time_ms: i64) -> Option<ClockSample> {
    let idx = samples.partition_point(|s| s.time_ms <= time_ms);
    idx.checked_sub(1)
        .and_then(|i| samples.get(i))
        .into_iter()
        .chain(samples.get(idx))
        .min_by_key(|s| s.time_ms.abs_diff(time_ms))
        .copied()
        .filter(|s| s.time_ms.abs_diff(time_ms) <= BRIDGE_CLOCK_MAX_AGE_MS)
}

/// Classification of messages emitted by `ChromeClientEA::addMessageToConsole` (`EAWebKit.dll` 12.4.2.2.2).
#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    Hash,
    Serialize,
    DeserializeFromStr,
    EnumString,
    IntoStaticStr,
    VariantArray,
)]
#[strum(serialize_all = "lowercase", ascii_case_insensitive)]
#[serde(rename_all = "lowercase")]
pub enum EaWebKitConsoleKind {
    /// `JSError: <source> (<line>): <message>`.
    JsError,
    /// `JSWarning: <source> (<line>): <message>`.
    JsWarning,
    /// `JS: <source> (<line>): <message>`.
    JsInfo,
    /// Unprefixed `EAWebKitClient::DebugLog` diagnostic text.
    RawDebug,
}

impl EaWebKitConsoleKind {
    /// Map this EAWebKit console category to the central [`LogLevel`].
    pub const fn to_log_level(self) -> LogLevel {
        match self {
            Self::JsError => LogLevel::Error,
            Self::JsWarning => LogLevel::Warning,
            Self::JsInfo => LogLevel::Info,
            Self::RawDebug => LogLevel::Debug,
        }
    }
}

/// Parsed `EAWebKitClient::DebugLog(DebugLogInfo&)` console message emitted by `ChromeClientEA::addMessageToConsole`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EaWebKitDebugLogMessage {
    /// Console message category (`JSError`, `JSWarning`, `JS`, or raw debug).
    pub kind: EaWebKitConsoleKind,
    /// Optional script source URI (for example `"game://localhost/resource/SCRUI_min"`).
    pub source: Option<String>,
    /// Optional 1-based line number inside `source`.
    pub line: Option<u32>,
    /// Redacted message body.
    pub message: String,
}

impl EaWebKitDebugLogMessage {
    /// Parse a line formatted by `ChromeClientEA::addMessageToConsole`:
    /// `JSWarning: <source> (<line>): <message>`, `JSError: <source> (<line>): <message>`, or `JS: <source> (<line>): <message>`.
    pub fn parse(raw: &str) -> Self {
        let trimmed = truncate_utf8_str(raw.trim(), MAX_CAPTURED_LOG_BYTES);
        let (kind, rest) = if let Some(r) = trimmed.strip_prefix("JSError: ") {
            (EaWebKitConsoleKind::JsError, r)
        } else if let Some(r) = trimmed.strip_prefix("JSWarning: ") {
            (EaWebKitConsoleKind::JsWarning, r)
        } else if let Some(r) = trimmed.strip_prefix("JS: ") {
            (EaWebKitConsoleKind::JsInfo, r)
        } else {
            return Self {
                kind: EaWebKitConsoleKind::RawDebug,
                source: None,
                line: None,
                message: redact_sensitive_text(trimmed),
            };
        };

        if let Some((source_and_line, msg)) = rest.split_once("): ")
            && let Some((src, line_str)) = source_and_line.rsplit_once(" (")
            && let Ok(line_num) = line_str.parse::<u32>()
        {
            return Self {
                kind,
                source: Some(redact_sensitive_text(src)),
                line: Some(line_num),
                message: redact_sensitive_text(msg),
            };
        }

        Self {
            kind,
            source: None,
            line: None,
            message: redact_sensitive_text(rest),
        }
    }

    /// Render a compact redacted summary suitable for the central log pipeline.
    pub fn formatted_summary(&self) -> String {
        match (&self.source, self.line) {
            (Some(src), Some(line)) => format!("{src}:{line}: {}", self.message),
            _ => self.message.clone(),
        }
    }
}

/// Extension helper feeding [`EaWebKitDebugLogMessage`] directly into [`simoder_core::DiagnosticLogPipeline`].
pub fn capture_eawebkit_to_pipeline(
    pipeline: &DiagnosticLogPipeline,
    timestamp_ms: u64,
    thread_id: u32,
    raw_line: &str,
) -> EaWebKitDebugLogMessage {
    let parsed = EaWebKitDebugLogMessage::parse(raw_line);
    let _ = pipeline.push(
        timestamp_ms,
        LogSourceType::Game,
        "EAWebKit::DebugLog",
        parsed.kind.to_log_level(),
        thread_id,
        &parsed.formatted_summary(),
    );
    parsed
}

/// Validate that a proposed `SimCity.exe` RVA for `EAWebKit` console logging does NOT detour the shared
/// `ret 4` no-op stub (`0x81E7C0`) shared across unrelated `EAWebKitClient` callbacks.
pub fn validate_eawebkit_log_hook_rva(exe_rva: u32) -> Result<(), CrashError> {
    if exe_rva == RETAIL_EXE_SHARED_RET4_NOOP_RVA {
        return Err(CrashError::UnsafeHookTarget(format!(
            "EXE RVA {exe_rva:#010X} is the shared 'ret 4' stub used by GetLocalizedString/NetworkError/CursorMoved/JSMessageBox; hook client vtable +0x0C or ChromeClientEA::addMessageToConsole (DLL RVA 0x5AC490) instead"
        )));
    }
    Ok(())
}
