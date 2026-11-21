#![forbid(unsafe_code)]

//! Integrated top-level runtime coordinator uniting `simoder-core`, `simoder-formats`,
//! `simoder-mods`, `simoder-scripting`, and `simoder-crash` inside the single `d3d9.dll` proxy.

use std::path::Path;
use std::sync::Arc;

use parking_lot::Mutex;
use simoder_core::{DiagnosticLogPipeline, LogLevel, LogSourceType};
use simoder_crash::{BridgeCorrelationTracker, CrashHandlerCoordinator, SymbolCatalog};
use simoder_mods::{
    HostToolRegistry, ModManager, NativeModuleCallbacks, NativeModuleHost,
    NativeModuleRegistration, PatchRegistry, SD_READY_MENU, SD_READY_RUNTIME, SdHandle,
    SdLifecycleState, SdResult,
};
use simoder_scripting::{FaultPolicy, ScriptScheduler};

use crate::D3d9Error;
use crate::bootstrap::{BootstrapCoordinator, BootstrapOutcome};

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
        }
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
            let patch_registry = Arc::new(PatchRegistry::new());
            *self.mod_manager.lock() = Some(ModManager::new(&mods_root, &state_path, patch_registry));

            self.native_host.lock().set_runtime_readiness(
                config.enabled,
                SdLifecycleState::Ready,
                SD_READY_RUNTIME | SD_READY_MENU,
            );
        }

        Ok(outcome)
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
