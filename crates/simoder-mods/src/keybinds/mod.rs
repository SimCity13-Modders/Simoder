#![forbid(unsafe_code)]

//! Context-scoped keybind registry, conflict detection, vanilla reserved shortcuts,
//! and atomic `keybinds.toon` persistence for Simoder.

pub mod chord;

use std::collections::BTreeMap;
use std::fs;
use std::io::Write as _;
use std::path::Path;

pub use chord::{KeyChord, KeybindContext, VANILLA_RESERVED_KEYBINDS, VanillaReservedKeybind};
use chord::MAX_CONTEXT_LEN;
use serde::{Deserialize, Serialize};
use simoder_formats::toon::{load_toon_file_typed, toon_to_string};
use tempfile::NamedTempFile;

use crate::types::is_valid_mod_id;

/// Canonical persistence filename created inside the `simoder/` directory.
pub const KEYBINDS_TOON_FILE_NAME: &str = "keybinds.toon";

const MAX_TITLE_LEN: usize = 128;
const MAX_DESCRIPTION_LEN: usize = 512;

/// Metadata describing one mod-registered keybind action.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct KeybindDescriptor {
    /// Canonical owning mod identifier (e.g. `"simoder.ui_inspector"`).
    pub owner_mod_id: String,
    /// Short action identifier within the mod (e.g. `"toggle_panel"`).
    pub action_id: String,
    /// Human-readable title displayed in the KeyBinds UI.
    pub title: String,
    /// Short description of what the action does.
    pub description: String,
    /// Context in which the keybind is active.
    pub context: KeybindContext,
    /// Optional default key chord (`None` if unbound by default).
    pub default_chord: Option<KeyChord>,
}

impl KeybindDescriptor {
    /// Compute the canonical global keybind ID (`"<owner_mod_id>.<action_id>"`), avoiding double prefixing.
    pub fn canonical_id(&self) -> String {
        let prefix = format!("{}.", self.owner_mod_id);
        if self.action_id.starts_with(&prefix) {
            self.action_id.clone()
        } else {
            format!("{prefix}{}", self.action_id)
        }
    }

    /// Compute the short action ID stripped of any `"<owner_mod_id>."` prefix.
    pub fn short_action_id(&self) -> &str {
        let prefix = format!("{}.", self.owner_mod_id);
        self.action_id.strip_prefix(&prefix).unwrap_or(&self.action_id)
    }
}

/// Classification of a detected keybind collision.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum KeybindConflictKind {
    /// Collision with another registered mod keybind in an overlapping context.
    Mod,
    /// Collision with a built-in retail SimCity 2013 shortcut in an overlapping context.
    Vanilla,
}

/// One detected conflict attached to a [`KeybindSnapshotEntry`].
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeybindConflict {
    /// Whether the collision is against another mod or against vanilla SimCity.
    pub kind: KeybindConflictKind,
    /// Conflicting keybind ID (or `"simcity.vanilla"` for built-in shortcuts).
    pub with_id: String,
    /// Human-readable title of the conflicting binding.
    pub with_title: String,
    /// Context of the conflicting binding.
    pub with_context: String,
}

/// UI-facing snapshot of one registered keybind, its effective chord, and any active conflicts.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct KeybindSnapshotEntry {
    /// Canonical full keybind ID (`"<mod_id>.<action_id>"`).
    pub id: String,
    /// Owning mod ID.
    pub mod_id: String,
    /// Short action ID within the mod.
    pub action_id: String,
    /// Display title.
    pub title: String,
    /// Display description.
    pub description: String,
    /// Context identifier.
    pub context: String,
    /// Default chord string, if any.
    pub default_chord: Option<String>,
    /// Effective bound chord string (`None` when unbound).
    pub bound_chord: Option<String>,
    /// True when a user override differs from the default binding.
    pub is_custom: bool,
    /// True when `conflicts` is non-empty (triggers red UI highlighting).
    pub has_conflict: bool,
    /// Detailed list of mod and vanilla conflicts.
    pub conflicts: Vec<KeybindConflict>,
}

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct KeybindsFile {
    bindings: BTreeMap<String, String>,
}

/// Central registry managing mod keybind descriptors, user overrides, conflict detection, and TOON persistence.
#[derive(Debug, Default)]
pub struct KeybindRegistry {
    descriptors: BTreeMap<String, KeybindDescriptor>,
    overrides: BTreeMap<String, Option<KeyChord>>,
    revision: u64,
}

impl KeybindRegistry {
    /// Create an empty [`KeybindRegistry`].
    pub fn new() -> Self {
        Self::default()
    }

    /// Monotonically incremented revision counter bumped whenever descriptors or bindings change.
    pub fn revision(&self) -> u64 {
        self.revision
    }

    /// Register or replace a mod's keybind descriptor and return its canonical ID.
    pub fn register(&mut self, desc: KeybindDescriptor) -> Result<String, String> {
        if !is_valid_mod_id(&desc.owner_mod_id) {
            return Err(format!("Invalid mod ID '{}'", desc.owner_mod_id));
        }
        let action_trimmed = desc.action_id.trim();
        if action_trimmed.is_empty()
            || action_trimmed.len() > MAX_CONTEXT_LEN
            || !action_trimmed
                .chars()
                .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || matches!(c, '.' | '_' | '-'))
        {
            return Err(format!("Invalid keybind action ID '{}'", desc.action_id));
        }
        if desc.title.trim().is_empty() || desc.title.len() > MAX_TITLE_LEN {
            return Err("Keybind title must be 1..=128 characters".to_string());
        }
        if desc.description.len() > MAX_DESCRIPTION_LEN {
            return Err("Keybind description exceeds 512 characters".to_string());
        }

        let canonical = desc.canonical_id();
        self.descriptors.insert(canonical.clone(), desc);
        self.revision = self.revision.saturating_add(1);
        Ok(canonical)
    }

    /// Remove all keybind descriptors owned by `mod_id` (preserving user overrides on disk).
    pub fn revoke_mod(&mut self, mod_id: &str) -> usize {
        let before = self.descriptors.len();
        self.descriptors.retain(|_, d| d.owner_mod_id != mod_id);
        let removed = before.saturating_sub(self.descriptors.len());
        if removed > 0 {
            self.revision = self.revision.saturating_add(1);
        }
        removed
    }

    /// Rebind `id` to `chord_opt` (`None` or empty string unbinds), returning the normalized chord string.
    pub fn rebind(&mut self, id: &str, chord_opt: Option<&str>) -> Result<Option<String>, String> {
        let Some(desc) = self.descriptors.get(id) else {
            return Err(format!("Unknown keybind '{id}'"));
        };
        let parsed = match chord_opt.map(str::trim).filter(|s| !s.is_empty()) {
            Some(raw) => Some(KeyChord::parse(raw)?),
            None => None,
        };
        let formatted = parsed.as_ref().map(KeyChord::canonical);
        if parsed == desc.default_chord {
            self.overrides.remove(id);
        } else {
            self.overrides.insert(id.to_string(), parsed);
        }
        self.revision = self.revision.saturating_add(1);
        Ok(formatted)
    }

    /// Reset `id` back to its mod-declared default chord.
    pub fn reset(&mut self, id: &str) -> Result<(), String> {
        if !self.descriptors.contains_key(id) {
            return Err(format!("Unknown keybind '{id}'"));
        }
        if self.overrides.remove(id).is_some() {
            self.revision = self.revision.saturating_add(1);
        }
        Ok(())
    }

    /// Clear all user overrides across every keybind.
    pub fn reset_all(&mut self) {
        if !self.overrides.is_empty() {
            self.overrides.clear();
            self.revision = self.revision.saturating_add(1);
        }
    }

    /// Resolve the effective [`KeyChord`] for `id`, taking user overrides into account.
    pub fn effective_chord(&self, id: &str) -> Option<&KeyChord> {
        let desc = self.descriptors.get(id)?;
        match self.overrides.get(id) {
            Some(overridden) => overridden.as_ref(),
            None => desc.default_chord.as_ref(),
        }
    }

    /// Return a map of `short_action_id -> canonical_chord` for all bound actions owned by `mod_id`.
    pub fn effective_map_for_mod(&self, mod_id: &str) -> BTreeMap<String, String> {
        let mut out = BTreeMap::new();
        for (id, desc) in &self.descriptors {
            if desc.owner_mod_id != mod_id {
                continue;
            }
            if let Some(chord) = self.effective_chord(id) {
                out.insert(desc.short_action_id().to_string(), chord.canonical());
            }
        }
        out
    }

    /// Build a complete UI snapshot including context-aware mod and vanilla conflict detection.
    pub fn snapshot(&self) -> Vec<KeybindSnapshotEntry> {
        let resolved: Vec<(&String, &KeybindDescriptor, Option<&KeyChord>)> = self
            .descriptors
            .iter()
            .map(|(id, desc)| (id, desc, self.effective_chord(id)))
            .collect();

        resolved
            .iter()
            .map(|(id, desc, maybe_chord)| {
                let mut conflicts = Vec::new();
                if let Some(chord) = maybe_chord {
                    for (other_id, other_desc, other_chord) in &resolved {
                        if *other_id == *id {
                            continue;
                        }
                        if let Some(oc) = other_chord
                            && *oc == *chord
                            && desc.context.overlaps(&other_desc.context)
                        {
                            conflicts.push(KeybindConflict {
                                kind: KeybindConflictKind::Mod,
                                with_id: (*other_id).clone(),
                                with_title: other_desc.title.clone(),
                                with_context: other_desc.context.as_str().to_string(),
                            });
                        }
                    }

                    for vanilla in VANILLA_RESERVED_KEYBINDS {
                        if let Ok(v_chord) = KeyChord::parse(vanilla.chord)
                            && let Ok(v_ctx) = KeybindContext::parse(vanilla.context)
                            && v_chord == **chord
                            && desc.context.overlaps(&v_ctx)
                        {
                            conflicts.push(KeybindConflict {
                                kind: KeybindConflictKind::Vanilla,
                                with_id: "simcity.vanilla".to_string(),
                                with_title: vanilla.title.to_string(),
                                with_context: vanilla.context.to_string(),
                            });
                        }
                    }
                }

                let is_custom = self.overrides.contains_key(*id);
                KeybindSnapshotEntry {
                    id: (*id).clone(),
                    mod_id: desc.owner_mod_id.clone(),
                    action_id: desc.short_action_id().to_string(),
                    title: desc.title.clone(),
                    description: desc.description.clone(),
                    context: desc.context.as_str().to_string(),
                    default_chord: desc.default_chord.as_ref().map(KeyChord::canonical),
                    bound_chord: maybe_chord.map(KeyChord::canonical),
                    is_custom,
                    has_conflict: !conflicts.is_empty(),
                    conflicts,
                }
            })
            .collect()
    }

    /// Load user overrides from `path` (`keybinds.toon`), ignoring a missing file and failing on invalid chords.
    pub fn load_overrides(&mut self, path: &Path) -> Result<(), String> {
        if !path.try_exists().map_err(|e| format!("Failed checking '{}': {e}", path.display()))? {
            return Ok(());
        }
        let file: KeybindsFile = load_toon_file_typed(path)
            .map_err(|e| format!("Failed loading '{}': {e}", path.display()))?;
        let mut parsed = BTreeMap::new();
        for (id, raw_chord) in file.bindings {
            let trimmed = raw_chord.trim();
            if trimmed.is_empty() {
                parsed.insert(id, None);
            } else {
                let chord = KeyChord::parse(trimmed).map_err(|e| {
                    format!(
                        "Invalid keybind override for '{id}' in '{}': {e}",
                        path.display()
                    )
                })?;
                parsed.insert(id, Some(chord));
            }
        }
        self.overrides = parsed;
        self.revision = self.revision.saturating_add(1);
        Ok(())
    }

    /// Atomically persist current user overrides to `path` (`keybinds.toon`).
    pub fn save_overrides(&self, path: &Path) -> Result<(), String> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)
                .map_err(|e| format!("Failed creating '{}': {e}", parent.display()))?;
        }
        let mut bindings = BTreeMap::new();
        for (id, chord_opt) in &self.overrides {
            bindings.insert(
                id.clone(),
                chord_opt.as_ref().map_or_else(String::new, KeyChord::canonical),
            );
        }
        let mut text = toon_to_string(&KeybindsFile { bindings })
            .map_err(|e| format!("Failed serializing keybinds.toon: {e}"))?;
        text.push('\n');

        let parent = path.parent().unwrap_or_else(|| Path::new("."));
        let mut temp = NamedTempFile::new_in(parent)
            .map_err(|e| format!("Failed creating temp keybinds file: {e}"))?;
        temp.write_all(text.as_bytes())
            .map_err(|e| format!("Failed writing keybinds temp file: {e}"))?;
        temp.as_file()
            .sync_all()
            .map_err(|e| format!("Failed syncing keybinds temp file: {e}"))?;
        temp.persist(path)
            .map_err(|e| format!("Failed persisting '{}': {}", path.display(), e.error))?;
        Ok(())
    }
}
