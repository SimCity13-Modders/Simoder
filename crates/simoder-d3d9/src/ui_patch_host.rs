#![forbid(unsafe_code)]

//! Generic declarative DOM patch transport DTOs and embedded strict-ES5 applicator script
//! executed inside verified `EAWebKit` view contexts (`SimCity.exe 10.3.6.0`).

use serde::{Deserialize, Serialize};
use simoder_mods::{UiPatchDescriptor, UiPatchEvent, UiViewObservation};

/// Generic pure-ES5 EAWebKit DOM patch reconciler source (contains zero game-screen business logic).
pub const UI_PATCH_APPLICATOR_ES5: &str = include_str!("ui_patch_applicator.js");

/// Read-only view probe packet sent to identify an active ready EAWebKit view without mutating DOM.
pub const UI_OBSERVE_PACKET_JSON: &str = r#"{"op":"observe"}"#;

/// Declarative synchronization payload dispatched from Rust to [`UI_PATCH_APPLICATOR_ES5`].
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UiSyncRequest<'a> {
    /// Wire operation (`"sync"`).
    pub op: &'static str,
    /// Monotonic UI composition revision owned by [`simoder_mods::UiPatchRegistry`].
    pub revision: u64,
    /// Ordered composed patches ready to render for the active phase.
    pub patches: &'a [UiPatchDescriptor],
}

/// Reply returned by [`UI_PATCH_APPLICATOR_ES5`] containing current view observation and user events.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UiSyncReply {
    /// Whether execution succeeded inside the EAWebKit view context.
    pub ok: bool,
    /// Observed current UI phase, active view identifiers, and game locale.
    #[serde(default)]
    pub observation: Option<UiViewObservation>,
    /// Bounded user interaction events raised since the previous sync tick.
    #[serde(default)]
    pub events: Vec<UiPatchEvent>,
    /// Optional JS-side failure message when `ok == false`.
    #[serde(default)]
    pub error: Option<String>,
}

/// Serialize a bounded [`UiSyncRequest`] packet for EAWebKit dispatch.
pub fn build_sync_packet(revision: u64, patches: &[UiPatchDescriptor]) -> Result<String, String> {
    serde_json::to_string(&UiSyncRequest {
        op: "sync",
        revision,
        patches,
    })
    .map_err(|e| format!("failed serializing UI sync packet: {e}"))
}

/// Parse and validate a JSON reply emitted by [`UI_PATCH_APPLICATOR_ES5`].
pub fn parse_sync_reply(raw_json: &str) -> Result<UiSyncReply, String> {
    let reply: UiSyncReply = serde_json::from_str(raw_json)
        .map_err(|e| format!("invalid UI sync JSON reply: {e}"))?;
    if !reply.ok {
        return Err(reply
            .error
            .unwrap_or_else(|| "EAWebKit UI sync failed without diagnostic".into()));
    }
    Ok(reply)
}
