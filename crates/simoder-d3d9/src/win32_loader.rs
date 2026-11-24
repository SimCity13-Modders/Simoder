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
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, OnceLock};
use std::time::Duration;

use simoder_core::{LogConsoleFilter, LogLevel, LogSourceType};
use simoder_crash::BOOTSTRAP_LOG_RELATIVE_PATH;
use simoder_mods::RESERVED_BASE_MOD_ID;

use crate::bootstrap::BootstrapOutcome;
use crate::eawebkit_bridge::initialize_eawebkit_bridge;
use crate::exports::D3d9Export;
use crate::proxy_ffi::{
    FnDirect3DCreate9, FnDirect3DCreate9Ex, global_d3d9_forwarder, global_simoder_runtime,
};
use crate::resource_bridge::{initialize_resource_bridge, shutdown_in_memory_raw_resources};
use crate::win32_console::{current_thread_id, start_global_logger, stop_global_logger, unix_time_ms};
use crate::win32_native::{scan_and_load_native_mods, shutdown_admitted_native_owners};
use crate::win32_ui_sync::{on_ui_sync_tick, shutdown_base_ui_host, start_base_ui_host};
use crate::win32_window_dispatch::{
    bind_game_window, configure_ui_tick, current_bound_window, ensure_window_dispatch_healthy,
    find_primary_game_window, is_window_dispatch_drained, shutdown_window_dispatch,
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
pub(crate) static LOADER_STOPPING: AtomicBool = AtomicBool::new(false);

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
    shutdown_base_ui_host();
    shutdown_in_memory_raw_resources();
    crate::win32_dev_ui::shutdown_dev_ui_mods();
    shutdown_admitted_native_owners();
    crate::win32_eawebkit_debuglog_hook::uninstall_eawebkit_debuglog_hook();
    crate::win32_ods_hook::uninstall_output_debug_string_hooks();
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
            crate::win32_crash::install_crash_handler(&paths.data_root);
            emit_log(
                LogLevel::Info,
                LogSourceType::Loader,
                "simoder.crash",
                "Installed Win32 unhandled exception crash handler (crashes/ + report UI)",
            );
            let console_active = crate::win32_console::is_developer_console_active();
            let ods_hooks = crate::win32_ods_hook::install_output_debug_string_hooks(
                Arc::clone(global_simoder_runtime().logs()),
            );
            emit_log(
                LogLevel::Info,
                LogSourceType::Loader,
                "simoder.console",
                &format!(
                    "Installed {ods_hooks} OutputDebugStringA/W IAT hook(s) for [GAME] log capture"
                ),
            );

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

            if config.enabled {
                crate::win32_dev_ui::init_dev_ui_mods(
                    &paths.data_root,
                    &mods_root,
                    config.dev_mode || config.developer_console.enabled,
                );
            }

            if config.enabled && global_simoder_runtime().is_base_ready() {
                start_base_ui_host(&mods_root);

                if let Err(err) = initialize_resource_bridge(build_verified) {
                    emit_log(
                        LogLevel::Warning,
                        LogSourceType::Loader,
                        "simoder.resources",
                        &format!("In-memory resource bridge unavailable: {err}"),
                    );
                } else {
                    emit_log(
                        LogLevel::Info,
                        LogSourceType::Loader,
                        "simoder.resources",
                        "Initialized verified in-memory IResourceManager / DatabasePackedFile bridge",
                    );
                }

                if let Err(err) = initialize_eawebkit_bridge(build_verified) {
                    emit_log(
                        LogLevel::Warning,
                        LogSourceType::Loader,
                        "simoder.ui",
                        &format!("EAWebKit UI bridge unavailable: {err}"),
                    );
                } else {
                    try_attach_eawebkit_debuglog_hook(false);
                    configure_ui_tick(on_ui_sync_tick);
                    let mut last_watch: Option<(Option<usize>, usize)> = None;
                    let mut last_failed_bind: Option<usize> = None;
                    while !LOADER_STOPPING.load(Ordering::Acquire) {
                        let found = find_primary_game_window();
                        let bound = current_bound_window();
                        if last_watch != Some((found, bound)) {
                            last_watch = Some((found, bound));
                            let found_str = match found {
                                Some(h) => format!("0x{h:X}"),
                                None => "none".to_string(),
                            };
                            emit_log(
                                LogLevel::Debug,
                                LogSourceType::Loader,
                                "simoder.ui",
                                &format!("Window watcher: found={found_str}, bound=0x{bound:X}"),
                            );
                        }
                        if let Some(hwnd) = found {
                            if hwnd != bound {
                                if bind_game_window(hwnd) {
                                    last_failed_bind = None;
                                    emit_log(
                                        LogLevel::Info,
                                        LogSourceType::Loader,
                                        "simoder.ui",
                                        &format!("Attached UI sync dispatcher to game HWND 0x{hwnd:X}"),
                                    );
                                } else if last_failed_bind != Some(hwnd) {
                                    last_failed_bind = Some(hwnd);
                                    emit_log(
                                        LogLevel::Warning,
                                        LogSourceType::Loader,
                                        "simoder.ui",
                                        &format!("bind_game_window(0x{hwnd:X}) returned false"),
                                    );
                                }
                            } else if let Some(reason) = ensure_window_dispatch_healthy(hwnd) {
                                emit_log(
                                    LogLevel::Info,
                                    LogSourceType::Loader,
                                    "simoder.ui",
                                    &format!(
                                        "Re-attached UI sync dispatcher to game HWND 0x{hwnd:X} ({reason})"
                                    ),
                                );
                            }
                        } else if bound != 0
                            && let Some(reason) = ensure_window_dispatch_healthy(bound)
                        {
                            emit_log(
                                LogLevel::Debug,
                                LogSourceType::Loader,
                                "simoder.ui",
                                &format!(
                                    "Detached UI sync dispatcher from game HWND 0x{bound:X} ({reason})"
                                ),
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

pub(crate) fn try_attach_eawebkit_debuglog_hook(warn_on_err: bool) {
    match crate::win32_eawebkit_debuglog_hook::install_eawebkit_debuglog_hook(Arc::clone(
        global_simoder_runtime().logs(),
    )) {
        Ok(true) => emit_log(
            LogLevel::Info,
            LogSourceType::Loader,
            "simoder.ui",
            "Installed EAWebKitClient::DebugLog vtable hook via String-XREF anchor",
        ),
        Ok(false) => {}
        Err(err) if warn_on_err => emit_log(
            LogLevel::Warning,
            LogSourceType::Loader,
            "simoder.ui",
            &format!("EAWebKitClient::DebugLog vtable hook skipped: {err}"),
        ),
        Err(_) => {}
    }
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
