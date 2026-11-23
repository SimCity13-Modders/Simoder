//! Authoritative `Utils.IsCityLoaded` query, `EcoGame` script cache keys, and retail UI protocol codecs.

use serde::{Deserialize, Serialize};
use serde_json::Value as JsonValue;
use simoder_core::fnv1_name_hash;

use crate::config::MAX_UI_COMMAND_BYTES;
use crate::types::ScriptError;

/// Factual state of the host game city context (explicitly distinct from VM or base readiness).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum CityContextState {
    /// Active city simulation context verified by game hooks.
    Loaded {
        /// Human-readable active city name.
        city_name: String,
        /// Authoritative host city context revision counter.
        revision: u64,
    },
    /// Verified unloaded city state (for example region view or main menu).
    NotLoaded,
    /// City context state has never been observed yet.
    Unknown,
    /// Game bridge initialization is in progress and cannot yet attest city state.
    NotReady,
    /// Prior city observation was invalidated by transition/reload and awaiting fresh confirmation.
    Stale,
}

/// Reason why `Utils.IsCityLoaded` could not attest a qualified boolean verdict.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum CityStateUnavailableReason {
    /// Authoritative context probe has not reported any city observation yet.
    Unknown,
    /// Host city bridge has not finished initialization.
    NotReady,
    /// Previous city context invalidated; waiting on next verified observation.
    Stale,
}

/// Result returned by `Utils.IsCityLoaded` preserving qualified `false` vs unavailable states (`Simoder-Base#9`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum CityLoadedVerdict {
    /// Qualified boolean city-loaded truth directly observed from authoritative game state.
    Qualified(bool),
    /// State is unknown, not ready, or stale (callers must never collapse this to `false`).
    Unavailable(CityStateUnavailableReason),
}

/// Convenience store backing `Utils.IsCityLoaded` without inferring city presence from Base or UI readiness.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthoritativeCityState {
    state: CityContextState,
}

impl Default for AuthoritativeCityState {
    fn default() -> Self {
        Self {
            state: CityContextState::Unknown,
        }
    }
}

impl AuthoritativeCityState {
    /// Create a city state store defaulting explicitly to [`CityContextState::Unknown`].
    pub fn new() -> Self {
        Self::default()
    }

    /// Update the authoritative game city context observation.
    pub fn set_state(&mut self, state: CityContextState) {
        self.state = state;
    }

    /// Query `Utils.IsCityLoaded`, distinguishing qualified `false` from `Unknown`/`NotReady`/`Stale`.
    pub fn is_city_loaded(&self) -> CityLoadedVerdict {
        match &self.state {
            CityContextState::Loaded { .. } => CityLoadedVerdict::Qualified(true),
            CityContextState::NotLoaded => CityLoadedVerdict::Qualified(false),
            CityContextState::Unknown => {
                CityLoadedVerdict::Unavailable(CityStateUnavailableReason::Unknown)
            }
            CityContextState::NotReady => {
                CityLoadedVerdict::Unavailable(CityStateUnavailableReason::NotReady)
            }
            CityContextState::Stale => {
                CityLoadedVerdict::Unavailable(CityStateUnavailableReason::Stale)
            }
        }
    }
}

/// Ordered add-on version descriptor participating in the retail `EcoGame::FindLoadedScripts` key.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct EcoGameAddOnKey {
    /// EcoGame add-on numeric identifier (`mID`).
    pub addon_id: u32,
    /// Ordered add-on version (`mVersions`).
    pub version: u32,
}

/// Retail `EcoGame::FindLoadedScripts` cache key tuple (`(gameID, currentVersion, addOns)`).
///
/// Filesystem path, mtime, and SHA-256 digests do NOT participate in the engine cache key, so
/// HotReload requires invalidating cached entries or incrementing `current_version`.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct EcoGameScriptCacheKey {
    /// Game SKU identifier passed to `EcoGame::FindLoadedScripts`.
    pub game_id: u32,
    /// Active engine rule/script generation version counter.
    pub current_version: u32,
    /// Ordered sequence of active add-on ID/version pairs.
    pub addons: Vec<EcoGameAddOnKey>,
}

impl EcoGameScriptCacheKey {
    /// Return `true` when transitioning to `next` invalidates the engine script cache natively.
    pub fn needs_invalidation(&self, next: &Self) -> bool {
        self != next
    }

    /// Bump `current_version` safely so HotReload invalidates `EcoGame::FindLoadedScripts` without
    /// relying on file timestamps or path edits.
    pub fn bump_version_for_hot_reload(&mut self) -> Result<u32, ScriptError> {
        let next = self.current_version.checked_add(1).ok_or_else(|| {
            ScriptError::SchedulerError("EcoGame cache current_version exhausted u32 range".into())
        })?;
        self.current_version = next;
        Ok(next)
    }
}

/// Validated `window.Client` command string preserving exact command/argument syntax verbatim.
///
/// Retail dispatch resolves command names via GlassBox 32-bit FNV-1 (`0x8C73F0`) without
/// lossy `/` tokenization.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UiCommand(String);

impl UiCommand {
    /// Validate a UI bridge command string (`1..=16 KiB`, no interior NUL bytes), preserving verbatim content.
    pub fn new(raw: impl Into<String>) -> Result<Self, ScriptError> {
        let raw = raw.into();
        if raw.is_empty() {
            return Err(ScriptError::InvalidConfig("UI command must not be empty".into()));
        }
        if raw.len() > MAX_UI_COMMAND_BYTES {
            return Err(ScriptError::InvalidConfig(format!(
                "UI command length {} exceeds max {} bytes",
                raw.len(),
                MAX_UI_COMMAND_BYTES
            )));
        }
        if raw.as_bytes().contains(&0) {
            return Err(ScriptError::InvalidConfig(
                "UI command must not contain NUL bytes".into(),
            ));
        }
        Ok(Self(raw))
    }

    /// Borrow the verbatim command text.
    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// Extract the command verb prefix before optional whitespace/slash delimiters and compute its FNV-1 hash.
    pub fn command_name_hash(&self) -> u32 {
        let head = self
            .0
            .split(['/', '?', '&', ' ', '\t'])
            .next()
            .unwrap_or(self.0.as_str());
        fnv1_name_hash(head)
    }
}

/// Direct UI push event transmitted across `ReceiveGameEventFromClient` (`ID` or `[ID, data]`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum UiClientEvent {
    /// Scalar numeric event identifier without payload (`1234` or `[1234]`).
    Signal {
        /// Numeric UI event ID.
        event_id: u32,
    },
    /// Numeric event identifier accompanied by a structured JSON payload (`[1234, data]`).
    Payload {
        /// Numeric UI event ID.
        event_id: u32,
        /// Structured JSON payload delivered to `scrui`.
        data: JsonValue,
    },
}

impl UiClientEvent {
    /// Return the numeric UI event identifier.
    pub const fn event_id(&self) -> u32 {
        match self {
            Self::Signal { event_id } | Self::Payload { event_id, .. } => *event_id,
        }
    }

    /// Decode a wire JSON push event (`number`, `[number]`, or `[number, data]`).
    pub fn from_json_value(value: &JsonValue) -> Result<Self, ScriptError> {
        if let Some(id) = value.as_u64() {
            let event_id = u32::try_from(id).map_err(|_| {
                ScriptError::InvalidConfig(format!("UI event ID {id} overflows u32"))
            })?;
            return Ok(Self::Signal { event_id });
        }
        let Some(items) = value.as_array() else {
            return Err(ScriptError::InvalidConfig(
                "UI client event must be a u32 ID or [id] / [id, data] array".into(),
            ));
        };
        let Some(id_val) = items.first().and_then(JsonValue::as_u64) else {
            return Err(ScriptError::InvalidConfig(
                "UI client event array must begin with a u32 event ID".into(),
            ));
        };
        let event_id = u32::try_from(id_val).map_err(|_| {
            ScriptError::InvalidConfig(format!("UI event ID {id_val} overflows u32"))
        })?;
        match items.len() {
            1 => Ok(Self::Signal { event_id }),
            2 => Ok(Self::Payload {
                event_id,
                data: items[1].clone(),
            }),
            other => Err(ScriptError::InvalidConfig(format!(
                "UI client event array length {other} must be 1 or 2"
            ))),
        }
    }
}
