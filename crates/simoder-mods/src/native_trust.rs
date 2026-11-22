//! Atomic persistent storage for explicit user trust decisions on native `.dll` mods (`native_trust.toon`).
//!
//! Enforces `ADR 0001` and `docs/architecture/native-module-boundary.md`: native `.dll` modules
//! execute unrestricted machine code outside the script sandbox and must never be loaded automatically
//! without explicit user approval bound to both `mod_id` and exact DLL `SHA-256`.

use std::fs;
use std::io::{self, Write as _};
use std::path::Path;

use serde::{Deserialize, Serialize};
use simoder_formats::{load_toon_file_typed, toon_to_string};
use tempfile::NamedTempFile;

use crate::{assets, types::*};

/// Explicit user consent decision recorded for a specific `(mod_id, sha256)` native `.dll`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum NativeTrustDecision {
    /// User explicitly approved executing this exact native `.dll` SHA-256 digest.
    Approved,
    /// User explicitly declined executing this native `.dll` SHA-256 digest.
    Denied,
}

/// One persisted trust decision entry inside [`PersistedNativeTrust`].
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NativeTrustEntry {
    /// Canonical mod identifier (`metadata.id`).
    pub mod_id: String,
    /// Relative DLL filename declared under `[native]`.
    pub dll: String,
    /// Lowercase 64-character SHA-256 hex digest covered by [`Self::decision`].
    pub sha256: String,
    /// Recorded user decision (`approved` or `denied`).
    pub decision: NativeTrustDecision,
}

/// Strict TOON schema persisted to `simoder/native_trust.toon`.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PersistedNativeTrust {
    /// Recorded native DLL trust decisions keyed logically by `mod_id` + `sha256`.
    pub entries: Vec<NativeTrustEntry>,
}

/// Validated in-memory store for native `.dll` trust approvals (`native_trust.toon`).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct NativeTrustStore {
    entries: Vec<NativeTrustEntry>,
}

impl NativeTrustStore {
    /// Create an empty [`NativeTrustStore`].
    pub const fn new() -> Self {
        Self {
            entries: Vec::new(),
        }
    }

    /// Borrow all recorded [`NativeTrustEntry`] records.
    pub fn entries(&self) -> &[NativeTrustEntry] {
        &self.entries
    }

    /// Load and validate native `.dll` trust entries from `path`.
    ///
    /// Returns an empty store if `path` does not exist yet. Rejects symlinks/reparse points,
    /// malformed TOON syntax, invalid mod IDs, or non-SHA-256 digests.
    pub fn load(path: &Path) -> Result<Self, ModError> {
        match fs::symlink_metadata(path) {
            Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(Self::new()),
            Err(err) => return Err(err.into()),
            Ok(_) => {
                assets::reject_reparse_or_symlink(path)?;
            }
        }
        let persisted: PersistedNativeTrust = load_toon_file_typed(path)?;
        for entry in &persisted.entries {
            validate_trust_entry(entry)?;
        }
        Ok(Self {
            entries: persisted.entries,
        })
    }

    /// Atomically serialize and flush this store to `path` using [`tempfile::NamedTempFile::persist`].
    pub fn save(&self, path: &Path) -> Result<(), ModError> {
        for entry in &self.entries {
            validate_trust_entry(entry)?;
        }
        let mut encoded = toon_to_string(&PersistedNativeTrust {
            entries: self.entries.clone(),
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

    /// Look up the recorded decision requiring BOTH exact `mod_id` and case-insensitive `sha256` match.
    ///
    /// Any change to the `.dll` bytes invalidates the stored approval and returns `None`, forcing
    /// fresh user confirmation before execution.
    pub fn decision_for(&self, mod_id: &str, sha256: &str) -> Option<NativeTrustDecision> {
        self.entries
            .iter()
            .find(|e| e.mod_id == mod_id && e.sha256.eq_ignore_ascii_case(sha256))
            .map(|e| e.decision)
    }

    /// Record or replace the user trust decision for `mod_id`.
    pub fn set_decision(
        &mut self,
        mod_id: impl Into<String>,
        dll: impl Into<String>,
        sha256: impl Into<String>,
        decision: NativeTrustDecision,
    ) -> Result<(), ModError> {
        let entry = NativeTrustEntry {
            mod_id: mod_id.into(),
            dll: dll.into(),
            sha256: sha256.into().to_ascii_lowercase(),
            decision,
        };
        validate_trust_entry(&entry)?;
        self.entries.retain(|e| e.mod_id != entry.mod_id);
        self.entries.push(entry);
        Ok(())
    }
}

fn validate_trust_entry(entry: &NativeTrustEntry) -> Result<(), ModError> {
    if !is_valid_mod_id(&entry.mod_id) {
        return Err(ModError::InvalidModId(entry.mod_id.clone()));
    }
    if entry.dll.is_empty() {
        return Err(ModError::ManifestValidation(
            "native_trust entry 'dll' must not be empty".into(),
        ));
    }
    if !assets::is_lowercase_sha256_hex(&entry.sha256) {
        return Err(ModError::ManifestValidation(format!(
            "native_trust entry '{}' requires a 64-character lowercase SHA-256 digest",
            entry.mod_id
        )));
    }
    Ok(())
}
