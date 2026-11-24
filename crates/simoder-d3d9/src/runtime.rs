#![forbid(unsafe_code)]

//! Integrated top-level runtime coordinator uniting `simoder-core`, `simoder-formats`,
//! `simoder-mods`, `simoder-scripting`, and `simoder-crash` inside the single `d3d9.dll` proxy.

use std::collections::{BTreeMap, VecDeque};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use parking_lot::Mutex;
use serde::Serialize;
use simoder_core::{DiagnosticLogPipeline, LogLevel, LogSourceType};
use simoder_crash::{BridgeCorrelationTracker, CrashHandlerCoordinator, SymbolCatalog};
use simoder_mods::{
    HostToolRegistry, ModManager, NativeModuleCallbacks, NativeModuleHost,
    NativeModuleRegistration, NativeModStatus, NativeModUiRecord, PatchRegistry,
    RESERVED_BASE_MOD_ID, SD_READY_MENU, SD_READY_RUNTIME, SdHandle, SdLifecycleState, SdResult,
    UiPatchDescriptor, UiPatchEvent, UiPatchRegistry, UiViewObservation, load_base_mod_definition,
};
use simoder_scripting::{FaultPolicy, ScriptScheduler};

use crate::D3d9Error;
use crate::bootstrap::{BootstrapCoordinator, BootstrapOutcome};

const MAX_PENDING_UI_EVENTS: usize = 256;

/// Verified native `.dll` mod candidate tracked by [`IntegratedSimoderRuntime`] for deferred UI trust approval.
#[derive(Debug, Clone)]
pub struct NativeModCandidateState {
    /// UI-facing metadata and current approval/admission status.
    pub record: NativeModUiRecord,
    /// Absolute source `.dll` path under the mod directory.
    pub dll_path: PathBuf,
    /// Parsed PE32 virtual image size (`SizeOfImage`).
    pub mapped_image_bytes: u32,
    /// On-disk byte length of the source `.dll`.
    pub source_size_bytes: u64,
    /// Live `ABI v2` handle when [`NativeModStatus::Active`].
    pub owner: Option<SdHandle>,
    /// Persistent `native_trust.toon` path governing this candidate.
    pub trust_path: PathBuf,
}

/// UI-safe summary of a discovered script/data mod serialized to `Simoder-Base`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AuthoringModUiRecord {
    /// Canonical mod identifier (empty on early discovery errors).
    pub id: String,
    /// Display name.
    pub name: String,
    /// Mod version string.
    pub version: String,
    /// Mod author string.
    pub author: String,
    /// Kebab/lowercase lifecycle state name.
    pub state: String,
    /// Whether enabling is valid right now.
    pub can_enable: bool,
    /// Whether disabling is valid right now.
    pub can_disable: bool,
    /// Optional diagnostic detail.
    pub diagnostic: Option<String>,
}

/// Combined mod snapshot consumed by `Simoder-Base`'s `mods-ui` service.
#[derive(Debug, Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ModsUiSnapshot {
    /// Discovered native `.dll` mods awaiting approval, active, denied, or failed.
    pub native_mods: Vec<NativeModUiRecord>,
    /// Discovered authoring/property/script mods managed by [`ModManager`].
    pub authoring_mods: Vec<AuthoringModUiRecord>,
    /// True once `mods/simoder.base` passed startup validation.
    pub base_ready: bool,
}

/// Unified runtime state uniting all Simoder subsystems inside the integrated x86 `d3d9.dll`.
pub struct IntegratedSimoderRuntime {
    bootstrap: BootstrapCoordinator,
    logs: Arc<DiagnosticLogPipeline>,
    bridge_tracker: Mutex<BridgeCorrelationTracker>,
    crash_handler: Mutex<Option<CrashHandlerCoordinator>>,
    mod_manager: Mutex<Option<ModManager>>,
    native_host: Mutex<NativeModuleHost>,
    tool_registry: Mutex<HostToolRegistry>,
    script_scheduler: Mutex<ScriptScheduler>,
    base_ready: AtomicBool,
    ui_patches: Mutex<UiPatchRegistry>,
    ui_observation: Mutex<UiViewObservation>,
    pending_ui_events: Mutex<VecDeque<UiPatchEvent>>,
    native_candidates: Mutex<BTreeMap<String, NativeModCandidateState>>,
    region_mod_overrides: Mutex<BTreeMap<String, BTreeMap<String, bool>>>,
    region_mods_path: Mutex<Option<PathBuf>>,
    active_region_id: Mutex<Option<String>>,
}

impl Default for IntegratedSimoderRuntime {
    fn default() -> Self {
        Self::new()
    }
}

impl IntegratedSimoderRuntime {
    /// Construct a fresh, unbooted [`IntegratedSimoderRuntime`] instance.
    pub fn new() -> Self {
        Self {
            bootstrap: BootstrapCoordinator::new(),
            logs: Arc::new(DiagnosticLogPipeline::new()),
            bridge_tracker: Mutex::new(BridgeCorrelationTracker::new()),
            crash_handler: Mutex::new(None),
            mod_manager: Mutex::new(None),
            native_host: Mutex::new(NativeModuleHost::new()),
            tool_registry: Mutex::new(HostToolRegistry::new()),
            script_scheduler: Mutex::new(ScriptScheduler::new(FaultPolicy::Regular)),
            base_ready: AtomicBool::new(false),
            ui_patches: Mutex::new(UiPatchRegistry::new()),
            ui_observation: Mutex::new(UiViewObservation {
                phase: "cold".into(),
                active_views: Vec::new(),
                locale: "en-us".into(),
            }),
            pending_ui_events: Mutex::new(VecDeque::new()),
            native_candidates: Mutex::new(BTreeMap::new()),
            region_mod_overrides: Mutex::new(BTreeMap::new()),
            region_mods_path: Mutex::new(None),
            active_region_id: Mutex::new(None),
        }
    }

    /// Return `true` after `simoder.base` validation succeeded during startup.
    pub fn is_base_ready(&self) -> bool {
        self.base_ready.load(Ordering::Acquire)
    }

    /// Access the compositional [`UiPatchRegistry`] under its mutex.
    pub fn with_ui_patches_mut<R>(&self, f: impl FnOnce(&mut UiPatchRegistry) -> R) -> R {
        let mut guard = self.ui_patches.lock();
        f(&mut guard)
    }

    /// Replace active composed patches committed by `Simoder-Base`'s `ui-manager`.
    pub fn commit_composed_ui_patches(
        &self,
        revision_hint: u64,
        patches: Vec<UiPatchDescriptor>,
    ) -> Result<u64, String> {
        self.ui_patches
            .lock()
            .replace_composed_snapshot(revision_hint, patches)
            .map_err(|e| e.to_string())
    }

    /// Snapshot current UI revision and composed patch list for EAWebKit DOM synchronization.
    pub fn composed_ui_snapshot(&self) -> (u64, Vec<UiPatchDescriptor>) {
        let (rev, patches, _conflicts) = self.ui_patches.lock().all_composed();
        (rev, patches)
    }

    /// Store the latest EAWebKit view observation reported from the game UI thread.
    pub fn set_ui_observation(&self, observation: UiViewObservation) {
        *self.ui_observation.lock() = observation;
    }

    /// Read the latest cached EAWebKit view observation.
    pub fn get_ui_observation(&self) -> UiViewObservation {
        self.ui_observation.lock().clone()
    }

    /// Queue DOM interaction events emitted by the ES5 applicator for `Simoder-Base` polling.
    pub fn push_ui_events(&self, events: impl IntoIterator<Item = UiPatchEvent>) {
        let mut queue = self.pending_ui_events.lock();
        for ev in events {
            if queue.len() >= MAX_PENDING_UI_EVENTS {
                queue.pop_front();
            }
            queue.push_back(ev);
        }
    }

    /// Drain all queued UI interaction events since the last tick.
    pub fn drain_ui_events(&self) -> Vec<UiPatchEvent> {
        let mut queue = self.pending_ui_events.lock();
        queue.drain(..).collect()
    }

    /// Register or replace a discovered native `.dll` candidate for `Mods` UI inspection/approval.
    pub fn register_native_candidate(&self, candidate: NativeModCandidateState) {
        let key = candidate.record.mod_id.clone();
        self.native_candidates.lock().insert(key, candidate);
    }

    /// Fetch a cloned [`NativeModCandidateState`] by `mod_id`.
    pub fn get_native_candidate(&self, mod_id: &str) -> Option<NativeModCandidateState> {
        self.native_candidates.lock().get(mod_id).cloned()
    }

    /// Update trust/admission status, optional live `SdHandle`, and optional diagnostic for `mod_id`.
    pub fn update_native_candidate_status(
        &self,
        mod_id: &str,
        status: NativeModStatus,
        sha256: Option<String>,
        owner: Option<SdHandle>,
        diagnostic: Option<String>,
    ) -> bool {
        let mut guard = self.native_candidates.lock();
        let Some(entry) = guard.get_mut(mod_id) else {
            return false;
        };
        entry.record.status = status;
        if let Some(hash) = sha256 {
            entry.record.sha256 = hash;
        }
        entry.owner = owner;
        entry.record.diagnostic = diagnostic;
        true
    }

    /// Return all registered native `.dll` candidate summaries sorted by `mod_id`.
    pub fn list_native_mods(&self) -> Vec<NativeModUiRecord> {
        self.native_candidates
            .lock()
            .values()
            .map(|c| c.record.clone())
            .collect()
    }

    /// Build a combined [`ModsUiSnapshot`] merging native `.dll` records and [`ModManager`] state.
    pub fn mods_ui_snapshot(&self) -> ModsUiSnapshot {
        let native_mods = self.list_native_mods();
        let authoring_mods = self
            .mod_manager
            .lock()
            .as_ref()
            .map(|mgr| {
                mgr.snapshot()
                    .into_iter()
                    .map(|entry| AuthoringModUiRecord {
                        id: entry.id,
                        name: entry.name,
                        version: entry.version,
                        author: entry.author,
                        state: entry.state.as_str().to_string(),
                        can_enable: entry.can_enable,
                        can_disable: entry.can_disable,
                        diagnostic: entry.diagnostic,
                    })
                    .collect()
            })
            .unwrap_or_default();
        ModsUiSnapshot {
            native_mods,
            authoring_mods,
            base_ready: self.is_base_ready(),
        }
    }

    /// Read per-region mod enablement overrides (`mod_id -> enabled`) for `region_id`.
    pub fn get_region_mods(&self, region_id: &str) -> BTreeMap<String, bool> {
        self.region_mod_overrides
            .lock()
            .get(region_id)
            .cloned()
            .unwrap_or_default()
    }

    /// Set and persist a per-region mod enablement override (`region_id`, `mod_id`, `enabled`).
    pub fn set_region_mod_enabled(
        &self,
        region_id: &str,
        mod_id: &str,
        enabled: bool,
    ) -> Result<(), String> {
        let reg = region_id.trim();
        let mid = mod_id.trim();
        if reg.is_empty() || reg.contains(',') || reg.contains('\n') || mid.is_empty() || mid.contains(',') || mid.contains('\n') {
            return Err("Invalid region_id or mod_id for region mod context".into());
        }
        let mut guard = self.region_mod_overrides.lock();
        guard
            .entry(reg.to_string())
            .or_default()
            .insert(mid.to_string(), enabled);
        if let Some(path) = self.region_mods_path.lock().as_ref() {
            let mut lines = Vec::new();
            for (r, mods) in guard.iter() {
                for (m, &en) in mods {
                    lines.push(format!("{r},{m},{en}"));
                }
            }
            let body = if lines.is_empty() {
                String::new()
            } else {
                format!("{}\n", lines.join("\n"))
            };
            fs::write(path, body).map_err(|e| format!("Failed saving region_mods.toon: {e}"))?;
        }
        Ok(())
    }

    /// Set the currently active region context (or `None` when in global/main-menu context).
    pub fn set_active_region(&self, region_id: Option<String>) {
        *self.active_region_id.lock() = region_id.and_then(|s| {
            let trimmed = s.trim().to_string();
            (!trimmed.is_empty()).then_some(trimmed)
        });
    }

    /// Return the currently active region ID, if any.
    pub fn active_region(&self) -> Option<String> {
        self.active_region_id.lock().clone()
    }

    /// Compute effective enablement for `mod_id` in `region_id` (`region_override.unwrap_or(global_enabled)`).
    pub fn effective_mod_enabled(&self, region_id: Option<&str>, mod_id: &str) -> bool {
        if let Some(reg) = region_id
            && let Some(overridden) = self
                .region_mod_overrides
                .lock()
                .get(reg)
                .and_then(|m| m.get(mod_id))
                .copied()
        {
            return overridden;
        }
        self.mod_manager
            .lock()
            .as_ref()
            .is_some_and(|mgr| mgr.enabled_order().iter().any(|id| id == mod_id))
    }

    /// Resolve effective in-memory Schema 3 raw resources for `region_id` (falling back to global state).
    pub fn effective_active_raw_resources(
        &self,
        region_id: Option<&str>,
    ) -> Vec<simoder_mods::ActiveRawResource> {
        let overrides = region_id
            .map(|reg| self.get_region_mods(reg))
            .unwrap_or_default();
        self.mod_manager
            .lock()
            .as_ref()
            .map_or_else(Vec::new, |mgr| mgr.effective_raw_resources(&overrides))
    }

    /// Borrow the one-shot [`BootstrapCoordinator`].
    pub const fn bootstrap(&self) -> &BootstrapCoordinator {
        &self.bootstrap
    }

    /// Borrow the shared central [`DiagnosticLogPipeline`].
    pub fn logs(&self) -> &Arc<DiagnosticLogPipeline> {
        &self.logs
    }

    /// Access the Backend <-> Frontend [`BridgeCorrelationTracker`] under its lock.
    pub fn with_bridge_tracker_mut<R>(
        &self,
        f: impl FnOnce(&mut BridgeCorrelationTracker) -> R,
    ) -> R {
        let mut guard = self.bridge_tracker.lock();
        f(&mut guard)
    }

    /// Access the [`NativeModuleHost`] (`ABI v2`) under its lock.
    pub fn with_native_host_mut<R>(&self, f: impl FnOnce(&mut NativeModuleHost) -> R) -> R {
        let mut guard = self.native_host.lock();
        f(&mut guard)
    }

    /// Access the decoupled [`HostToolRegistry`] under its lock.
    pub fn with_tool_registry_mut<R>(&self, f: impl FnOnce(&mut HostToolRegistry) -> R) -> R {
        let mut guard = self.tool_registry.lock();
        f(&mut guard)
    }

    /// Access the [`ScriptScheduler`] (`Luau` + `QuickJS`) under its lock.
    pub fn with_script_scheduler_mut<R>(&self, f: impl FnOnce(&mut ScriptScheduler) -> R) -> R {
        let mut guard = self.script_scheduler.lock();
        f(&mut guard)
    }

    /// Access the [`ModManager`] if the runtime booted for `SimCity.exe`.
    pub fn with_mod_manager_mut<R>(&self, f: impl FnOnce(Option<&mut ModManager>) -> R) -> R {
        let mut guard = self.mod_manager.lock();
        f(guard.as_mut())
    }

    /// Access the [`CrashHandlerCoordinator`] if the runtime booted for `SimCity.exe`.
    pub fn with_crash_handler_mut<R>(
        &self,
        f: impl FnOnce(Option<&mut CrashHandlerCoordinator>) -> R,
    ) -> R {
        let mut guard = self.crash_handler.lock();
        f(guard.as_mut())
    }

    /// Execute the one-shot bootstrap sequence and initialize all runtime subsystems when running inside `SimCity.exe`.
    pub fn boot(
        &self,
        proxy_dll_path: &Path,
        executable_path: &Path,
        system_dir: &Path,
        timestamp_ms: u64,
    ) -> Result<BootstrapOutcome, D3d9Error> {
        let outcome =
            self.bootstrap
                .try_begin_bootstrap(proxy_dll_path, executable_path, system_dir)?;

        if let BootstrapOutcome::Initialized {
            ref paths,
            ref config,
            ..
        } = outcome
        {
            let _ = self.logs.push(
                timestamp_ms,
                LogSourceType::Loader,
                "simoder-d3d9",
                LogLevel::Info,
                1,
                &format!(
                    "Simoder runtime initialized (mods_dir='{}', enabled={}, dev_mode={})",
                    config.mods_dir, config.enabled, config.dev_mode
                ),
            );

            let crashes_dir = paths.data_root.join("crashes");
            let fallback_crashes_dir = paths.data_root.join("crashes-fallback");
            let logs_dir = paths.data_root.join("logs");
            *self.crash_handler.lock() = Some(CrashHandlerCoordinator::new(
                &crashes_dir,
                &fallback_crashes_dir,
                &logs_dir,
                SymbolCatalog::default(),
            ));

            let mods_root = paths.game_root.join(&config.mods_dir);
            let state_path = paths.data_root.join("state.toon");
            let region_mods_path = paths.data_root.join("region_mods.toon");
            if let Ok(raw) = fs::read_to_string(&region_mods_path) {
                let mut loaded: BTreeMap<String, BTreeMap<String, bool>> = BTreeMap::new();
                for line in raw.lines() {
                    let parts: Vec<&str> = line.trim().splitn(3, ',').collect();
                    if parts.len() == 3
                        && !parts[0].is_empty()
                        && !parts[1].is_empty()
                        && let Ok(en) = parts[2].parse::<bool>()
                    {
                        loaded
                            .entry(parts[0].to_string())
                            .or_default()
                            .insert(parts[1].to_string(), en);
                    }
                }
                *self.region_mod_overrides.lock() = loaded;
            }
            *self.region_mods_path.lock() = Some(region_mods_path);
            let patch_registry = Arc::new(PatchRegistry::new());
            *self.mod_manager.lock() = Some(ModManager::new(&mods_root, &state_path, patch_registry));

            self.native_host.lock().set_runtime_readiness(
                false,
                SdLifecycleState::Starting,
                SD_READY_RUNTIME,
            );
        }

        Ok(outcome)
    }

    /// Initialize [`ModManager`] under `mods_root` and gate [`NativeModuleHost`] readiness on a valid `simoder.base` Luau installation.
    ///
    /// Returns `Ok(true)` once `mods/simoder.base` is validated with a readable non-symlink Luau entrypoint
    /// and runtime is enabled; returns `Ok(false)` when mods are disabled; returns `Err(reason)` if `simoder.base`
    /// is missing or invalid (keeping `base_ready = false` so [`Self::load_and_admit_native_module`] returns `SdResult::NotReady`).
    pub fn initialize_mods_and_base(&self, mods_root: &Path, enabled: bool) -> Result<bool, String> {
        let now_ms = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |d| u64::try_from(d.as_millis()).unwrap_or(u64::MAX));

        if let Some(mgr) = self.mod_manager.lock().as_mut()
            && let Err(err) = mgr.initialize()
        {
            let _ = self.logs.push(
                now_ms,
                LogSourceType::Loader,
                "simoder-d3d9",
                LogLevel::Warning,
                1,
                &format!("ModManager initialization reported issues: {err}"),
            );
        }

        if !enabled {
            self.base_ready.store(false, Ordering::Release);
            self.native_host.lock().set_runtime_readiness(
                false,
                SdLifecycleState::Starting,
                SD_READY_RUNTIME,
            );
            return Ok(false);
        }

        let base_dir = mods_root.join(RESERVED_BASE_MOD_ID);
        let base_result = (|| -> Result<(), String> {
            let base_def = load_base_mod_definition(&base_dir)
                .map_err(|e| format!("invalid '{RESERVED_BASE_MOD_ID}' at '{}': {e}", base_dir.display()))?;
            let scripts = base_def.scripts().ok_or_else(|| {
                format!("'{RESERVED_BASE_MOD_ID}' manifest must declare a [scripts] table")
            })?;
            if scripts.language != "luau" {
                return Err(format!(
                    "'{RESERVED_BASE_MOD_ID}' must use Luau scripts (found '{}')",
                    scripts.language
                ));
            }
            let src = fs::read_to_string(&scripts.resolved_path).map_err(|e| {
                format!(
                    "failed reading '{RESERVED_BASE_MOD_ID}' entrypoint '{}': {e}",
                    scripts.resolved_path.display()
                )
            })?;
            if src.trim().is_empty() {
                return Err(format!(
                    "'{RESERVED_BASE_MOD_ID}' script entrypoint '{}' is empty",
                    scripts.resolved_path.display()
                ));
            }
            Ok(())
        })();

        match base_result {
            Ok(()) => {
                self.base_ready.store(true, Ordering::Release);
                self.native_host.lock().set_runtime_readiness(
                    true,
                    SdLifecycleState::Ready,
                    SD_READY_RUNTIME | SD_READY_MENU,
                );
                let _ = self.logs.push(
                    now_ms,
                    LogSourceType::Mod,
                    RESERVED_BASE_MOD_ID,
                    LogLevel::Info,
                    1,
                    "Verified mandatory Luau base runtime; NativeModuleHost readiness promoted to Ready",
                );
                Ok(true)
            }
            Err(reason) => {
                self.base_ready.store(false, Ordering::Release);
                self.native_host.lock().set_runtime_readiness(
                    false,
                    SdLifecycleState::Starting,
                    SD_READY_RUNTIME,
                );
                Err(reason)
            }
        }
    }

    /// Stage (`PrepareV2`) and admit (`AdmitV2`) a trusted `ABI v2` native mod `.dll` (such as `simoder_mcp.dll`).
    pub fn load_and_admit_native_module(
        &self,
        reg: &NativeModuleRegistration,
        callbacks: Arc<dyn NativeModuleCallbacks>,
    ) -> Result<SdHandle, SdResult> {
        let mut host = self.native_host.lock();
        let owner = host.prepare_module(reg, callbacks)?;
        let admit_res = host.admit_module(owner);
        if admit_res != SdResult::Ok {
            let _ = host.quiesce_module(owner);
            let _ = host.drain_module(owner);
            return Err(admit_res);
        }
        Ok(owner)
    }

    /// Cooperatively quiesce and drain a loaded native module generation, then mark runtime stopped if requested.
    pub fn shutdown_native_module(&self, owner: SdHandle, mark_runtime_stopped: bool) -> SdResult {
        let mut host = self.native_host.lock();
        let _ = host.quiesce_module(owner);
        let drain_res = host.drain_module(owner);
        if mark_runtime_stopped && drain_res == SdResult::Ok {
            host.set_runtime_readiness(false, SdLifecycleState::Stopped, 0);
        }
        drain_res
    }
}
