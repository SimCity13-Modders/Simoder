#![forbid(unsafe_code)]

//! Owning-thread UI synchronization, `Simoder-Base` host lifecycle, hot-reload,
//! sandboxed third-party Luau mod loading, and `EAWebKit` patch dispatch.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, OnceLock};

use parking_lot::Mutex;
use simoder_core::{LogLevel, LogSourceType};
use simoder_mods::{KeybindDescriptor, KeybindSnapshotEntry, RESERVED_BASE_MOD_ID};

use crate::base_bridge::{BaseRuntimeHost, ModsActionBackend};
use crate::base_hot_reload::{BaseHotReloadState, load_applicator_script};
use crate::eawebkit_bridge::{execute_eawebkit_packet, is_eawebkit_bridge_ready};
use crate::proxy_ffi::global_simoder_runtime;
use crate::resource_bridge::{is_resource_bridge_ready, sync_in_memory_raw_resources};
use crate::runtime::ModsUiSnapshot;
use crate::ui_patch_host::{
    UI_PATCH_APPLICATOR_ES5, build_sync_packet_full, parse_sync_reply,
};
use crate::win32_console::unix_time_ms;
use crate::win32_loader::{LOADER_STOPPING, emit_log, try_attach_eawebkit_debuglog_hook};
use crate::win32_native::{approve_and_load_native_mod, deny_native_mod, revoke_native_mod};
use crate::win32_window_dispatch::set_modal_overlay_active;

static EAWEBKIT_VIEW_SEEN: AtomicBool = AtomicBool::new(false);
static LAST_HOT_RELOAD_POLL_MS: AtomicU64 = AtomicU64::new(0);
static BASE_HOST_SLOT: OnceLock<Mutex<Option<BaseRuntimeHost>>> = OnceLock::new();
static BASE_HOT_RELOAD: OnceLock<Mutex<Option<BaseHotReloadState>>> = OnceLock::new();
static ACTIVE_LUAU_MODS: OnceLock<Mutex<BTreeMap<String, u64>>> = OnceLock::new();
static LUAU_MODS_DIRTY: AtomicBool = AtomicBool::new(true);

fn base_host_slot() -> &'static Mutex<Option<BaseRuntimeHost>> {
    BASE_HOST_SLOT.get_or_init(|| Mutex::new(None))
}

fn base_hot_reload_slot() -> &'static Mutex<Option<BaseHotReloadState>> {
    BASE_HOT_RELOAD.get_or_init(|| Mutex::new(None))
}

struct Win32ModsBackend;

impl ModsActionBackend for Win32ModsBackend {
    fn list_mods(&self) -> ModsUiSnapshot {
        global_simoder_runtime().mods_ui_snapshot()
    }

    fn approve_native(&self, mod_id: &str, sha256: &str) -> Result<(), String> {
        if crate::win32_dev_ui::is_dev_ui_mod(mod_id) {
            crate::win32_dev_ui::approve_dev_ui_mod(mod_id, sha256)
        } else {
            approve_and_load_native_mod(mod_id, sha256)
        }
    }

    fn deny_native(&self, mod_id: &str, sha256: &str) -> Result<(), String> {
        if crate::win32_dev_ui::is_dev_ui_mod(mod_id) {
            crate::win32_dev_ui::deny_dev_ui_mod(mod_id, sha256)
        } else {
            deny_native_mod(mod_id, sha256)
        }
    }

    fn revoke_native(&self, mod_id: &str) -> Result<(), String> {
        if crate::win32_dev_ui::is_dev_ui_mod(mod_id) {
            crate::win32_dev_ui::revoke_dev_ui_mod(mod_id)
        } else {
            revoke_native_mod(mod_id)
        }
    }

    fn set_enabled(&self, mod_id: &str, enabled: bool) -> Result<(), String> {
        global_simoder_runtime().with_mod_manager_mut(|maybe| {
            let mgr = maybe.ok_or_else(|| "ModManager unavailable".to_string())?;
            mgr.set_enabled(mod_id, enabled).map_err(|e| e.to_string())
        })?;
        LUAU_MODS_DIRTY.store(true, Ordering::Release);
        sync_raw_resources_on_game_thread();
        Ok(())
    }

    fn get_region_mods(&self, region_id: &str) -> BTreeMap<String, bool> {
        global_simoder_runtime().get_region_mods(region_id)
    }

    fn set_region_mod_enabled(
        &self,
        region_id: &str,
        mod_id: &str,
        enabled: bool,
    ) -> Result<(), String> {
        global_simoder_runtime().set_region_mod_enabled(region_id, mod_id, enabled)?;
        LUAU_MODS_DIRTY.store(true, Ordering::Release);
        sync_raw_resources_on_game_thread();
        Ok(())
    }

    fn list_keybinds(&self) -> Vec<KeybindSnapshotEntry> {
        global_simoder_runtime().list_keybinds()
    }

    fn register_keybind(&self, desc: KeybindDescriptor) -> Result<String, String> {
        global_simoder_runtime().register_keybind(desc)
    }

    fn rebind_keybind(&self, id: &str, chord: Option<&str>) -> Result<Option<String>, String> {
        global_simoder_runtime().rebind_keybind(id, chord)
    }

    fn reset_keybind(&self, id: &str) -> Result<(), String> {
        global_simoder_runtime().reset_keybind(id)
    }
}

/// Boot `Simoder-Base`, initialize hot-reload state, and load enabled Luau authoring mods.
pub(crate) fn start_base_ui_host(mods_root: &Path) {
    let base_dir = mods_root.join(RESERVED_BASE_MOD_ID);
    match BaseRuntimeHost::start_from_base_dir(
        &base_dir,
        Arc::new(global_simoder_runtime()),
        Arc::new(Win32ModsBackend),
    ) {
        Ok(host) => {
            sync_luau_authoring_mods(&host, 0);
            *base_host_slot().lock() = Some(host);
            let watcher = BaseHotReloadState::new(base_dir.clone());
            let (_, ext_app) = load_applicator_script(&base_dir);
            *base_hot_reload_slot().lock() = Some(watcher);
            emit_log(
                LogLevel::Info,
                LogSourceType::Mod,
                RESERVED_BASE_MOD_ID,
                &format!(
                    "Initialized Simoder-Base Luau UI composition & Mods UI services (hot_reload=true, external_applicator={ext_app})"
                ),
            );
        }
        Err(err) => emit_log(
            LogLevel::Error,
            LogSourceType::Mod,
            RESERVED_BASE_MOD_ID,
            &format!("Failed booting Simoder-Base UI host: {err}"),
        ),
    }
}

/// Release `Simoder-Base` and hot-reload watcher instances during safe DLL teardown.
pub(crate) fn shutdown_base_ui_host() {
    if let Some(mut slot) = base_host_slot().try_lock() {
        *slot = None;
    }
    if let Some(mut watcher) = base_hot_reload_slot().try_lock() {
        *watcher = None;
    }
}

fn sync_luau_authoring_mods(base: &BaseRuntimeHost, reload_gen: u64) {
    if !LUAU_MODS_DIRTY.swap(false, Ordering::AcqRel) {
        return;
    }
    let runtime = global_simoder_runtime();
    let active_region = runtime.active_region();
    let desired: BTreeMap<String, PathBuf> = runtime
        .enabled_luau_authoring_mods(active_region.as_deref())
        .into_iter()
        .collect();
    let mut active = ACTIVE_LUAU_MODS
        .get_or_init(|| Mutex::new(BTreeMap::new()))
        .lock();

    let to_revoke: Vec<String> = active
        .keys()
        .filter(|id| !desired.contains_key(id.as_str()))
        .cloned()
        .collect();
    for mod_id in to_revoke {
        active.remove(&mod_id);
        base.revoke_mod_in_base(&mod_id);
        emit_log(
            LogLevel::Info,
            LogSourceType::Mod,
            &mod_id,
            "Unloaded sandboxed Luau mod",
        );
    }

    for (mod_id, entry_path) in desired {
        if active.get(&mod_id).copied() == Some(reload_gen) {
            continue;
        }
        let Ok(meta) = fs::symlink_metadata(&entry_path) else {
            emit_log(
                LogLevel::Warning,
                LogSourceType::Mod,
                &mod_id,
                &format!("Luau script entry missing: '{}'", entry_path.display()),
            );
            continue;
        };
        if meta.file_type().is_symlink() || !meta.is_file() {
            emit_log(
                LogLevel::Warning,
                LogSourceType::Mod,
                &mod_id,
                &format!(
                    "Rejected invalid/symlink Luau entry: '{}'",
                    entry_path.display()
                ),
            );
            continue;
        }
        let source = match fs::read_to_string(&entry_path) {
            Ok(s) => s,
            Err(err) => {
                emit_log(
                    LogLevel::Warning,
                    LogSourceType::Mod,
                    &mod_id,
                    &format!("Failed reading Luau script '{}': {err}", entry_path.display()),
                );
                continue;
            }
        };
        match base.run_sandboxed_mod_script(
            &mod_id,
            reload_gen.saturating_add(1),
            &source,
            "onEnable",
        ) {
            Ok(()) => {
                active.insert(mod_id.clone(), reload_gen);
                emit_log(
                    LogLevel::Info,
                    LogSourceType::Mod,
                    &mod_id,
                    "Loaded sandboxed Luau mod",
                );
            }
            Err(err) => {
                emit_log(
                    LogLevel::Warning,
                    LogSourceType::Mod,
                    &mod_id,
                    &format!("Sandboxed Luau mod error: {err}"),
                );
            }
        }
    }
}

fn sync_raw_resources_on_game_thread() {
    if !is_resource_bridge_ready() {
        return;
    }
    let runtime = global_simoder_runtime();
    let active_region = runtime.active_region();
    let resources = runtime.effective_active_raw_resources(active_region.as_deref());
    match sync_in_memory_raw_resources(&resources) {
        Ok(Some(count)) => emit_log(
            LogLevel::Info,
            LogSourceType::Loader,
            "simoder.resources",
            &format!("Synchronized {count} in-memory raw resources with IResourceManager"),
        ),
        Ok(None) => {}
        Err(err) => emit_log(
            LogLevel::Warning,
            LogSourceType::Loader,
            "simoder.resources",
            &format!("In-memory raw-resources sync deferred/failed: {err}"),
        ),
    }
}

/// Periodic owning-thread UI sync tick registered on `win32_window_dispatch`.
pub(crate) fn on_ui_sync_tick(hwnd: usize) {
    if LOADER_STOPPING.load(Ordering::Acquire) || !is_eawebkit_bridge_ready() {
        return;
    }
    sync_raw_resources_on_game_thread();
    let runtime = global_simoder_runtime();
    let now_ms = unix_time_ms();
    let last_poll = LAST_HOT_RELOAD_POLL_MS.load(Ordering::Acquire);
    if now_ms.saturating_sub(last_poll) >= 1000
        && LAST_HOT_RELOAD_POLL_MS
            .compare_exchange(last_poll, now_ms, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        && let (Some(watcher), Some(host)) = (
            base_hot_reload_slot().lock().as_mut(),
            base_host_slot().lock().as_mut(),
        )
    {
        match watcher.check_and_reload(host, Arc::new(runtime), Arc::new(Win32ModsBackend)) {
            Ok(Some(report)) => {
                LUAU_MODS_DIRTY.store(true, Ordering::Release);
                emit_log(
                    LogLevel::Info,
                    LogSourceType::Mod,
                    RESERVED_BASE_MOD_ID,
                    &format!(
                        "Hot-reloaded Simoder-Base (gen={}, external_applicator={})",
                        report.reload_gen, report.external_applicator
                    ),
                );
            }
            Ok(None) => {}
            Err(err) => emit_log(
                LogLevel::Warning,
                LogSourceType::Mod,
                RESERVED_BASE_MOD_ID,
                &format!("Simoder-Base hot-reload rejected (keeping previous version): {err}"),
            ),
        }
    }

    static LAST_DEV_UI_VIEW: OnceLock<Mutex<(usize, String)>> = OnceLock::new();
    static LAST_BRIDGE_ERR: OnceLock<Mutex<Option<(String, u64, bool)>>> = OnceLock::new();
    let mut view_changed = false;

    for _pass in 0..2 {
        let (reload_gen, applicator_script) = {
            let guard = base_hot_reload_slot().lock();
            if let Some(watcher) = guard.as_ref() {
                (watcher.reload_gen(), watcher.applicator_script().to_string())
            } else {
                (0, UI_PATCH_APPLICATOR_ES5.to_string())
            }
        };
        if let Some(base) = base_host_slot().lock().as_ref() {
            sync_luau_authoring_mods(base, reload_gen);
            if let Err(err) = base.tick_ui() {
                emit_log(
                    LogLevel::Warning,
                    LogSourceType::Mod,
                    RESERVED_BASE_MOD_ID,
                    &format!("Simoder-Base UI tick error: {err}"),
                );
                return;
            }
        }
        let (revision, patches) = runtime.composed_ui_snapshot();
        set_modal_overlay_active(patches.iter().any(|p| p.slot == "modal.overlay"));
        let Ok(packet) = build_sync_packet_full(
            revision,
            reload_gen,
            &patches,
            runtime.skip_intro_requested(),
        ) else {
            return;
        };
        match execute_eawebkit_packet(hwnd, &applicator_script, &packet) {
            Ok(raw_reply) => match parse_sync_reply(&raw_reply) {
                Ok(reply) => {
                    let recovered = LAST_BRIDGE_ERR
                        .get_or_init(|| Mutex::new(None))
                        .lock()
                        .take()
                        .is_some();
                    if !EAWEBKIT_VIEW_SEEN.swap(true, Ordering::AcqRel) {
                        emit_log(
                            LogLevel::Info,
                            LogSourceType::Loader,
                            "simoder.ui",
                            "Connected Simoder-Base declarative UI bridge to active EAWebKit view",
                        );
                        try_attach_eawebkit_debuglog_hook(true);
                    }
                    for entry in reply.console_logs {
                        let lvl = LogLevel::parse(&entry.level).unwrap_or(LogLevel::Info);
                        emit_log(lvl, LogSourceType::Game, &entry.source, &entry.message);
                    }
                    let next_region = reply
                        .active_region_id
                        .map(|s| s.trim().to_string())
                        .filter(|s| !s.is_empty());
                    if next_region != runtime.active_region() {
                        runtime.set_active_region(next_region);
                        sync_raw_resources_on_game_thread();
                    }
                    let prev_obs = runtime.get_ui_observation();
                    let obs_changed = reply
                        .observation
                        .as_ref()
                        .is_some_and(|obs| *obs != prev_obs);
                    let cur_phase = reply
                        .observation
                        .as_ref()
                        .map(|obs| obs.phase.clone())
                        .unwrap_or_default();
                    {
                        let mut last_view = LAST_DEV_UI_VIEW
                            .get_or_init(|| Mutex::new((0, String::new())))
                            .lock();
                        if recovered || last_view.0 != hwnd || last_view.1 != cur_phase {
                            emit_log(
                                LogLevel::Debug,
                                LogSourceType::Loader,
                                "simoder.ui",
                                &format!(
                                    "UI view phase -> '{cur_phase}' (hwnd=0x{hwnd:X}, recovered={recovered})"
                                ),
                            );
                            *last_view = (hwnd, cur_phase);
                            view_changed = true;
                        }
                    }
                    if let Some(obs) = reply.observation {
                        runtime.set_ui_observation(obs);
                    }
                    let has_events = !reply.events.is_empty();
                    if has_events {
                        runtime.push_ui_events(reply.events);
                    }
                    if !has_events && !obs_changed {
                        break;
                    }
                }
                Err(err) => {
                    emit_log(LogLevel::Warning, LogSourceType::Loader, "simoder.ui", &err);
                    break;
                }
            },
            Err(err) => {
                let mut err_slot = LAST_BRIDGE_ERR
                    .get_or_init(|| Mutex::new(None))
                    .lock();
                match err_slot.as_mut() {
                    Some((prev_msg, _first_ms, _logged)) if *prev_msg == err => {}
                    _ => {
                        emit_log(
                            LogLevel::Debug,
                            LogSourceType::Loader,
                            "simoder.ui",
                            &format!("EAWebKit UI bridge error: {err}"),
                        );
                        *err_slot = Some((err, now_ms, true));
                    }
                }
                break;
            }
        }
    }
    crate::win32_dev_ui::dispatch_dev_ui_drag_move(hwnd);
    crate::win32_dev_ui::sync_dev_ui_mods_on_tick(hwnd, view_changed);
}
