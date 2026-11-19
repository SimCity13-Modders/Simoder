#![forbid(unsafe_code)]

//! Mod registry and deterministic TGI patch conflict resolution.

use std::collections::HashMap;

use simoder_core::{Tgi, tgi_key};

/// Single resource patch declaration owned by one enabled mod.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PatchEntry {
    /// Identifier of the mod claiming this patch.
    pub owner: String,
    /// Target DBPF resource [`Tgi`] modified by this entry.
    pub target: Tgi,
    /// SHA-256 digest of the replacement payload.
    pub payload_sha256: [u8; 32],
}

/// Explicit visibility record for [`Tgi`] collisions resolved by deterministic load order.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConflictRecord {
    /// Contended target DBPF resource identifier.
    pub target: Tgi,
    /// Owner mod whose patch currently wins for `target`.
    pub winner: String,
    /// Earlier mod IDs overridden by `winner`.
    pub overridden: Vec<String>,
}

/// Ordered registry mapping [`Tgi`] targets to their active winning patch owner.
#[derive(Debug, Default, Clone)]
pub struct PatchRegistry {
    entries_by_target: HashMap<Tgi, Vec<PatchEntry>>,
}

impl PatchRegistry {
    /// Create an empty [`PatchRegistry`].
    pub fn new() -> Self {
        Self::default()
    }

    /// Register a [`PatchEntry`] in active mod load order (later registrations win on conflict).
    pub fn register(&mut self, entry: PatchEntry) {
        self.entries_by_target
            .entry(entry.target)
            .or_default()
            .push(entry);
    }

    /// Remove all patch entries registered by a disabled or unloaded mod (`owner`).
    pub fn remove_owner(&mut self, owner: &str) {
        self.entries_by_target.retain(|_, entries| {
            entries.retain(|e| e.owner != owner);
            !entries.is_empty()
        });
    }

    /// Retrieve the currently winning [`PatchEntry`] for a given [`Tgi`] target.
    pub fn active_for_target(&self, target: Tgi) -> Option<&PatchEntry> {
        self.entries_by_target.get(&target)?.last()
    }

    /// Enumerate all [`Tgi`] targets contended by more than one mod, sorted canonically via [`tgi_key`].
    pub fn conflicts(&self) -> Vec<ConflictRecord> {
        let mut records: Vec<ConflictRecord> = self
            .entries_by_target
            .iter()
            .filter_map(|(&target, entries)| {
                if entries.len() < 2 {
                    return None;
                }
                let winner = entries.last()?.owner.clone();
                let overridden = entries[..entries.len() - 1]
                    .iter()
                    .map(|e| e.owner.clone())
                    .collect();
                Some(ConflictRecord {
                    target,
                    winner,
                    overridden,
                })
            })
            .collect();
        records.sort_by_key(|record| tgi_key(record.target));
        records
    }
}
