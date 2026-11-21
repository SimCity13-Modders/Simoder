//! Native GlassBox engine and simulation telemetry decoders ported from `Simoder-OpenSCP`:
//! - Retail `SLDelta` v4 outer envelope parser (`src/pages/runtime-observer/deltaEnvelope.ts`),
//! - `cEcoGame::LogAction` (`0x5CC130` / `0x5CC1D0`) header decoder & contextual [`RuleStreamContext`]
//!   (`docs/re/retail-rule-logging-entrypoints-20261017.md`),
//! - Verified retail `EA::Trace` pipeline entrypoints (`docs/re/mod-diagnostics-logging-20261017.md`).

use serde::{Deserialize, Serialize};
use simoder_core::{Digest, Sha256, hex};

use crate::config::{
    RETAIL_CHEAT_MANAGER_OUTPUT_VA, RETAIL_EATRACE_CONSOLE_REPORTER_VA,
    RETAIL_EATRACE_FILE_REPORTER_VA, RETAIL_EATRACE_GET_SERVER_VA, RETAIL_EATRACE_SERVER_TRACE_VA,
    RETAIL_EATRACE_TRACE_FORMATTED_VA, RULE_STREAM_RETAIL_VERSION_THRESHOLD,
    SLDELTA_RECORD_MAGIC_BE, SLDELTA_RETAIL_CONTAINER_VERSION,
};
use crate::types::CrashError;

/// One outer record decoded from a retail `SLDelta` v4 container (`Simoder-OpenSCP` `deltaEnvelope.ts`).
///
/// Note that the inner payload of an `SLDelta` v4 envelope is NOT a raw `EncodeActionHeader` stream.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SlDeltaEnvelopeRecord {
    /// Source file identifier.
    pub file_name: String,
    /// Lowercase hex SHA-256 of the entire `SLDelta` container buffer.
    pub container_sha256: String,
    /// Byte offset of this record inside the container.
    pub offset: usize,
    /// Total record byte size (`>= 24`).
    pub size: u32,
    /// Monotonic 64-bit delta record identifier (`offset + 8`, big-endian).
    pub delta_id: u64,
    /// `true` when `delta_id < previous_delta_id`.
    pub id_regressed: bool,
    /// Previous record's `delta_id`, if any.
    pub previous_delta_id: Option<u64>,
    /// Simulation tick counter recorded in the envelope header (`offset + 16`, big-endian).
    pub sim_ticks: u32,
    /// Envelope record kind (`offset + 20`, big-endian).
    pub kind: u32,
    /// Length of the inner payload in bytes (`size - 24`).
    pub payload_bytes: u32,
}

/// Parse a retail `SLDelta` v4 binary stream into validated outer envelope records (`Simoder-OpenSCP` `deltaEnvelope.ts`).
pub fn parse_sldelta_v4_envelopes(
    data: &[u8],
    file_name: &str,
) -> Result<Vec<SlDeltaEnvelopeRecord>, CrashError> {
    if data.len() < 4 {
        return Err(CrashError::RuleStreamDecode(
            "SLDelta container is shorter than 4-byte version header".into(),
        ));
    }
    let version = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
    if version != SLDELTA_RETAIL_CONTAINER_VERSION {
        return Err(CrashError::RuleStreamDecode(format!(
            "unsupported SLDelta container version {version} (expected retail v{SLDELTA_RETAIL_CONTAINER_VERSION})"
        )));
    }

    let container_sha256 = hex::encode(Sha256::digest(data));
    let mut rows = Vec::new();
    let mut offset = 4usize;
    let mut previous: Option<u64> = None;

    while offset < data.len() {
        if offset + 24 > data.len() {
            return Err(CrashError::RuleStreamDecode(format!(
                "truncated SLDelta v4 record header at byte offset {offset}"
            )));
        }
        let magic = u32::from_be_bytes([
            data[offset],
            data[offset + 1],
            data[offset + 2],
            data[offset + 3],
        ]);
        if magic != SLDELTA_RECORD_MAGIC_BE {
            return Err(CrashError::RuleStreamDecode(format!(
                "invalid SLDelta v4 record magic {magic:#010X} at byte offset {offset} (expected {SLDELTA_RECORD_MAGIC_BE:#010X})"
            )));
        }
        let size = u32::from_be_bytes([
            data[offset + 4],
            data[offset + 5],
            data[offset + 6],
            data[offset + 7],
        ]);
        let size_usize = size as usize;
        if size < 24 || offset.saturating_add(size_usize) > data.len() {
            return Err(CrashError::RuleStreamDecode(format!(
                "invalid SLDelta v4 record size {size} at byte offset {offset}"
            )));
        }
        let delta_id = u64::from_be_bytes([
            data[offset + 8],
            data[offset + 9],
            data[offset + 10],
            data[offset + 11],
            data[offset + 12],
            data[offset + 13],
            data[offset + 14],
            data[offset + 15],
        ]);
        let sim_ticks = u32::from_be_bytes([
            data[offset + 16],
            data[offset + 17],
            data[offset + 18],
            data[offset + 19],
        ]);
        let kind = u32::from_be_bytes([
            data[offset + 20],
            data[offset + 21],
            data[offset + 22],
            data[offset + 23],
        ]);
        rows.push(SlDeltaEnvelopeRecord {
            file_name: file_name.to_string(),
            container_sha256: container_sha256.clone(),
            offset,
            size,
            delta_id,
            id_regressed: previous.is_some_and(|prev| delta_id < prev),
            previous_delta_id: previous,
            sim_ticks,
            kind,
            payload_bytes: size - 24,
        });
        previous = Some(delta_id);
        offset += size_usize;
    }

    Ok(rows)
}

/// Verified retail `SimCity.exe` (`10.3.6.0`) entrypoints for the internal `EA::Trace` text logging pipeline
/// (`Simoder-OpenSCP` `docs/re/mod-diagnostics-logging-20261017.md`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct EaTraceRetailEntrypoint {
    /// Semantic name of the `EA::Trace` or `cCheatManager` function.
    pub name: &'static str,
    /// Verified retail virtual address (image base `0x00400000`).
    pub retail_va: u32,
    /// Calling convention (`"cdecl"` or `"thiscall"`).
    pub calling_convention: &'static str,
}

/// Return the 6 verified retail `EA::Trace` / `cCheatManager` observation entrypoints from `Simoder-OpenSCP`.
pub const fn retail_eatrace_entrypoints() -> [EaTraceRetailEntrypoint; 6] {
    [
        EaTraceRetailEntrypoint {
            name: "EA::Trace::TraceHelper::TraceFormatted",
            retail_va: RETAIL_EATRACE_TRACE_FORMATTED_VA,
            calling_convention: "cdecl",
        },
        EaTraceRetailEntrypoint {
            name: "EA::Trace::Server::Trace",
            retail_va: RETAIL_EATRACE_SERVER_TRACE_VA,
            calling_convention: "thiscall",
        },
        EaTraceRetailEntrypoint {
            name: "anonymous::cLogReporterFile::Report",
            retail_va: RETAIL_EATRACE_FILE_REPORTER_VA,
            calling_convention: "thiscall",
        },
        EaTraceRetailEntrypoint {
            name: "anonymous::cLogReporterDebugConsole::Report",
            retail_va: RETAIL_EATRACE_CONSOLE_REPORTER_VA,
            calling_convention: "thiscall",
        },
        EaTraceRetailEntrypoint {
            name: "SP::cCheatManager::Output",
            retail_va: RETAIL_CHEAT_MANAGER_OUTPUT_VA,
            calling_convention: "thiscall",
        },
        EaTraceRetailEntrypoint {
            name: "EA::Trace::GetServer",
            retail_va: RETAIL_EATRACE_GET_SERVER_VA,
            calling_convention: "cdecl",
        },
    ]
}

/// Decoded action header from `cEcoGame::LogAction` (`0x5CC130` / `0x5CC1D0`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct DecodedRuleActionHeader {
    /// Action identifier (`1` = `LogUnitRule`, `2` = select current `unitSlot`, `5` = `LogGlobalRule`).
    pub action_id: u8,
    /// Primary parameter (`data1`, or `u32::MAX` when sentinel `-1` was encoded).
    pub data1: u32,
    /// Secondary parameter (`data2`, or `u32::MAX` when sentinel `-1` was encoded).
    pub data2: u32,
    /// Number of bytes consumed from the big-endian stream.
    pub bytes_consumed: usize,
}

/// Decode a big-endian rule action header (`0x5CC1D0`) requiring an explicit `stream_version`
/// (`< 27` uses `0x3E`/`0x3F` data2 markers; `>= 27` uses retail `0xFE`/`0xFF` markers, allowing `62`, `63`, and `253` directly).
pub fn decode_rule_action_header(
    stream_version: Option<u32>,
    bytes: &[u8],
) -> Result<DecodedRuleActionHeader, CrashError> {
    let version = stream_version.ok_or_else(|| {
        CrashError::RuleStreamDecode(
            "explicit stream_version is required to decode rule action headers".into(),
        )
    })?;
    if bytes.len() < 4 {
        return Err(CrashError::RuleStreamDecode(
            "rule action header requires at least 4 bytes".into(),
        ));
    }

    let action_id = bytes[0];
    let raw_data1 = u16::from_be_bytes([bytes[1], bytes[2]]);
    let raw_data2 = bytes[3];
    let mut cursor = 4usize;

    let data1 = if raw_data1 == 0xFFFF {
        u32::MAX
    } else if raw_data1 == 0xFFFE {
        if bytes.len() < cursor + 4 {
            return Err(CrashError::RuleStreamDecode(
                "truncated 32-bit data1 extension in rule action header".into(),
            ));
        }
        let ext = u32::from_be_bytes([
            bytes[cursor],
            bytes[cursor + 1],
            bytes[cursor + 2],
            bytes[cursor + 3],
        ]);
        cursor += 4;
        ext
    } else {
        u32::from(raw_data1)
    };

    let (ext_marker, sentinel_marker) = if version < RULE_STREAM_RETAIL_VERSION_THRESHOLD {
        (0x3E_u8, 0x3F_u8)
    } else {
        (0xFE_u8, 0xFF_u8)
    };

    let data2 = if raw_data2 == sentinel_marker {
        u32::MAX
    } else if raw_data2 == ext_marker {
        if bytes.len() < cursor + 4 {
            return Err(CrashError::RuleStreamDecode(
                "truncated 32-bit data2 extension in rule action header".into(),
            ));
        }
        let ext = u32::from_be_bytes([
            bytes[cursor],
            bytes[cursor + 1],
            bytes[cursor + 2],
            bytes[cursor + 3],
        ]);
        cursor += 4;
        ext
    } else {
        u32::from(raw_data2)
    };

    Ok(DecodedRuleActionHeader {
        action_id,
        data1,
        data2,
        bytes_consumed: cursor,
    })
}

/// Contextual rule telemetry tracker preserving retail `action 2 (unitSlot)` across subsequent `action 1 (stateIndex)` records.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RuleStreamContext {
    current_unit_slot: Option<u32>,
}

/// High-level semantic rule telemetry event resolved by [`RuleStreamContext`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum RuleTelemetryEvent {
    /// `action 2`: updated `game+0x1908` current logged `unitSlot`.
    UnitSlotSelected {
        /// Active unit slot index.
        unit_slot: u32,
    },
    /// `action 1`: `cEcoGame::LogUnitRule` firing for `state_index` attributed to `unit_slot` (`None` if stream started mid-way without prior `action 2`).
    UnitRuleApplied {
        /// Attributed unit slot from prior `action 2` (`None` if unknown; never misattributed to slot `0`).
        unit_slot: Option<u32>,
        /// Unit-local rule state index (`data2`).
        state_index: u32,
    },
    /// `action 5`: `cEcoGame::LogGlobalRule` firing for global `state_index`.
    GlobalRuleApplied {
        /// Global bin-set rule state index.
        state_index: u32,
    },
    /// Other action code preserved verbatim.
    Other {
        /// Raw action identifier.
        action_id: u8,
        /// Primary parameter.
        data1: u32,
        /// Secondary parameter.
        data2: u32,
    },
}

impl RuleStreamContext {
    /// Create a fresh rule stream tracker with no initial unit slot assumed (`retail-rule-logging-entrypoints-20261017.md`).
    pub const fn new() -> Self {
        Self {
            current_unit_slot: None,
        }
    }

    /// Return the currently tracked `unitSlot` (`game+0x1908`), if an `action 2` has been observed.
    pub const fn current_unit_slot(&self) -> Option<u32> {
        self.current_unit_slot
    }

    /// Apply a decoded action header, updating `current_unit_slot` on `action 2` and attributing `action 1`
    /// without falsely treating `action 1`'s `data1 == 0` as unit `0`.
    pub fn observe_header(&mut self, header: DecodedRuleActionHeader) -> RuleTelemetryEvent {
        match header.action_id {
            2 => {
                self.current_unit_slot = Some(header.data1);
                RuleTelemetryEvent::UnitSlotSelected {
                    unit_slot: header.data1,
                }
            }
            1 => RuleTelemetryEvent::UnitRuleApplied {
                unit_slot: self.current_unit_slot,
                state_index: header.data2,
            },
            5 => RuleTelemetryEvent::GlobalRuleApplied {
                state_index: header.data1,
            },
            other => RuleTelemetryEvent::Other {
                action_id: other,
                data1: header.data1,
                data2: header.data2,
            },
        }
    }
}
