//! Deterministic script PROP patch reducer enforcing per-script override limits.

use std::collections::BTreeMap;

use simoder_core::{Tgi, tgi_key};

use crate::config::MAX_PROP_OVERRIDES_PER_SCRIPT;
use crate::types::{PropCommandAction, PropertyAlias, ScriptError, ScriptPatchCommand};

/// Normalized persistent float override maintained for one `(target, property_id)` slot by a script.
#[derive(Debug, Clone, PartialEq)]
pub struct ScriptOverrideEntry {
    /// Target `0x00B1B104` PROP resource TGI.
    pub target: Tgi,
    /// Target 32-bit FNV-1 property ID.
    pub property_id: u32,
    /// Finite IEEE-754 float value written by this override.
    pub value: f32,
    /// Provenance source label (`[symbols.properties]` source or default VM API origin).
    pub source: String,
    /// Optional resolved property alias metadata when referenced by name.
    pub symbol_origin: Option<PropertyAlias>,
}

/// Merge `previous` active script overrides with a validated `commands` batch in emission order.
///
/// Rejects non-finite float values, removes cleared entries cleanly, orders surviving entries
/// deterministically by `(tgi_key(target), property_id)`, and enforces
/// [`MAX_PROP_OVERRIDES_PER_SCRIPT`].
pub fn reduce_script_patches(
    previous: &[ScriptOverrideEntry],
    commands: &[ScriptPatchCommand],
) -> Result<Vec<ScriptOverrideEntry>, ScriptError> {
    let mut fields: BTreeMap<((u32, u32, u32), u32), ScriptOverrideEntry> = BTreeMap::new();

    for entry in previous {
        if !entry.value.is_finite() {
            return Err(ScriptError::PropApiError(format!(
                "prior override on {}:{:#010x} has non-finite float {}",
                entry.target, entry.property_id, entry.value
            )));
        }
        fields.insert((tgi_key(entry.target), entry.property_id), entry.clone());
    }

    for command in commands {
        let key = (tgi_key(command.target), command.property_id);
        match command.action {
            PropCommandAction::ClearFloat => {
                fields.remove(&key);
            }
            PropCommandAction::SetFloat(value) => {
                if !value.is_finite() {
                    return Err(ScriptError::PropApiError(format!(
                        "setFloat on {}:{:#010x} requires a finite float32 (got {value})",
                        command.target, command.property_id
                    )));
                }
                let source = command
                    .symbol_origin
                    .as_ref()
                    .map_or_else(|| command.default_source.to_string(), |alias| alias.source.clone());
                fields.insert(
                    key,
                    ScriptOverrideEntry {
                        target: command.target,
                        property_id: command.property_id,
                        value,
                        source,
                        symbol_origin: command.symbol_origin.clone(),
                    },
                );
            }
        }
    }

    if fields.len() > MAX_PROP_OVERRIDES_PER_SCRIPT {
        return Err(ScriptError::PatchLimitExceeded {
            limit: MAX_PROP_OVERRIDES_PER_SCRIPT,
            actual: fields.len(),
        });
    }

    Ok(fields.into_values().collect())
}
