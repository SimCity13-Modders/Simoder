#![allow(unsafe_code)]

//! Win32 system `d3d9.dll` resolver, background bootstrap launcher, and clean runtime teardown
//! for production `i686-pc-windows-msvc` deployment beside `SimCity.exe`.

use std::ffi::{CString, OsStr, c_void};
use std::fs::{self, OpenOptions};
use std::io::Write as _;
use std::mem::transmute;
use std::os::windows::ffi::OsStrExt as _;
use std::path::{Path, PathBuf};
use std::ptr::{null, null_mut};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, OnceLock};
use std::time::Duration;

use parking_lot::Mutex;
use simoder_core::{LogConsoleFilter, LogLevel, LogSourceType};
use simoder_crash::BOOTSTRAP_LOG_RELATIVE_PATH;
use simoder_mods::RESERVED_BASE_MOD_ID;

use crate::base_bridge::{BaseRuntimeHost, ModsActionBackend};
use crate::base_hot_reload::{BaseHotReloadState, load_applicator_script};
use crate::bootstrap::BootstrapOutcome;
use crate::eawebkit_bridge::{
    execute_eawebkit_packet, initialize_eawebkit_bridge, is_eawebkit_bridge_ready,
};
use crate::exports::D3d9Export;
use crate::proxy_ffi::{
    FnDirect3DCreate9, FnDirect3DCreate9Ex, global_d3d9_forwarder, global_simoder_runtime,
};
use crate::runtime::ModsUiSnapshot;
use crate::ui_patch_host::{
    UI_PATCH_APPLICATOR_ES5, build_sync_packet_with_reload, parse_sync_reply,
};
use crate::win32_console::{
    current_thread_id, start_global_logger, stop_global_logger, unix_time_ms,
};
use crate::win32_native::{
    approve_and_load_native_mod, deny_native_mod, revoke_native_mod, scan_and_load_native_mods,
    shutdown_admitted_native_owners,
};
use crate::win32_window_dispatch::{
    bind_game_window, configure_ui_tick, current_bound_window, find_primary_game_window,
    is_window_dispatch_drained, set_modal_overlay_active, shutdown_window_dispatch,
};

type HModule = *mut c_void;
type FarProc = *const c_void;

const LOAD_LIBRARY_SEARCH_SYSTEM32: u32 = 0x0000_0800;
const GET_MODULE_HANDLE_EX_FLAG_PIN: u32 = 0x0000_0001;
const GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS: u32 = 0x0000_0004;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetSystemDirectoryW(lp_buffer: *mut u16, u_size: u32) -> u32;
    fn LoadLibraryExW(lp_lib_file_name: *const u16, h_file: *mut c_void, dw_flags: u32) -> HModule;
    fn GetProcAddress(h_module: HModule, lp_proc_name: *const i8) -> FarProc;
    fn GetModuleHandleExW(
        dw_flags: u32,
        lp_module_name: *const u16,
        ph_module: *mut HModule,
    ) -> i32;
    fn GetModuleFileNameW(h_module: HModule, lp_filename: *mut u16, n_size: u32) -> u32;
    fn GetLastError() -> u32;
    fn SetLastError(dw_err_code: u32);
    fn DisableThreadLibraryCalls(h_lib_module: HModule) -> i32;
}

static PROXY_HMODULE: AtomicUsize = AtomicUsize::new(0);
static SYSTEM_D3D9_INIT: OnceLock<()> = OnceLock::new();
static BOOTSTRAP_STARTED: AtomicBool = AtomicBool::new(false);
static LOADER_STOPPING: AtomicBool = AtomicBool::new(false);
static EAWEBKIT_VIEW_SEEN: AtomicBool = AtomicBool::new(false);
static LAST_HOT_RELOAD_POLL_MS: AtomicU64 = AtomicU64::new(0);
static BASE_HOST_SLOT: OnceLock<Mutex<Option<BaseRuntimeHost>>> = OnceLock::new();
static BASE_HOT_RELOAD: OnceLock<Mutex<Option<BaseHotReloadState>>> = OnceLock::new();

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
        approve_and_load_native_mod(mod_id, sha256)
    }

    fn deny_native(&self, mod_id: &str, sha256: &str) -> Result<(), String> {
        deny_native_mod(mod_id, sha256)
    }

    fn revoke_native(&self, mod_id: &str) -> Result<(), String> {
        revoke_native_mod(mod_id)
    }

    fn set_enabled(&self, mod_id: &str, enabled: bool) -> Result<(), String> {
        global_simoder_runtime().with_mod_manager_mut(|maybe| {
            let mgr = maybe.ok_or_else(|| "ModManager unavailable".to_string())?;
            mgr.set_enabled(mod_id, enabled).map_err(|e| e.to_string())
        })
    }

    fn get_region_mods(&self, region_id: &str) -> std::collections::BTreeMap<String, bool> {
        global_simoder_runtime().get_region_mods(region_id)
    }

    fn set_region_mod_enabled(
        &self,
        region_id: &str,
        mod_id: &str,
        enabled: bool,
    ) -> Result<(), String> {
        global_simoder_runtime().set_region_mod_enabled(region_id, mod_id, enabled)
    }
}

fn on_ui_sync_tick(hwnd: usize) {
    if LOADER_STOPPING.load(Ordering::Acquire) || !is_eawebkit_bridge_ready() {
        return;
    }
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
            Ok(Some(report)) => emit_log(
                LogLevel::Info,
                LogSourceType::Mod,
                RESERVED_BASE_MOD_ID,
                &format!(
                    "Hot-reloaded Simoder-Base (gen={}, external_applicator={})",
                    report.reload_gen, report.external_applicator
                ),
            ),
            Ok(None) => {}
            Err(err) => emit_log(
                LogLevel::Warning,
                LogSourceType::Mod,
                RESERVED_BASE_MOD_ID,
                &format!("Simoder-Base hot-reload rejected (keeping previous version): {err}"),
            ),
        }
    }

    for _pass in 0..2 {
        if let Some(base) = base_host_slot().lock().as_ref()
            && let Err(err) = base.tick_ui()
        {
            emit_log(
                LogLevel::Warning,
                LogSourceType::Mod,
                RESERVED_BASE_MOD_ID,
                &format!("Simoder-Base UI tick error: {err}"),
            );
            return;
        }
        let (revision, patches) = runtime.composed_ui_snapshot();
        set_modal_overlay_active(patches.iter().any(|p| p.slot == "modal.overlay"));
        let (reload_gen, applicator_script) = {
            let guard = base_hot_reload_slot().lock();
            if let Some(watcher) = guard.as_ref() {
                (watcher.reload_gen(), watcher.applicator_script().to_string())
            } else {
                (0, UI_PATCH_APPLICATOR_ES5.to_string())
            }
        };
        let Ok(packet) = build_sync_packet_with_reload(revision, reload_gen, &patches) else {
            return;
        };
        match execute_eawebkit_packet(hwnd, &applicator_script, &packet) {
            Ok(raw_reply) => match parse_sync_reply(&raw_reply) {
                Ok(reply) => {
                    if !EAWEBKIT_VIEW_SEEN.swap(true, Ordering::AcqRel) {
                        emit_log(
                            LogLevel::Info,
                            LogSourceType::Loader,
                            "simoder.ui",
                            "Connected Simoder-Base declarative UI bridge to active EAWebKit view",
                        );
                    }
                    let prev_obs = runtime.get_ui_observation();
                    let obs_changed = reply
                        .observation
                        .as_ref()
                        .is_some_and(|obs| *obs != prev_obs);
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
            Err(_not_ready) => break,
        }
    }
}

/// Record our own proxy `HMODULE` and disable per-thread `DLL_THREAD_ATTACH` notifications.
pub fn on_process_attach(instance: HModule) {
    PROXY_HMODULE.store(instance as usize, Ordering::Release);
    // SAFETY: Valid `HINSTANCE` supplied by Windows loader in `DllMain(DLL_PROCESS_ATTACH)`.
    unsafe {
        let _ = DisableThreadLibraryCalls(instance);
    }
}

/// Load genuine `<System32>\d3d9.dll` once and populate [`global_d3d9_forwarder`].
pub fn ensure_system_d3d9_loaded() {
    SYSTEM_D3D9_INIT.get_or_init(|| {
        // SAFETY: Preserves caller `GetLastError()`, resolves `<System32>\d3d9.dll` via `LOAD_LIBRARY_SEARCH_SYSTEM32`,
        // and binds real Direct3D9 entrypoints into `global_d3d9_forwarder()`.
        unsafe {
            let prev_err = GetLastError();
            let Some(sys_dir) = query_system_directory() else {
                SetLastError(prev_err);
                return;
            };
            let sys_d3d9 = sys_dir.join("d3d9.dll");
            let wide = to_wide_null(sys_d3d9.as_os_str());
            let h_orig = LoadLibraryExW(wide.as_ptr(), null_mut(), LOAD_LIBRARY_SEARCH_SYSTEM32);
            if h_orig.is_null() {
                SetLastError(prev_err);
                return;
            }
            let own = PROXY_HMODULE.load(Ordering::Acquire) as HModule;
            if !own.is_null() && h_orig == own {
                SetLastError(prev_err);
                return;
            }

            let forwarder = global_d3d9_forwarder();
            for &export in crate::exports::all_d3d9_exports() {
                let proc_ptr = if let Some(sym) = export.symbol_name() {
                    CString::new(sym)
                        .ok()
                        .map_or(null(), |cstr| GetProcAddress(h_orig, cstr.as_ptr()))
                } else {
                    GetProcAddress(h_orig, export.ordinal() as usize as *const i8)
                };
                if !proc_ptr.is_null() {
                    forwarder.set_ordinal_target(export, proc_ptr as usize);
                }
            }

            let c9 = forwarder.ordinal_target(D3d9Export::Direct3DCreate9);
            if c9 != 0 {
                forwarder.set_create9_target(Some(transmute::<usize, FnDirect3DCreate9>(c9)));
            }
            let c9ex = forwarder.ordinal_target(D3d9Export::Direct3DCreate9Ex);
            if c9ex != 0 {
                forwarder
                    .set_create9ex_target(Some(transmute::<usize, FnDirect3DCreate9Ex>(c9ex)));
            }

            SetLastError(prev_err);
        }
    });
}

/// Pin `d3d9.dll` in process memory and spawn the one-shot Simoder runtime bootstrap worker.
pub fn spawn_bootstrap_once() {
    if BOOTSTRAP_STARTED
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        return;
    }

    // SAFETY: Preserves caller `GetLastError()` and pins this DLL (`GET_MODULE_HANDLE_EX_FLAG_PIN`)
    // before starting a background initialization thread off the graphics call stack.
    unsafe {
        let prev_err = GetLastError();
        let mut pinned: HModule = null_mut();
        let _ = GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            spawn_bootstrap_once as *const () as *const u16,
            &mut pinned,
        );
        SetLastError(prev_err);
    }

    let _ = std::thread::Builder::new()
        .name("simoder-bootstrap".to_string())
        .spawn(run_bootstrap_worker);
}

/// Cooperatively quiesce and drain loaded native mod generations on safe DLL detach.
pub fn shutdown_loaded_native_modules() {
    LOADER_STOPPING.store(true, Ordering::Release);
    shutdown_window_dispatch();
    for _ in 0..40 {
        if is_window_dispatch_drained() {
            break;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    if let Some(mut slot) = base_host_slot().try_lock() {
        *slot = None;
    }
    if let Some(mut watcher) = base_hot_reload_slot().try_lock() {
        *watcher = None;
    }
    shutdown_admitted_native_owners();
    stop_global_logger();
}

fn run_bootstrap_worker() {
    let own = PROXY_HMODULE.load(Ordering::Acquire) as HModule;
    let proxy_path = if !own.is_null() {
        query_module_filename(own)
    } else {
        // SAFETY: Queries module handle owning `spawn_bootstrap_once`.
        unsafe {
            let mut h: HModule = null_mut();
            let _ = GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                spawn_bootstrap_once as *const () as *const u16,
                &mut h,
            );
            query_module_filename(h)
        }
    };
    let executable_path = query_module_filename(null_mut());
    let system_dir = query_system_directory();

    let (Some(proxy_dll_path), Some(executable_path), Some(system_dir)) =
        (proxy_path, executable_path, system_dir)
    else {
        return;
    };

    let unix_ms = unix_time_ms();

    match global_simoder_runtime().boot(&proxy_dll_path, &executable_path, &system_dir, unix_ms) {
        Ok(BootstrapOutcome::Initialized {
            paths,
            config,
            build,
        }) => {
            let build_verified = build.as_ref().is_some_and(|b| b.verified_retail_file);
            let _ = start_global_logger(
                &paths.data_root,
                Arc::clone(global_simoder_runtime().logs()),
                LogConsoleFilter::from(&config.developer_console),
                build_verified,
            );
            let console_active = crate::win32_console::is_developer_console_active();

            let summary = format!(
                "Simoder runtime initialized: pid={} exe='{}' mods_dir='{}' enabled={} dev_mode={} build_verified={} console_enabled={} console_active={} level={}",
                std::process::id(),
                executable_path.display(),
                config.mods_dir,
                config.enabled,
                config.dev_mode,
                build_verified,
                config.developer_console.enabled,
                console_active,
                config.developer_console.level.as_str(),
            );
            emit_log(LogLevel::Info, LogSourceType::Loader, "simoder-d3d9", &summary);
            append_bootstrap_log(&paths.data_root, &summary);

            if !build_verified {
                emit_log(
                    LogLevel::Warning,
                    LogSourceType::Loader,
                    "simoder-d3d9",
                    "Game build fingerprint does not match verified 10.3.6.0 profile; Developer Console suppressed",
                );
            }

            let mods_root = paths.game_root.join(&config.mods_dir);
            if config.enabled && !mods_root.join(RESERVED_BASE_MOD_ID).exists() {
                let _ = crate::win32_console::prompt_and_bootstrap_base(&mods_root);
            }

            if let Err(err) =
                global_simoder_runtime().initialize_mods_and_base(&mods_root, config.enabled)
            {
                emit_log(
                    LogLevel::Error,
                    LogSourceType::Loader,
                    "simoder-d3d9",
                    &format!(
                        "Mandatory '{RESERVED_BASE_MOD_ID}' check failed ({err}); native module admission blocked (SdResult::NotReady)"
                    ),
                );
            }

            scan_and_load_native_mods(&paths, &mods_root, &config);

            if config.enabled && global_simoder_runtime().is_base_ready() {
                let base_dir = mods_root.join(RESERVED_BASE_MOD_ID);
                match BaseRuntimeHost::start_from_base_dir(
                    &base_dir,
                    Arc::new(global_simoder_runtime()),
                    Arc::new(Win32ModsBackend),
                ) {
                    Ok(host) => {
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

                if let Err(err) = initialize_eawebkit_bridge(build_verified) {
                    emit_log(
                        LogLevel::Warning,
                        LogSourceType::Loader,
                        "simoder.ui",
                        &format!("EAWebKit UI bridge unavailable: {err}"),
                    );
                } else {
                    configure_ui_tick(on_ui_sync_tick);
                    while !LOADER_STOPPING.load(Ordering::Acquire) {
                        if let Some(hwnd) = find_primary_game_window()
                            && hwnd != current_bound_window()
                            && bind_game_window(hwnd)
                        {
                            emit_log(
                                LogLevel::Info,
                                LogSourceType::Loader,
                                "simoder.ui",
                                &format!("Attached UI sync dispatcher to game HWND 0x{hwnd:X}"),
                            );
                        }
                        std::thread::sleep(Duration::from_millis(250));
                    }
                }
            }
        }
        Ok(BootstrapOutcome::DisabledByMarkerFile { marker_path }) => {
            if let Some(data_root) = marker_path.parent() {
                append_bootstrap_log(
                    data_root,
                    "Simoder runtime disabled by proxy.disabled marker; graphics forwarding active",
                );
            }
        }
        Ok(BootstrapOutcome::DisabledNonSimCityProcess { .. })
        | Ok(BootstrapOutcome::SkippedAlreadyRequested) => {}
        Err(err) => {
            if let Some(parent) = proxy_dll_path.parent().and_then(Path::parent) {
                append_bootstrap_log(&parent.join("simoder"), &format!("Bootstrap error: {err}"));
            }
        }
    }
}

fn query_system_directory() -> Option<PathBuf> {
    let mut buf = vec![0u16; 32768];
    // SAFETY: Valid mutable UTF-16 output buffer of length `32768`.
    let len = unsafe { GetSystemDirectoryW(buf.as_mut_ptr(), buf.len() as u32) } as usize;
    if len == 0 || len >= buf.len() {
        return None;
    }
    Some(PathBuf::from(String::from_utf16_lossy(&buf[..len])))
}

fn query_module_filename(h_mod: HModule) -> Option<PathBuf> {
    let mut buf = vec![0u16; 32768];
    // SAFETY: Valid mutable UTF-16 output buffer and `HMODULE` (or `null` for executable).
    let len = unsafe { GetModuleFileNameW(h_mod, buf.as_mut_ptr(), buf.len() as u32) } as usize;
    if len == 0 || len >= buf.len() {
        return None;
    }
    Some(PathBuf::from(String::from_utf16_lossy(&buf[..len])))
}

pub(crate) fn to_wide_null(s: &OsStr) -> Vec<u16> {
    s.encode_wide().chain(std::iter::once(0)).collect()
}

pub(crate) fn emit_log(
    level: LogLevel,
    source_type: LogSourceType,
    source_name: &str,
    message: &str,
) {
    let _ = global_simoder_runtime().logs().push(
        unix_time_ms(),
        source_type,
        source_name,
        level,
        current_thread_id(),
        message,
    );
}

fn append_bootstrap_log(data_root: &Path, message: &str) {
    let log_path = data_root.join(BOOTSTRAP_LOG_RELATIVE_PATH);
    if let Some(parent) = log_path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(&log_path) {
        let _ = writeln!(file, "[simoder] {message}");
    }
}
