//! Atomic persistent storage for ordered enabled mod IDs in `state.toon`.

use std::collections::HashSet;
use std::fs;
use std::io::Write as _;
use std::path::Path;

use serde::{Deserialize, Serialize};
use simoder_formats::{load_toon_file_typed, toon_to_string};
use tempfile::NamedTempFile;

use crate::{config, types::*};

/// Strict TOON schema for `state.toon`, recording active mod IDs in activation order.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PersistedEnabledState {
    /// Ordered list of active mod IDs (`enabled[N]: a,b`).
    pub enabled: Vec<String>,
}

/// Load and validate ordered enabled mod IDs from `state.toon`.
///
/// Returns an empty vector when `path` does not exist. Rejects malformed TOON syntax,
/// unknown keys, invalid mod IDs via [`is_valid_mod_id`], and duplicate entries.
pub fn load_enabled_state(path: &Path) -> Result<Vec<String>, ModError> {
    if !path.try_exists()? {
        return Ok(Vec::new());
    }
    let state: PersistedEnabledState = load_toon_file_typed(path)?;
    validate_enabled_list(&state.enabled)?;
    Ok(state.enabled)
}

/// Validate and atomically persist `enabled` mod IDs to `state.toon`.
///
/// Enforces [`config::MAX_ENABLED_STATE_IDS`], rejects invalid or duplicate IDs, creates missing parent
/// directories, and publishes via [`tempfile::NamedTempFile::persist`].
pub fn save_enabled_state(path: &Path, enabled: &[String]) -> Result<(), ModError> {
    if enabled.len() > config::MAX_ENABLED_STATE_IDS {
        return Err(ModError::ManifestValidation(format!(
            "enabled state exceeds maximum entry count ({})",
            config::MAX_ENABLED_STATE_IDS
        )));
    }
    validate_enabled_list(enabled)?;
    let mut encoded = toon_to_string(&PersistedEnabledState {
        enabled: enabled.to_vec(),
    })
    .map_err(|err| ModError::Format(err.into()))?;
    encoded.push('\n');

    let parent = match path.parent() {
        Some(dir) if !dir.as_os_str().is_empty() => {
            fs::create_dir_all(dir)?;
            dir
        }
        _ => Path::new("."),
    };
    let mut temp = NamedTempFile::new_in(parent)?;
    temp.write_all(encoded.as_bytes())?;
    temp.as_file().sync_all()?;
    temp.persist(path).map_err(|err| ModError::Io(err.error))?;
    Ok(())
}

fn validate_enabled_list(enabled: &[String]) -> Result<(), ModError> {
    let mut seen = HashSet::with_capacity(enabled.len());
    for id in enabled {
        if !is_valid_mod_id(id) {
            return Err(ModError::InvalidModId(id.clone()));
        }
        if !seen.insert(id.as_str()) {
            return Err(ModError::ManifestValidation(format!(
                "state.toon contains duplicate mod ID '{id}'"
            )));
        }
    }
    Ok(())
}
