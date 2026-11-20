//! Thread-safe generationed patch registry, deterministic TGI conflict reporting,
//! and verified runtime PROP float sequence application.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::sync::{Arc, RwLock, RwLockReadGuard, RwLockWriteGuard};

use simoder_core::{
    RuntimePropertyRecord, RuntimeTableStatus, SCALAR_FLOAT_METADATA, Tgi, tgi_key,
    validate_runtime_property_table,
};

use crate::types::*;

/// One active resource patch entry bound to its owning mod and activation order sequence.
#[derive(Debug, Clone, PartialEq)]
pub struct RegisteredPatch {
    /// Owning mod identifier.
    pub owner: String,
    /// Target resource TGI and property mutations contributed by `owner`.
    pub patch: ResourcePatchDefinition,
    /// Monotonic per-owner activation order counter (`0` = enabled earliest).
    pub sequence: u64,
}

/// Property-level override conflict observed when multiple distinct mods patch the same `(Tgi, property_id)`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PatchConflict {
    /// Contended DBPF resource TGI.
    pub target: Tgi,
    /// Contended property hash inside `target`.
    pub property_id: u32,
    /// Earlier owner whose mutation is executed before or overwritten by `winning_owner`.
    pub previous_owner: String,
    /// Later owner executing after `previous_owner`.
    pub winning_owner: String,
}

/// Immutable point-in-time snapshot of all registered resource patches and detected conflicts.
#[derive(Debug, Clone)]
pub struct PatchRegistrySnapshot {
    generation: u64,
    patches_by_target: HashMap<Tgi, Vec<RegisteredPatch>>,
    conflicts: Vec<PatchConflict>,
}

impl PatchRegistrySnapshot {
    /// Return the monotonically increasing registry mutation generation for this snapshot.
    pub const fn generation(&self) -> u64 {
        self.generation
    }

    /// Look up ordered [`RegisteredPatch`] entries targeting `target`, sorted by `(sequence, owner)`.
    pub fn find(&self, target: Tgi) -> Option<&[RegisteredPatch]> {
        self.patches_by_target.get(&target).map(Vec::as_slice)
    }

    /// Return all detected property-level conflicts sorted deterministically by `(tgi_key(target), property_id)`.
    pub fn conflicts(&self) -> &[PatchConflict] {
        &self.conflicts
    }

    /// Return all active patched resource TGIs sorted canonically via [`tgi_key`].
    pub fn active_targets(&self) -> Vec<Tgi> {
        let mut targets: Vec<Tgi> = self.patches_by_target.keys().copied().collect();
        targets.sort_by_key(|&t| tgi_key(t));
        targets
    }
}

#[derive(Debug, Clone)]
struct OwnedPatchSet {
    sequence: u64,
    patches: Vec<ResourcePatchDefinition>,
}

#[derive(Debug)]
struct RegistryInner {
    active: HashMap<String, OwnedPatchSet>,
    next_sequence: u64,
    generation: u64,
    snapshot: Arc<PatchRegistrySnapshot>,
}

/// Thread-safe ordered registry tracking active declarative patches per mod owner and publishing
/// immutable [`PatchRegistrySnapshot`] views across mutations.
#[derive(Debug)]
pub struct PatchRegistry {
    inner: RwLock<RegistryInner>,
}

impl Default for PatchRegistry {
    fn default() -> Self {
        Self::new()
    }
}

impl PatchRegistry {
    /// Create an empty [`PatchRegistry`] at generation `0`.
    pub fn new() -> Self {
        let initial = Arc::new(PatchRegistrySnapshot {
            generation: 0,
            patches_by_target: HashMap::new(),
            conflicts: Vec::new(),
        });
        Self {
            inner: RwLock::new(RegistryInner {
                active: HashMap::new(),
                next_sequence: 0,
                generation: 0,
                snapshot: initial,
            }),
        }
    }

    fn read_inner(&self) -> RwLockReadGuard<'_, RegistryInner> {
        self.inner.read().expect("patch registry lock poisoned")
    }

    fn write_inner(&self) -> RwLockWriteGuard<'_, RegistryInner> {
        self.inner.write().expect("patch registry lock poisoned")
    }

    /// Activate `owner` with `patches` using a fresh monotonic sequence, returning sorted affected TGIs.
    pub fn activate(
        &self,
        owner: &str,
        patches: &[ResourcePatchDefinition],
    ) -> Result<Vec<Tgi>, ModError> {
        if owner.is_empty() {
            return Err(ModError::PatchValidation(
                "patch owner must not be empty".into(),
            ));
        }
        let mut guard = self.write_inner();
        if guard.active.contains_key(owner) {
            return Err(ModError::PatchValidation(format!(
                "patch owner '{owner}' is already active"
            )));
        }
        let sequence = guard.next_sequence;
        let next_gen = guard
            .generation
            .checked_add(1)
            .ok_or_else(|| ModError::PatchValidation("patch generation exhausted".into()))?;
        let next_seq = guard
            .next_sequence
            .checked_add(1)
            .ok_or_else(|| ModError::PatchValidation("patch sequence exhausted".into()))?;

        let affected = collect_sorted_targets([patches]);
        guard.active.insert(
            owner.to_string(),
            OwnedPatchSet {
                sequence,
                patches: patches.to_vec(),
            },
        );
        guard.next_sequence = next_seq;
        guard.generation = next_gen;
        guard.snapshot = Arc::new(build_snapshot(&guard.active, next_gen));
        Ok(affected)
    }

    /// Deactivate `owner`, removing all of its patches and returning sorted affected TGIs.
    pub fn deactivate(&self, owner: &str) -> Result<Vec<Tgi>, ModError> {
        if owner.is_empty() {
            return Err(ModError::PatchValidation(
                "patch owner must not be empty".into(),
            ));
        }
        let mut guard = self.write_inner();
        let Some(removed) = guard.active.remove(owner) else {
            return Err(ModError::PatchValidation(format!(
                "patch owner '{owner}' is not active"
            )));
        };
        let next_gen = guard
            .generation
            .checked_add(1)
            .ok_or_else(|| ModError::PatchValidation("patch generation exhausted".into()))?;
        let affected = collect_sorted_targets([removed.patches.as_slice()]);
        guard.generation = next_gen;
        guard.snapshot = Arc::new(build_snapshot(&guard.active, next_gen));
        Ok(affected)
    }

    /// Replace all patches owned by an active `owner` while preserving its activation sequence.
    ///
    /// Returns the deduplicated sorted union of previous and replacement target TGIs.
    pub fn replace(
        &self,
        owner: &str,
        patches: &[ResourcePatchDefinition],
    ) -> Result<Vec<Tgi>, ModError> {
        if owner.is_empty() {
            return Err(ModError::PatchValidation(
                "patch owner must not be empty".into(),
            ));
        }
        let mut guard = self.write_inner();
        let next_gen = guard
            .generation
            .checked_add(1)
            .ok_or_else(|| ModError::PatchValidation("patch generation exhausted".into()))?;
        let affected = {
            let existing = guard.active.get_mut(owner).ok_or_else(|| {
                ModError::PatchValidation(format!("patch owner '{owner}' is not active"))
            })?;
            let union = collect_sorted_targets([existing.patches.as_slice(), patches]);
            existing.patches = patches.to_vec();
            union
        };
        guard.generation = next_gen;
        guard.snapshot = Arc::new(build_snapshot(&guard.active, next_gen));
        Ok(affected)
    }

    /// Obtain a shared handle to the latest committed [`PatchRegistrySnapshot`].
    pub fn current(&self) -> Arc<PatchRegistrySnapshot> {
        Arc::clone(&self.read_inner().snapshot)
    }

    /// Return `true` when `owner` is currently registered.
    pub fn is_active(&self, owner: &str) -> bool {
        self.read_inner().active.contains_key(owner)
    }

    /// Return the current committed registry generation.
    pub fn generation(&self) -> u64 {
        self.read_inner().generation
    }
}

fn collect_sorted_targets<'a>(slices: impl IntoIterator<Item = &'a [ResourcePatchDefinition]>) -> Vec<Tgi> {
    let mut set = HashSet::new();
    for slice in slices {
        for patch in slice {
            set.insert(patch.target);
        }
    }
    let mut out: Vec<Tgi> = set.into_iter().collect();
    out.sort_by_key(|&t| tgi_key(t));
    out
}

fn build_snapshot(active: &HashMap<String, OwnedPatchSet>, generation: u64) -> PatchRegistrySnapshot {
    let mut grouped: BTreeMap<(u32, u32, u32), (Tgi, Vec<RegisteredPatch>)> = BTreeMap::new();
    for (owner, owned) in active {
        for patch in &owned.patches {
            grouped
                .entry(tgi_key(patch.target))
                .or_insert_with(|| (patch.target, Vec::new()))
                .1
                .push(RegisteredPatch {
                    owner: owner.clone(),
                    patch: patch.clone(),
                    sequence: owned.sequence,
                });
        }
    }

    let mut patches_by_target = HashMap::with_capacity(grouped.len());
    let mut conflicts = Vec::new();
    for (_, (target, mut list)) in grouped {
        list.sort_by(|a, b| a.sequence.cmp(&b.sequence).then_with(|| a.owner.cmp(&b.owner)));
        let mut previous_owners: HashMap<u32, String> = HashMap::new();
        for reg in &list {
            for prop in &reg.patch.properties {
                if let Some(prev) = previous_owners.get(&prop.property_id)
                    && prev != &reg.owner
                {
                    conflicts.push(PatchConflict {
                        target,
                        property_id: prop.property_id,
                        previous_owner: prev.clone(),
                        winning_owner: reg.owner.clone(),
                    });
                }
                previous_owners.insert(prop.property_id, reg.owner.clone());
            }
        }
        patches_by_target.insert(target, list);
    }
    conflicts.sort_by_key(|c| (tgi_key(c.target), c.property_id));

    PatchRegistrySnapshot {
        generation,
        patches_by_target,
        conflicts,
    }
}

/// Evaluate one arithmetic [`PatchOperation`] over a finite IEEE-754 `f32` value.
pub fn apply_float_operation(
    current: f32,
    operation: PatchOperation,
    operand: f32,
) -> Result<f32, ModError> {
    if !current.is_finite() || !operand.is_finite() {
        return Err(ModError::PatchValidation(
            "float patch operands must be finite".into(),
        ));
    }
    let result = match operation {
        PatchOperation::Set => operand,
        PatchOperation::Add => current + operand,
        PatchOperation::Subtract => current - operand,
        PatchOperation::Multiply => current * operand,
        PatchOperation::Divide => {
            if operand == 0.0 {
                return Err(ModError::PatchValidation(
                    "divide patch cannot use zero".into(),
                ));
            }
            current / operand
        }
    };
    if !result.is_finite() {
        return Err(ModError::PatchValidation(
            "float patch produced non-finite result".into(),
        ));
    }
    Ok(result)
}

/// Apply an ordered slice of [`RegisteredPatch`] entries against a validated runtime PROP record vector.
///
/// Requires `vanilla` to pass [`validate_runtime_property_table`], resolves each patched property via
/// binary search, checks [`SCALAR_FLOAT_METADATA`], and guarantees finite output bit patterns.
pub fn apply_patch_sequence(
    target: Tgi,
    vanilla: &[RuntimePropertyRecord],
    patches: &[RegisteredPatch],
) -> Result<Vec<RuntimePropertyRecord>, ModError> {
    if validate_runtime_property_table(vanilla, vanilla.len()) != RuntimeTableStatus::Valid {
        return Err(ModError::PatchValidation(
            "vanilla runtime property table failed count or strict-sort validation".into(),
        ));
    }
    let mut candidate = vanilla.to_vec();
    for registered in patches {
        if registered.owner.is_empty() || registered.patch.target != target {
            return Err(ModError::PatchValidation(
                "registered patch ownership or target does not match".into(),
            ));
        }
        for prop_patch in &registered.patch.properties {
            let idx = candidate
                .binary_search_by_key(&prop_patch.property_id, |entry| entry.id)
                .map_err(|_| {
                    ModError::PatchValidation(format!(
                        "mod '{}' targets missing property 0x{:08X} on {target}",
                        registered.owner, prop_patch.property_id
                    ))
                })?;
            let record = &mut candidate[idx];
            if prop_patch.value_type != PropertyValueType::Float
                || record.metadata != SCALAR_FLOAT_METADATA
            {
                return Err(ModError::PatchValidation(format!(
                    "mod '{}' targets property 0x{:08X} with incompatible runtime metadata 0x{:08X}",
                    registered.owner, prop_patch.property_id, record.metadata
                )));
            }
            let current = f32::from_bits(record.value_bits);
            let updated =
                apply_float_operation(current, prop_patch.operation, prop_patch.value).map_err(
                    |err| {
                        ModError::PatchValidation(format!("mod '{}': {err}", registered.owner))
                    },
                )?;
            record.value_bits = updated.to_bits();
        }
    }
    Ok(candidate)
}
