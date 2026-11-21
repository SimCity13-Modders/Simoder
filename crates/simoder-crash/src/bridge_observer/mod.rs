//! Backend <-> Frontend (`SimCity.exe` <-> `EAWebKit.dll`) runtime bridge observer:
//! - [`BridgeTime`] simulation clock & pause tracker,
//! - 14-channel pre-limiter request classifier, signature pairing, JSON state diffing, and [`CaptureQualityReport`],
//! - [`EaWebKitDebugLogMessage`] console parser & vtable hook safety verifier.

pub mod bridge_time;

use std::collections::{BTreeMap, HashMap, VecDeque};

pub use bridge_time::*;
use serde::{Deserialize, Serialize};
use serde_json::Value as JsonValue;
use serde_with::DeserializeFromStr;
use simoder_core::{redact_sensitive_text, truncate_utf8_str};
use strum::{EnumString, IntoStaticStr, VariantArray};

use crate::config::{
    DEFAULT_BRIDGE_CLOCK_QUOTA, DEFAULT_BRIDGE_COMMAND_QUOTA, DEFAULT_BRIDGE_EVENT_QUOTA,
    DEFAULT_BRIDGE_MUTATION_QUOTA, DEFAULT_BRIDGE_QUERY_QUOTA, DEFAULT_BRIDGE_REGION_QUOTA,
    DEFAULT_BRIDGE_TEMPLATE_QUOTA, MAX_BRIDGE_CHANNEL_QUEUE,
};

/// All 14 independent UI <-> Native bridge sampling channels.
#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    PartialOrd,
    Ord,
    Hash,
    Serialize,
    DeserializeFromStr,
    EnumString,
    IntoStaticStr,
    VariantArray,
)]
#[strum(serialize_all = "camelCase", ascii_case_insensitive)]
#[serde(rename_all = "camelCase")]
pub enum BridgeChannel {
    /// `window.Client.PostGameCommand`.
    Command,
    /// Ordinary high-frequency read polling via `window.Client.RequestGameData`.
    Query,
    /// `periodTimes` and `gamespeed` clock queries (`12/s` dedicated budget).
    Clock,
    /// Paired `onLeave` results for `Clock` queries.
    ClockResult,
    /// Unclassified native `invokeMethod` calls.
    Other,
    /// `ChromeClientEA::addMessageToConsole` (`EAWebKit.dll` `0x5AC490`) messages.
    Console,
    /// Paired `onLeave` results for ordinary `Query` calls.
    Result,
    /// Engine-to-UI push events via `ReceiveGameEventFromClient` (`EXE RVA 0x62F500`).
    Event,
    /// State-mutating requests via `RequestGameData` (`SaveLocalGame`, `SaveRegionAs`, `UpdateBoxName`, `UpdateRegionName`, `CreateRegion`, `CreateBox`).
    Mutation,
    /// Paired `onLeave` results for `Mutation` requests.
    MutationResult,
    /// `RegionTemplates` export queries (`2/s` dedicated budget).
    Template,
    /// Paired `onLeave` results for `Template` queries.
    TemplateResult,
    /// Region creation/identity lifecycle queries (`CreateRegion`, `CreateBox`, `GetRegionDetails`, `LatestBox`, `activeBoxId`, `ActiveCityUID`, `RegionID`) (`4/s` dedicated budget).
    Region,
    /// Paired `onLeave` results for `Region` queries.
    RegionResult,
}

impl BridgeChannel {
    /// Classify a UI->Native `invokeMethod` call BEFORE ordinary `Query` rate limiting.
    ///
    /// Note that `CreateRegion` and `CreateBox` are checked under `Mutation` when explicitly routed as state writes,
    /// while `GetRegionDetails`, `LatestBox`, `activeBoxId`, `ActiveCityUID`, and `RegionID` route to `Region`.
    pub fn classify_request(method_name: &str, request_verb: &str) -> Self {
        if method_name.eq_ignore_ascii_case("PostGameCommand") {
            return Self::Command;
        }
        if !method_name.eq_ignore_ascii_case("RequestGameData") {
            return Self::Other;
        }
        let head = request_verb
            .split('/')
            .next()
            .unwrap_or(request_verb)
            .trim();
        match head {
            "periodTimes" | "gamespeed" => Self::Clock,
            "RegionTemplates" => Self::Template,
            "GetRegionDetails" | "LatestBox" | "activeBoxId" | "ActiveCityUID" | "RegionID" => {
                Self::Region
            }
            "SaveLocalGame"
            | "SaveRegionAs"
            | "UpdateBoxName"
            | "UpdateRegionName"
            | "CreateRegion"
            | "CreateBox"
            | "ClaimBox" => Self::Mutation,
            _ => Self::Query,
        }
    }

    /// Return the paired `onLeave` result channel corresponding to a `RequestGameData` request channel.
    pub const fn paired_result_channel(self) -> Self {
        match self {
            Self::Clock | Self::ClockResult => Self::ClockResult,
            Self::Template | Self::TemplateResult => Self::TemplateResult,
            Self::Region | Self::RegionResult => Self::RegionResult,
            Self::Mutation | Self::MutationResult => Self::MutationResult,
            _ => Self::Result,
        }
    }

    /// Default per-second admission quota configured for this channel.
    pub const fn default_quota(self) -> usize {
        match self {
            Self::Region | Self::RegionResult => DEFAULT_BRIDGE_REGION_QUOTA,
            Self::Template | Self::TemplateResult => DEFAULT_BRIDGE_TEMPLATE_QUOTA,
            Self::Clock | Self::ClockResult => DEFAULT_BRIDGE_CLOCK_QUOTA,
            Self::Query | Self::Result => DEFAULT_BRIDGE_QUERY_QUOTA,
            Self::Mutation | Self::MutationResult => DEFAULT_BRIDGE_MUTATION_QUOTA,
            Self::Event => DEFAULT_BRIDGE_EVENT_QUOTA,
            Self::Command | Self::Other | Self::Console => DEFAULT_BRIDGE_COMMAND_QUOTA,
        }
    }
}

/// Direction of a correlated bridge diagnostic record (`CRASH-005`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum BridgeDirection {
    /// UI JavaScript -> native `SimCity.exe` request/command (`onEnter`).
    UiToNative,
    /// Native `SimCity.exe` -> UI JavaScript return value (`onLeave`) or pushed event.
    NativeToUi,
}

/// Pairing diagnosis computed across `invoke` and `result` records.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum BridgePairingIssue {
    /// Request has not received its `onLeave` result yet, or a result had no matching request.
    Unpaired,
    /// Multiple requests or results shared the same `(binding, page, name, requestName, sequence)` signature.
    Ambiguous,
}

/// Sanitized, correlated bridge trace record with state-diff paths.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct BridgeTraceRecord {
    /// Monotonic sequence number assigned on request entry (or `call_sequence` on paired response).
    pub sequence: u64,
    /// Dedicated sampling channel.
    pub channel: BridgeChannel,
    /// Call direction.
    pub direction: BridgeDirection,
    /// Redacted verb/event identifier (for example `"SaveLocalGame"` or `"UpdateBoxName/8/NoweMiasto"`).
    pub operation: String,
    /// OS thread ID where the bridge boundary was crossed.
    pub thread_id: u32,
    /// Optional mod identity and generation when initiated by or attributed to a mod.
    pub owner_mod: Option<(String, u64)>,
    /// Timestamp in milliseconds.
    pub timestamp_ms: u64,
    /// Optional round-trip duration in microseconds on response completion.
    pub duration_us: Option<u64>,
    /// Whether the bridge call succeeded (`true`) or returned an error/false verdict (`false`).
    pub succeeded: bool,
    /// Optional structured return value (used for consecutive JSON diffing).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub result_value: Option<JsonValue>,
    /// JSON paths (`$`, `$.amount`, etc.) that changed compared to the previous successful call for the same operation.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub changed_paths: Vec<String>,
    /// Pairing issue (`Unpaired` until paired with a unique `result`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pairing_issue: Option<BridgePairingIssue>,
}

/// Per-channel capture quality metrics.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChannelQualityStats {
    /// Channel identifier.
    pub channel: BridgeChannel,
    /// Total observed hits before quota admission.
    pub hits: u64,
    /// Total records accepted into this channel.
    pub accepted: u64,
    /// Total records dropped by this channel's rate/queue limit.
    pub dropped: u64,
    /// Currently queued records in this channel's ring buffer.
    pub queued: usize,
}

/// Session-level capture quality summary.
///
/// Top-level `dropped` equals the sum of channel drops (callers must never add `top_level_dropped` and channel `dropped` together).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CaptureQualityReport {
    /// Total accepted records across all channels.
    pub reported: u64,
    /// Total dropped records across all channels.
    pub dropped: u64,
    /// Count of requests currently lacking a paired response (`Unpaired`).
    pub incomplete_requests: usize,
    /// Per-channel statistics breakdown.
    pub channels: Vec<ChannelQualityStats>,
}

struct ChannelBucket {
    records: VecDeque<BridgeTraceRecord>,
    quota_per_window: usize,
    used_in_window: usize,
    hits_count: u64,
    accepted_count: u64,
    dropped_count: u64,
}

impl ChannelBucket {
    fn new(quota_per_window: usize) -> Self {
        Self {
            records: VecDeque::with_capacity(MAX_BRIDGE_CHANNEL_QUEUE),
            quota_per_window,
            used_in_window: 0,
            hits_count: 0,
            accepted_count: 0,
            dropped_count: 0,
        }
    }

    fn try_admit(&mut self, record: BridgeTraceRecord, bypass_window_quota: bool) -> bool {
        self.hits_count = self.hits_count.saturating_add(1);
        if !bypass_window_quota && self.used_in_window >= self.quota_per_window {
            self.dropped_count = self.dropped_count.saturating_add(1);
            return false;
        }
        if !bypass_window_quota {
            self.used_in_window += 1;
        }
        self.accepted_count = self.accepted_count.saturating_add(1);
        if self.records.len() >= MAX_BRIDGE_CHANNEL_QUEUE {
            let _ = self.records.pop_front();
        }
        self.records.push_back(record);
        true
    }
}

/// Multi-channel Backend <-> Frontend bridge correlation tracker.
pub struct BridgeCorrelationTracker {
    next_sequence: u64,
    buckets: BTreeMap<BridgeChannel, ChannelBucket>,
    previous_results: HashMap<String, JsonValue>,
    clock: BridgeTime,
}

impl Default for BridgeCorrelationTracker {
    fn default() -> Self {
        Self::new()
    }
}

impl BridgeCorrelationTracker {
    /// Construct a tracker with all 14 channels initialized to their verified per-window quotas.
    pub fn new() -> Self {
        let mut buckets = BTreeMap::new();
        for &ch in BridgeChannel::VARIANTS {
            buckets.insert(ch, ChannelBucket::new(ch.default_quota()));
        }
        Self {
            next_sequence: 1,
            buckets,
            previous_results: HashMap::new(),
            clock: BridgeTime::new(),
        }
    }

    /// Borrow the integrated [`BridgeTime`] simulation clock and pause state tracker.
    pub const fn bridge_time(&self) -> &BridgeTime {
        &self.clock
    }

    /// Mutably borrow the integrated [`BridgeTime`] tracker.
    pub fn bridge_time_mut(&mut self) -> &mut BridgeTime {
        &mut self.clock
    }

    /// Reset per-window rate quotas while preserving buffered records and cumulative drop statistics.
    pub fn reset_rate_window(&mut self) {
        for bucket in self.buckets.values_mut() {
            bucket.used_in_window = 0;
        }
    }

    /// Record an outgoing UI-to-native request (`PostGameCommand` or `RequestGameData`); returns `Some(sequence)` if admitted.
    pub fn record_request(
        &mut self,
        method_name: &str,
        raw_verb: &str,
        thread_id: u32,
        owner_mod: Option<(String, u64)>,
        timestamp_ms: u64,
    ) -> Option<u64> {
        let channel = BridgeChannel::classify_request(method_name, raw_verb);
        let seq = self.next_sequence;
        self.next_sequence = self.next_sequence.saturating_add(1);

        let sanitized_op = sanitize_bridge_operation(raw_verb);
        let is_query_expecting_result = method_name.eq_ignore_ascii_case("RequestGameData");
        let rec = BridgeTraceRecord {
            sequence: seq,
            channel,
            direction: BridgeDirection::UiToNative,
            operation: sanitized_op,
            thread_id,
            owner_mod,
            timestamp_ms,
            duration_us: None,
            succeeded: true,
            result_value: None,
            changed_paths: Vec::new(),
            pairing_issue: is_query_expecting_result.then_some(BridgePairingIssue::Unpaired),
        };
        let admitted = self
            .buckets
            .get_mut(&channel)
            .is_some_and(|b| b.try_admit(rec, false));
        admitted.then_some(seq)
    }

    /// Pair a native-to-UI response with a previously admitted `sequence` without being starved by query rate limits.
    pub fn record_response(
        &mut self,
        sequence: u64,
        request_channel: BridgeChannel,
        timestamp_ms: u64,
        duration_us: u64,
        succeeded: bool,
    ) -> bool {
        self.record_response_with_value(
            sequence,
            request_channel,
            timestamp_ms,
            duration_us,
            succeeded,
            None,
        )
    }

    /// Pair a native-to-UI response carrying an optional decoded `result_value`, automatically computing
    /// JSON diff paths (`changed_paths`) against the previous successful call.
    pub fn record_response_with_value(
        &mut self,
        sequence: u64,
        request_channel: BridgeChannel,
        timestamp_ms: u64,
        duration_us: u64,
        succeeded: bool,
        result_value: Option<JsonValue>,
    ) -> bool {
        let mut matched_count = 0usize;
        let mut meta = None;
        if let Some(req_bucket) = self.buckets.get_mut(&request_channel) {
            for r in req_bucket
                .records
                .iter_mut()
                .filter(|r| r.sequence == sequence && r.direction == BridgeDirection::UiToNative)
            {
                matched_count += 1;
                r.pairing_issue = None;
                meta = Some((r.operation.clone(), r.thread_id, r.owner_mod.clone()));
            }
            if matched_count > 1 {
                for r in req_bucket
                    .records
                    .iter_mut()
                    .filter(|r| r.sequence == sequence && r.direction == BridgeDirection::UiToNative)
                {
                    r.pairing_issue = Some(BridgePairingIssue::Ambiguous);
                }
            }
        }

        let (operation, thread_id, owner_mod) =
            meta.unwrap_or_else(|| (format!("seq:{sequence}"), 0, None));
        let pairing_issue = match matched_count {
            1 => None,
            0 => Some(BridgePairingIssue::Unpaired),
            _ => Some(BridgePairingIssue::Ambiguous),
        };

        let mut changed_paths = Vec::new();
        if succeeded && let Some(ref val) = result_value {
            if let Some(prev) = self.previous_results.get(&operation) {
                diff_json_paths(prev, val, "$", 0, &mut changed_paths);
            }
            self.previous_results.insert(operation.clone(), val.clone());
        }

        let result_channel = request_channel.paired_result_channel();
        let rec = BridgeTraceRecord {
            sequence,
            channel: result_channel,
            direction: BridgeDirection::NativeToUi,
            operation,
            thread_id,
            owner_mod,
            timestamp_ms,
            duration_us: Some(duration_us),
            succeeded,
            result_value,
            changed_paths,
            pairing_issue,
        };
        self.buckets
            .get_mut(&result_channel)
            .is_some_and(|b| b.try_admit(rec, true))
    }

    /// Record an engine-to-UI `ReceiveGameEventFromClient` notification by numeric event ID.
    pub fn record_reverse_event(
        &mut self,
        event_id: u32,
        thread_id: u32,
        timestamp_ms: u64,
    ) -> Option<u64> {
        let seq = self.next_sequence;
        self.next_sequence = self.next_sequence.saturating_add(1);
        let rec = BridgeTraceRecord {
            sequence: seq,
            channel: BridgeChannel::Event,
            direction: BridgeDirection::NativeToUi,
            operation: format!("event:{event_id}"),
            thread_id,
            owner_mod: None,
            timestamp_ms,
            duration_us: None,
            succeeded: true,
            result_value: None,
            changed_paths: Vec::new(),
            pairing_issue: None,
        };
        self.buckets
            .get_mut(&BridgeChannel::Event)
            .is_some_and(|b| b.try_admit(rec, false))
            .then_some(seq)
    }

    /// Return buffered records for `channel` (including paired responses when querying a request channel).
    pub fn records(&self, channel: BridgeChannel) -> Vec<BridgeTraceRecord> {
        let mut out: Vec<BridgeTraceRecord> = self
            .buckets
            .get(&channel)
            .map(|b| b.records.iter().cloned().collect())
            .unwrap_or_default();
        let paired = channel.paired_result_channel();
        if paired != channel
            && !matches!(
                channel,
                BridgeChannel::Command
                    | BridgeChannel::Event
                    | BridgeChannel::Console
                    | BridgeChannel::Other
            )
            && let Some(res_bucket) = self.buckets.get(&paired)
        {
            out.extend(res_bucket.records.iter().cloned());
            out.sort_by_key(|r| (r.sequence, r.direction == BridgeDirection::NativeToUi));
        }
        out
    }

    /// Return the cumulative dropped record count for `channel`.
    pub fn dropped_count(&self, channel: BridgeChannel) -> u64 {
        self.buckets.get(&channel).map_or(0, |b| b.dropped_count)
    }

    /// Compute a session-wide [`CaptureQualityReport`].
    pub fn capture_quality(&self) -> CaptureQualityReport {
        let mut reported = 0u64;
        let mut dropped = 0u64;
        let mut incomplete_requests = 0usize;
        let mut channels = Vec::with_capacity(self.buckets.len());

        for (&channel, bucket) in &self.buckets {
            reported = reported.saturating_add(bucket.accepted_count);
            dropped = dropped.saturating_add(bucket.dropped_count);
            incomplete_requests += bucket
                .records
                .iter()
                .filter(|r| {
                    r.direction == BridgeDirection::UiToNative
                        && r.pairing_issue == Some(BridgePairingIssue::Unpaired)
                })
                .count();
            channels.push(ChannelQualityStats {
                channel,
                hits: bucket.hits_count,
                accepted: bucket.accepted_count,
                dropped: bucket.dropped_count,
                queued: bucket.records.len(),
            });
        }

        CaptureQualityReport {
            reported,
            dropped,
            incomplete_requests,
            channels,
        }
    }
}

fn diff_json_paths(
    before: &JsonValue,
    after: &JsonValue,
    path: &str,
    depth: usize,
    out: &mut Vec<String>,
) {
    if before == after {
        return;
    }
    if depth < 12
        && let (Some(a), Some(b)) = (before.as_object(), after.as_object())
    {
        let mut keys: Vec<&str> = a.keys().chain(b.keys()).map(String::as_str).collect();
        keys.sort_unstable();
        keys.dedup();
        for key in keys {
            let child_path = format!("{path}.{key}");
            match (a.get(key), b.get(key)) {
                (Some(va), Some(vb)) => diff_json_paths(va, vb, &child_path, depth + 1, out),
                _ => out.push(child_path),
            }
        }
        return;
    }
    out.push(path.to_string());
}

fn sanitize_bridge_operation(raw_verb: &str) -> String {
    let truncated = truncate_utf8_str(raw_verb.trim(), 256);
    let without_json = truncated
        .split('{')
        .next()
        .unwrap_or(truncated)
        .trim_end_matches('/');
    redact_sensitive_text(without_json)
}
