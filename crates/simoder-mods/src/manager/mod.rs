//! Transactional mod lifecycle manager handling directory discovery, Schema 3 blocking,
//! change detection, hot-reload revision validation, atomic state persistence, and stop gating.

mod discovery;

use std::collections::{BTreeMap, HashSet};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use parking_lot::{Mutex, MutexGuard};
use simoder_core::Tgi;

use crate::{assets, patch_registry::*, state_store, types::*};

use discovery::scan_mods_directory;

/// Optional callback invoked after registry mutations with the affected target TGIs.
pub type RuntimeRefreshCallback = Box<dyn Fn(&[Tgi]) -> Result<(), String> + Send + Sync>;
type SharedRefreshCallback = Arc<dyn Fn(&[Tgi]) -> Result<(), String> + Send + Sync>;

#[derive(Debug, Clone)]
struct PendingCandidate {
    definition: ModDefinition,
    fingerprint: ModFingerprint,
}

#[derive(Debug, Clone)]
enum ModLifecycle {
    Inactive,
    Loading,
    Active {
        diagnostic: Option<String>,
    },
    ChangedPending {
        pending: Box<PendingCandidate>,
        diagnostic: String,
    },
    ChangedInvalid {
        diagnostic: String,
    },
    Unloading,
    Failed {
        diagnostic: String,
    },
    Missing {
        diagnostic: String,
    },
}

impl ModLifecycle {
    const fn state(&self) -> ModState {
        match self {
            Self::Inactive => ModState::Inactive,
            Self::Loading => ModState::Loading,
            Self::Active { .. } => ModState::Active,
            Self::ChangedPending { .. } | Self::ChangedInvalid { .. } => ModState::Changed,
            Self::Unloading => ModState::Unloading,
            Self::Failed { .. } => ModState::Failed,
            Self::Missing { .. } => ModState::Missing,
        }
    }

    fn diagnostic(&self) -> Option<&str> {
        match self {
            Self::Active { diagnostic } => diagnostic.as_deref(),
            Self::ChangedPending { diagnostic, .. }
            | Self::ChangedInvalid { diagnostic }
            | Self::Failed { diagnostic }
            | Self::Missing { diagnostic } => Some(diagnostic.as_str()),
            Self::Inactive | Self::Loading | Self::Unloading => None,
        }
    }

    fn pending(&self) -> Option<&PendingCandidate> {
        match self {
            Self::ChangedPending { pending, .. } => Some(pending.as_ref()),
            _ => None,
        }
    }

    fn set_diagnostic(&mut self, msg: String) {
        match self {
            Self::Active { diagnostic } => *diagnostic = Some(msg),
            Self::ChangedPending { diagnostic, .. }
            | Self::ChangedInvalid { diagnostic }
            | Self::Failed { diagnostic }
            | Self::Missing { diagnostic } => *diagnostic = msg,
            Self::Inactive | Self::Loading | Self::Unloading => {
                *self = Self::Failed { diagnostic: msg };
            }
        }
    }

    fn append_diagnostic(&mut self, msg: String) {
        match self {
            Self::Active {
                diagnostic: Some(existing),
            } if !existing.is_empty() => {
                existing.push_str("; ");
                existing.push_str(&msg);
            }
            Self::ChangedPending {
                diagnostic: existing,
                ..
            }
            | Self::ChangedInvalid {
                diagnostic: existing,
            }
            | Self::Failed {
                diagnostic: existing,
            }
            | Self::Missing {
                diagnostic: existing,
            } if !existing.is_empty() => {
                existing.push_str("; ");
                existing.push_str(&msg);
            }
            _ => self.set_diagnostic(msg),
        }
    }
}

#[derive(Debug, Clone)]
struct ModRecord {
    definition: ModDefinition,
    fingerprint: ModFingerprint,
    lifecycle: ModLifecycle,
}

/// In-memory verified binary payload for an active Schema 3 `simoder.raw-resources` entry.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ActiveRawResource {
    /// Owning mod ID.
    pub owner: String,
    /// Canonical uppercase `TYPE:GROUP:16-HEX-INSTANCE` resource ID.
    pub id: String,
    /// Resource mutation operation (`add` or `replace`).
    pub operation: RawResourceOperation,
    /// Verified lowercase 64-character SHA-256 hex digest.
    pub sha256: String,
    /// Verified raw resource bytes held in memory (never written to `.package` on disk).
    pub bytes: Vec<u8>,
}

#[derive(Debug, Default)]
struct InnerState {
    initialized: bool,
    records: BTreeMap<String, ModRecord>,
    failed_discoveries: Vec<ModUiEntry>,
    enabled_order: Vec<String>,
    active_raw_resources: BTreeMap<String, Vec<ActiveRawResource>>,
}

/// Manages mod discovery under `mods/`, active persistence in `state.toon`, change staging,
/// hot-reload revision checks, and synchronized publication into [`PatchRegistry`].
pub struct ModManager {
    mods_directory: PathBuf,
    state_path: PathBuf,
    registry: Arc<PatchRegistry>,
    refresh_callback: Option<SharedRefreshCallback>,
    known_vanilla_digests: HashSet<String>,
    stopping: AtomicBool,
    state: Mutex<InnerState>,
}

impl ModManager {
    /// Construct a new uninitialized [`ModManager`] wired to `mods_directory`, `state_path`, and `registry`.
    pub fn new(
        mods_directory: impl Into<PathBuf>,
        state_path: impl Into<PathBuf>,
        registry: Arc<PatchRegistry>,
    ) -> Self {
        Self {
            mods_directory: mods_directory.into(),
            state_path: state_path.into(),
            registry,
            refresh_callback: None,
            known_vanilla_digests: HashSet::new(),
            stopping: AtomicBool::new(false),
            state: Mutex::new(InnerState::default()),
        }
    }

    /// Attach a [`RuntimeRefreshCallback`] notified with sorted affected TGIs after enable/disable/reload.
    pub fn with_refresh_callback(mut self, callback: RuntimeRefreshCallback) -> Self {
        self.refresh_callback = Some(Arc::from(callback));
        self
    }

    /// Register SHA-256 digests of known vanilla files rejected if bundled under `[[assets.new]]`.
    pub fn with_known_vanilla_digests(
        mut self,
        digests: impl IntoIterator<Item = String>,
    ) -> Self {
        let raw: HashSet<String> = digests.into_iter().collect();
        self.known_vanilla_digests = assets::normalized_digest_set(&raw);
        self
    }

    /// Latch the sticky runtime stop gate, blocking subsequent lifecycle mutations cleanly.
    pub fn request_stop(&self) {
        self.stopping.store(true, Ordering::SeqCst);
    }

    /// Return `true` after [`Self::request_stop`] has been called.
    pub fn is_stopping(&self) -> bool {
        self.stopping.load(Ordering::SeqCst)
    }

    fn lock_state(&self) -> MutexGuard<'_, InnerState> {
        self.state.lock()
    }

    fn lock_ready_state(
        &self,
        uninitialized_msg: &str,
    ) -> Result<MutexGuard<'_, InnerState>, ModError> {
        let inner = self.lock_state();
        if self.is_stopping() {
            return Err(ModError::Lifecycle("Mod runtime is stopping".into()));
        }
        if !inner.initialized {
            return Err(ModError::Lifecycle(uninitialized_msg.into()));
        }
        Ok(inner)
    }

    /// Scan `mods_directory`, restore previously enabled mods from `state.toon`, and commit initial state.
    pub fn initialize(&self) -> Result<(), ModError> {
        let mut inner = self.lock_state();
        if self.is_stopping() {
            return Err(ModError::Lifecycle("Mod runtime is stopping".into()));
        }
        if inner.initialized {
            return Err(ModError::Lifecycle(
                "Mod manager is already initialized".into(),
            ));
        }

        let mut errors = Vec::new();
        let requested = match state_store::load_enabled_state(&self.state_path) {
            Ok(list) => Some(list),
            Err(err) => {
                errors.push(err.to_string());
                None
            }
        };

        inner.enabled_order.clear();
        if let Err(err) = self.refresh_locked(&mut inner) {
            errors.push(err.to_string());
        }

        let mut pending_notifications = Vec::new();
        if let Some(requested_ids) = requested {
            for id in requested_ids {
                let inactive = inner
                    .records
                    .get(&id)
                    .is_some_and(|rec| rec.lifecycle.state() == ModState::Inactive);
                if !inactive {
                    errors.push(format!("Persisted mod '{id}' is unavailable"));
                    continue;
                }
                match self.enable_locked(&mut inner, &id, false) {
                    Ok(affected) => pending_notifications.push((id, affected)),
                    Err(err) => {
                        if let Some(rec) = inner.records.get_mut(&id) {
                            rec.lifecycle = ModLifecycle::Failed {
                                diagnostic: err.to_string(),
                            };
                        }
                        errors.push(format!("Mod '{id}': {err}"));
                    }
                }
            }
        }

        if let Err(err) = state_store::save_enabled_state(&self.state_path, &inner.enabled_order) {
            errors.push(err.to_string());
        }
        inner.initialized = true;
        drop(inner);

        for (id, affected) in pending_notifications {
            self.notify_runtime_refresh(&id, &affected, "enable");
        }

        if errors.is_empty() {
            Ok(())
        } else {
            Err(ModError::Lifecycle(errors.join("; ")))
        }
    }

    /// Re-scan immediate subdirectories of `mods_directory` and update inactive/changed/missing records.
    pub fn refresh(&self) -> Result<(), ModError> {
        let mut inner = self.lock_ready_state("Mod manager must be initialized before refresh")?;
        self.refresh_locked(&mut inner)
    }

    /// Enable (`true`) or disable (`false`) mod `id`, persisting `state.toon` before registry publication.
    pub fn set_enabled(&self, id: &str, enabled: bool) -> Result<(), ModError> {
        let (affected, action) = {
            let mut inner = self.lock_ready_state("Mod manager is not initialized")?;
            if enabled {
                self.refresh_locked(&mut inner).map_err(|err| {
                    ModError::Lifecycle(format!("Could not refresh mod files before enable: {err}"))
                })?;
                (self.enable_locked(&mut inner, id, true)?, "enable")
            } else {
                (self.disable_locked(&mut inner, id, true, true)?, "disable")
            }
        };
        self.notify_runtime_refresh(id, &affected, action);
        Ok(())
    }

    /// Hot-reload pending changes on an already active `id`, optionally checking `expected_revision`.
    ///
    /// Preserves the mod's original activation sequence relative to other enabled mods.
    pub fn reload(&self, id: &str, expected_revision: Option<&str>) -> Result<(), ModError> {
        let affected = {
            let mut inner = self.lock_ready_state("Mod manager is not initialized")?;
            self.refresh_locked(&mut inner)?;

            let pending = {
                let record = inner.records.get(id).ok_or_else(|| {
                    ModError::Lifecycle("No valid pending definition for an active mod".into())
                })?;
                if !self.registry.is_active(id) {
                    return Err(ModError::Lifecycle(
                        "No valid pending definition for an active mod".into(),
                    ));
                }
                let ModLifecycle::ChangedPending { pending, .. } = &record.lifecycle else {
                    return Err(ModError::Lifecycle(
                        "No valid pending definition for an active mod".into(),
                    ));
                };
                if let Some(expected) = expected_revision
                    && !expected.is_empty()
                    && expected != pending.fingerprint.to_hex()
                {
                    return Err(ModError::Lifecycle(
                        "Reload preview is stale; review the latest pending definition".into(),
                    ));
                }
                pending.as_ref().clone()
            };

            let affected = match self.registry.replace(id, pending.definition.patches()) {
                Ok(affected) => affected,
                Err(err) => {
                    if let Some(rec) = inner.records.get_mut(id) {
                        rec.lifecycle.set_diagnostic(format!("Reload rejected: {err}"));
                    }
                    return Err(err);
                }
            };

            let record = inner.records.get_mut(id).ok_or_else(|| {
                ModError::Lifecycle(format!("Mod record '{id}' missing after reload"))
            })?;
            record.definition = pending.definition;
            record.fingerprint = pending.fingerprint;
            record.lifecycle = ModLifecycle::Active { diagnostic: None };
            affected
        };
        self.notify_runtime_refresh(id, &affected, "reload");
        Ok(())
    }

    /// Return a point-in-time snapshot of all discovered valid, active, missing, and failed mods.
    pub fn snapshot(&self) -> Vec<ModUiEntry> {
        let inner = self.lock_state();
        let mut out = Vec::with_capacity(inner.records.len() + inner.failed_discoveries.len());
        for (id, record) in &inner.records {
            let registered = self.registry.is_active(id);
            let state = record.lifecycle.state();
            let pending = record.lifecycle.pending();
            out.push(ModUiEntry {
                id: id.clone(),
                name: record.definition.manifest.name.clone(),
                version: record.definition.manifest.version.clone(),
                author: record.definition.manifest.author.clone(),
                state,
                can_enable: state == ModState::Inactive && !registered,
                can_disable: registered,
                has_pending_changes: state == ModState::Changed,
                can_reload: registered && pending.is_some(),
                pending_revision: pending.map(|p| p.fingerprint.to_hex()),
                diagnostic: record.lifecycle.diagnostic().map(str::to_string),
                active_patches: if registered {
                    record.definition.patches().to_vec()
                } else {
                    Vec::new()
                },
                pending_patches: pending
                    .map_or_else(Vec::new, |p| p.definition.patches().to_vec()),
                new_asset_count: record.definition.raw_resources().map_or_else(
                    || record.definition.new_assets().len(),
                    |r| r.resources.len(),
                ),
                vanilla_reference_count: record.definition.vanilla_references().len(),
            });
        }
        out.extend(inner.failed_discoveries.iter().cloned());
        out
    }

    /// Return active mod IDs in deterministic activation order.
    pub fn enabled_order(&self) -> Vec<String> {
        self.lock_state().enabled_order.clone()
    }

    /// Return property-level override conflicts currently active in [`PatchRegistry`].
    pub fn conflicts(&self) -> Vec<PatchConflict> {
        self.registry.current().conflicts().to_vec()
    }

    /// Return the current committed [`PatchRegistry`] generation.
    pub fn generation(&self) -> u64 {
        self.registry.generation()
    }

    /// Borrow the underlying shared [`PatchRegistry`].
    pub const fn registry(&self) -> &Arc<PatchRegistry> {
        &self.registry
    }

    /// Return all active in-memory Schema 3 raw resources ordered by `enabled_order`.
    pub fn active_raw_resources(&self) -> Vec<ActiveRawResource> {
        let inner = self.lock_state();
        let mut out = Vec::new();
        for owner in &inner.enabled_order {
            if let Some(entries) = inner.active_raw_resources.get(owner) {
                out.extend(entries.iter().cloned());
            }
        }
        out
    }

    /// Resolve the highest-priority (last-enabled) active in-memory raw resource matching `canonical_id`.
    pub fn resolve_raw_resource(&self, canonical_id: &str) -> Option<ActiveRawResource> {
        let target = canonical_id.to_ascii_uppercase();
        let inner = self.lock_state();
        for owner in inner.enabled_order.iter().rev() {
            if let Some(entries) = inner.active_raw_resources.get(owner)
                && let Some(found) = entries.iter().find(|entry| entry.id == target)
            {
                return Some(found.clone());
            }
        }
        None
    }

    /// Resolve effective in-memory Schema 3 raw resources after applying per-region `region_overrides`.
    ///
    /// Mods with `region_overrides.get(id) == Some(&false)` are excluded even if globally enabled;
    /// discovered mods with `region_overrides.get(id) == Some(&true)` are included even if globally disabled
    /// without mutating global `state.toon`.
    pub fn effective_raw_resources(
        &self,
        region_overrides: &BTreeMap<String, bool>,
    ) -> Vec<ActiveRawResource> {
        let inner = self.lock_state();
        let mut effective_ids: Vec<String> = inner
            .enabled_order
            .iter()
            .filter(|id| region_overrides.get(id.as_str()).copied() != Some(false))
            .cloned()
            .collect();

        for (id, record) in &inner.records {
            if region_overrides.get(id.as_str()).copied() == Some(true)
                && !effective_ids.iter().any(|existing| existing == id)
                && !matches!(
                    record.lifecycle,
                    ModLifecycle::Failed { .. } | ModLifecycle::Missing { .. }
                )
            {
                effective_ids.push(id.clone());
            }
        }

        let mut out = Vec::new();
        for owner in &effective_ids {
            if let Some(entries) = inner.active_raw_resources.get(owner) {
                out.extend(entries.iter().cloned());
            } else if let Some(record) = inner.records.get(owner)
                && let Ok(entries) =
                    Self::load_raw_resources_into_memory(&record.definition, &self.known_vanilla_digests)
            {
                out.extend(entries);
            }
        }
        out
    }

    fn refresh_locked(&self, inner: &mut InnerState) -> Result<(), ModError> {
        if self.is_stopping() {
            return Err(ModError::Lifecycle("Mod runtime is stopping".into()));
        }
        let mut scan = scan_mods_directory(&self.mods_directory, &self.known_vanilla_digests)?;

        let mut retained_failed_dirs = HashSet::new();
        let mut persistence_changed = false;
        let mut refresh_errors = Vec::new();
        let mut pending_notifications = Vec::new();

        let existing_ids: Vec<String> = inner.records.keys().cloned().collect();
        for id in existing_ids {
            let registered = self.registry.is_active(&id);
            if let Some(candidate) = scan.unique.remove(&id) {
                if let Some(record) = inner.records.get_mut(&id) {
                    if registered && record.fingerprint != candidate.fingerprint {
                        record.lifecycle = ModLifecycle::ChangedPending {
                            pending: Box::new(PendingCandidate {
                                definition: candidate.definition,
                                fingerprint: candidate.fingerprint,
                            }),
                            diagnostic:
                                "Files changed while active; preview and reload, or switch OFF/ON"
                                    .into(),
                        };
                    } else if registered {
                        let diagnostic = match &record.lifecycle {
                            ModLifecycle::Active { diagnostic } => diagnostic.clone(),
                            _ => None,
                        };
                        record.lifecycle = ModLifecycle::Active { diagnostic };
                    } else {
                        record.definition = candidate.definition;
                        record.fingerprint = candidate.fingerprint;
                        record.lifecycle = ModLifecycle::Inactive;
                    }
                }
                continue;
            }

            if !registered {
                inner.records.remove(&id);
                continue;
            }

            let Some(record_dir) = inner.records.get(&id).map(|rec| rec.definition.directory.clone())
            else {
                continue;
            };
            if let Some((fail_msg, _)) = scan.failed_by_path.get(&record_dir) {
                if let Some(record) = inner.records.get_mut(&id) {
                    record.lifecycle = ModLifecycle::ChangedInvalid {
                        diagnostic: format!(
                            "Changed files are invalid; active definition retained: {fail_msg}"
                        ),
                    };
                }
                retained_failed_dirs.insert(record_dir);
                continue;
            }
            if scan.duplicate_ids.contains(&id) {
                if let Some(record) = inner.records.get_mut(&id) {
                    record.lifecycle = ModLifecycle::ChangedInvalid {
                        diagnostic:
                            "Duplicate mod ID detected; active definition retained until OFF".into(),
                    };
                }
                continue;
            }

            match self.disable_locked(inner, &id, false, false) {
                Ok(affected) => {
                    persistence_changed = true;
                    if let Some(record) = inner.records.get_mut(&id) {
                        record.lifecycle = ModLifecycle::Missing {
                            diagnostic: "Active mod directory is missing or no longer valid".into(),
                        };
                    }
                    pending_notifications.push((id, affected));
                }
                Err(err) => {
                    let msg = format!("Failed to unload missing mod '{id}': {err}");
                    if let Some(record) = inner.records.get_mut(&id) {
                        record.lifecycle = ModLifecycle::Failed {
                            diagnostic: msg.clone(),
                        };
                    }
                    refresh_errors.push(msg);
                }
            }
        }

        for (id, candidate) in scan.unique {
            inner.records.insert(
                id,
                ModRecord {
                    definition: candidate.definition,
                    fingerprint: candidate.fingerprint,
                    lifecycle: ModLifecycle::Inactive,
                },
            );
        }

        if persistence_changed
            && let Err(err) = state_store::save_enabled_state(&self.state_path, &inner.enabled_order)
        {
            refresh_errors.push(err.to_string());
        }

        let mut sorted_failed_paths: Vec<PathBuf> = scan
            .failed_by_path
            .keys()
            .filter(|path| !retained_failed_dirs.contains(*path))
            .cloned()
            .collect();
        sorted_failed_paths.sort();

        let mut failed_list: Vec<ModUiEntry> = sorted_failed_paths
            .into_iter()
            .filter_map(|path| scan.failed_by_path.remove(&path).map(|(_, entry)| entry))
            .collect();
        failed_list.extend(scan.duplicate_ui_entries);
        inner.failed_discoveries = failed_list;

        for (id, affected) in pending_notifications {
            self.notify_runtime_refresh_inline(inner, &id, &affected, "disable");
        }

        if refresh_errors.is_empty() {
            Ok(())
        } else {
            Err(ModError::Lifecycle(refresh_errors.join("; ")))
        }
    }

    fn load_raw_resources_into_memory(
        definition: &ModDefinition,
        known_vanilla_digests: &HashSet<String>,
    ) -> Result<Vec<ActiveRawResource>, ModError> {
        let Some(raw) = definition.raw_resources() else {
            return Ok(Vec::new());
        };
        let normalized_vanilla = assets::normalized_digest_set(known_vanilla_digests);
        let mut loaded = Vec::with_capacity(raw.resources.len());
        for res in &raw.resources {
            assets::reject_reparse_or_symlink(&res.resolved_path)?;
            let bytes = std::fs::read(&res.resolved_path)?;
            let digest = simoder_core::hex::encode(assets::sha256_bytes(&bytes));
            if digest != res.sha256 {
                return Err(ModError::AssetValidation(format!(
                    "raw resource '{}' modified on disk prior to activation",
                    res.id
                )));
            }
            if !normalized_vanilla.is_empty() && normalized_vanilla.contains(&digest) {
                return Err(ModError::VanillaCopyViolation(format!(
                    "refusing to activate vanilla asset digest {digest} from '{}'",
                    res.relative_path
                )));
            }
            loaded.push(ActiveRawResource {
                owner: definition.manifest.id.clone(),
                id: res.id.clone(),
                operation: res.operation,
                sha256: res.sha256.clone(),
                bytes,
            });
        }
        Ok(loaded)
    }

    fn enable_locked(
        &self,
        inner: &mut InnerState,
        id: &str,
        persist: bool,
    ) -> Result<Vec<Tgi>, ModError> {
        if self.is_stopping() {
            return Err(ModError::Lifecycle("Mod runtime is stopping".into()));
        }
        let (patches, raw_def) = {
            let record = inner
                .records
                .get_mut(id)
                .ok_or_else(|| ModError::Lifecycle(format!("Unknown mod ID '{id}'")))?;
            if record.lifecycle.state() != ModState::Inactive {
                return Err(ModError::Lifecycle(
                    "Only an inactive mod can be enabled".into(),
                ));
            }
            record.lifecycle = ModLifecycle::Loading;
            let raw_def = if record.definition.raw_resources().is_some() {
                Some(record.definition.clone())
            } else {
                None
            };
            (record.definition.patches().to_vec(), raw_def)
        };

        let raw_entries = if let Some(def) = raw_def.as_ref() {
            match Self::load_raw_resources_into_memory(def, &self.known_vanilla_digests) {
                Ok(entries) => entries,
                Err(err) => {
                    let msg = err.to_string();
                    if let Some(rec) = inner.records.get_mut(id) {
                        rec.lifecycle = ModLifecycle::Failed {
                            diagnostic: msg.clone(),
                        };
                    }
                    return Err(err);
                }
            }
        } else {
            Vec::new()
        };

        let previous_order = inner.enabled_order.clone();
        let mut candidate_order = previous_order.clone();
        candidate_order.push(id.to_string());

        if persist && let Err(err) = state_store::save_enabled_state(&self.state_path, &candidate_order) {
            if let Some(rec) = inner.records.get_mut(id) {
                rec.lifecycle = ModLifecycle::Inactive;
            }
            return Err(err);
        }

        let affected = match self.registry.activate(id, &patches) {
            Ok(affected) => affected,
            Err(err) => {
                let mut msg = err.to_string();
                if persist
                    && let Err(rb_err) =
                        state_store::save_enabled_state(&self.state_path, &previous_order)
                {
                    msg = format!("{msg}; persistence rollback failed: {rb_err}");
                }
                if let Some(rec) = inner.records.get_mut(id) {
                    rec.lifecycle = ModLifecycle::Failed {
                        diagnostic: msg.clone(),
                    };
                }
                return Err(ModError::Lifecycle(msg));
            }
        };

        if !raw_entries.is_empty() {
            inner.active_raw_resources.insert(id.to_string(), raw_entries);
        }
        inner.enabled_order = candidate_order;
        if let Some(record) = inner.records.get_mut(id) {
            record.lifecycle = ModLifecycle::Active { diagnostic: None };
        }
        Ok(affected)
    }

    fn disable_locked(
        &self,
        inner: &mut InnerState,
        id: &str,
        persist: bool,
        finalize_pending: bool,
    ) -> Result<Vec<Tgi>, ModError> {
        let Some(record) = inner.records.get_mut(id) else {
            return Err(ModError::Lifecycle(format!("Unknown mod ID '{id}'")));
        };
        if !self.registry.is_active(id) {
            return Err(ModError::Lifecycle(
                "Only an active or changed mod can be disabled".into(),
            ));
        }

        let previous_lifecycle = std::mem::replace(&mut record.lifecycle, ModLifecycle::Unloading);
        let previous_order = inner.enabled_order.clone();
        let mut candidate_order = previous_order.clone();
        candidate_order.retain(|entry| entry != id);

        if persist && let Err(err) = state_store::save_enabled_state(&self.state_path, &candidate_order) {
            if let Some(rec) = inner.records.get_mut(id) {
                rec.lifecycle = previous_lifecycle;
                rec.lifecycle.set_diagnostic(err.to_string());
            }
            return Err(err);
        }

        let affected = match self.registry.deactivate(id) {
            Ok(affected) => affected,
            Err(err) => {
                let mut msg = err.to_string();
                if persist
                    && let Err(rb_err) =
                        state_store::save_enabled_state(&self.state_path, &previous_order)
                {
                    msg = format!("{msg}; persistence rollback failed: {rb_err}");
                }
                if let Some(rec) = inner.records.get_mut(id) {
                    rec.lifecycle = previous_lifecycle;
                    rec.lifecycle.append_diagnostic(msg.clone());
                }
                return Err(ModError::Lifecycle(msg));
            }
        };

        inner.active_raw_resources.remove(id);

        inner.enabled_order = candidate_order;
        if let Some(record) = inner.records.get_mut(id) {
            match previous_lifecycle {
                ModLifecycle::ChangedPending { pending, .. } if finalize_pending => {
                    record.definition = pending.definition;
                    record.fingerprint = pending.fingerprint;
                    record.lifecycle = ModLifecycle::Inactive;
                }
                ModLifecycle::ChangedInvalid { diagnostic } => {
                    record.lifecycle = ModLifecycle::Failed {
                        diagnostic: if diagnostic.is_empty() {
                            "Changed files must validate before this mod can be enabled again"
                                .into()
                        } else {
                            diagnostic
                        },
                    };
                }
                _ => record.lifecycle = ModLifecycle::Inactive,
            }
        }
        Ok(affected)
    }

    fn notify_runtime_refresh(&self, id: &str, affected: &[Tgi], action: &str) {
        let Some(callback) = self.refresh_callback.as_ref().map(Arc::clone) else {
            return;
        };
        if affected.is_empty() {
            return;
        }
        if let Err(err_msg) = callback(affected) {
            let reason = if err_msg.is_empty() {
                "unspecified runtime refresh error".to_string()
            } else {
                err_msg
            };
            let mut inner = self.lock_state();
            if let Some(record) = inner.records.get_mut(id) {
                record.lifecycle.append_diagnostic(format!(
                    "Registry state committed, but live {action} failed: {reason}"
                ));
            }
        }
    }

    fn notify_runtime_refresh_inline(
        &self,
        inner: &mut InnerState,
        id: &str,
        affected: &[Tgi],
        action: &str,
    ) {
        let Some(callback) = &self.refresh_callback else {
            return;
        };
        if affected.is_empty() {
            return;
        }
        if let Err(err_msg) = callback(affected) {
            let reason = if err_msg.is_empty() {
                "unspecified runtime refresh error".to_string()
            } else {
                err_msg
            };
            if let Some(record) = inner.records.get_mut(id) {
                record.lifecycle.append_diagnostic(format!(
                    "Registry state committed, but live {action} failed: {reason}"
                ));
            }
        }
    }
}
