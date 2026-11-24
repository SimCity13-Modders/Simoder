#![allow(unsafe_code)]

//! Generic `ABI v2` native `.dll` mod scanner, SHA-256-pinned deferred trust admission,
//! staging directory verifier, and `LOAD_LIBRARY_SEARCH_SYSTEM32` loader for `simoder-d3d9`.

use std::ffi::{CString, c_void};
use std::fs;
use std::mem::transmute;
use std::path::{Path, PathBuf};
use std::ptr::null_mut;
use std::sync::{Arc, OnceLock};

use parking_lot::Mutex;
use simoder_core::{Digest, LogLevel, LogSourceType, Sha256, hex};
use simoder_formats::LoaderConfig;
use simoder_mods::{
    MAX_NATIVE_SOURCE_DLL_BYTES, ModDefinition, NATIVE_TRUST_FILE_NAME, NativeModConfig,
    NativeModStatus, NativeModUiRecord, NativeModuleCallbacks, NativeModuleRegistration,
    NativeTrustDecision, NativeTrustStore, RESERVED_BASE_MOD_ID, SD_U2A_HOST_VERSION, SdHandle,
    SdModuleDescriptorV2, SdResult, load_base_mod_definition, load_mod_definition,
    reject_reparse_or_symlink,
};

use crate::bootstrap::InstallPaths;
use crate::pe::Pe32Image;
use crate::proxy_ffi::global_simoder_runtime;
use crate::runtime::NativeModCandidateState;
use crate::win32_loader::{emit_log, to_wide_null};
use crate::win32_window_dispatch::{eval_js_on_game_thread, wake_ui_dispatch};

type HModule = *mut c_void;
type FarProc = *const c_void;

const LOAD_LIBRARY_SEARCH_SYSTEM32: u32 = 0x0000_0800;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn LoadLibraryExW(lp_lib_file_name: *const u16, h_file: *mut c_void, dw_flags: u32) -> HModule;
    fn GetProcAddress(h_module: HModule, lp_proc_name: *const i8) -> FarProc;
    fn GetLastError() -> u32;
}

#[link(name = "shell32")]
unsafe extern "system" {
    fn ShellExecuteW(
        hwnd: *mut c_void,
        lpOperation: *const u16,
        lpFile: *const u16,
        lpParameters: *const u16,
        lpDirectory: *const u16,
        nShowCmd: i32,
    ) -> *mut c_void;
}

/// Open an external URL or file path using the default Windows shell handler.
pub fn open_url_in_browser(url: &str) {
    let op: Vec<u16> = "open\0".encode_utf16().collect();
    let file_wide: Vec<u16> = url.encode_utf16().chain(std::iter::once(0)).collect();
    // SAFETY: Invokes Windows ShellExecuteW with null parent HWND and SW_SHOWNORMAL (1).
    unsafe {
        let _ = ShellExecuteW(
            null_mut(),
            op.as_ptr(),
            file_wide.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            1,
        );
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SdHeaderRaw {
    size: u32,
    version: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SdHandleRaw {
    slot: u32,
    generation: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SdHostApiV2Raw {
    header: SdHeaderRaw,
    owner: SdHandleRaw,
    lifecycle_api_ptr: usize,
    dispatch_api_ptr: usize,
    credential_api_ptr: usize,
    semantic_api_ptr: usize,
}

#[repr(C)]
struct SdSemanticHostApiV2Raw {
    size: u32,
    version: u32,
    eval_ui_js: extern "C" fn(
        script_ptr: *const u8,
        script_len: usize,
        arg_ptr: *const u8,
        arg_len: usize,
        out_buf: *mut u8,
        out_cap: usize,
        out_len: *mut usize,
        timeout_ms: u32,
    ) -> u32,
    log_message: extern "C" fn(
        level: u32,
        source_ptr: *const u8,
        source_len: usize,
        msg_ptr: *const u8,
        msg_len: usize,
    ) -> u32,
}

const MAX_SEMANTIC_JS_INPUT_BYTES: usize = 128 * 1024;

extern "C" fn host_eval_ui_js(
    script_ptr: *const u8,
    script_len: usize,
    arg_ptr: *const u8,
    arg_len: usize,
    out_buf: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
    timeout_ms: u32,
) -> u32 {
    if script_ptr.is_null()
        || script_len == 0
        || script_len > MAX_SEMANTIC_JS_INPUT_BYTES
        || arg_len > MAX_SEMANTIC_JS_INPUT_BYTES
        || (arg_len > 0 && arg_ptr.is_null())
        || out_len.is_null()
    {
        return SdResult::Invalid as u32;
    }

    // SAFETY: Caller passes valid UTF-8 byte ranges for `script_len` and `arg_len`.
    let script_bytes = unsafe { std::slice::from_raw_parts(script_ptr, script_len) };
    let Ok(script) = std::str::from_utf8(script_bytes) else {
        return SdResult::Invalid as u32;
    };
    let arg_bytes = if arg_len == 0 {
        &[]
    } else {
        // SAFETY: `arg_ptr` checked non-null when `arg_len > 0`.
        unsafe { std::slice::from_raw_parts(arg_ptr, arg_len) }
    };
    let Ok(arg) = std::str::from_utf8(arg_bytes) else {
        return SdResult::Invalid as u32;
    };

    let (status_code, payload_bytes): (SdResult, Vec<u8>) =
        match eval_js_on_game_thread(script, arg, timeout_ms) {
            Ok(text) => (SdResult::Ok, text.into_bytes()),
            Err(err_msg) => {
                let code = if err_msg.contains("timed out") {
                    SdResult::Deadline
                } else if err_msg.contains("not ready") {
                    SdResult::NotReady
                } else {
                    SdResult::OperationFailed
                };
                (code, err_msg.into_bytes())
            }
        };

    // SAFETY: `out_len` checked non-null at entrypoint.
    unsafe {
        *out_len = payload_bytes.len();
    }
    if payload_bytes.len() > out_cap || out_buf.is_null() {
        return SdResult::BufferSmall as u32;
    }
    if !payload_bytes.is_empty() {
        // SAFETY: `out_buf` is non-null with capacity `out_cap >= payload_bytes.len()`.
        unsafe {
            std::ptr::copy_nonoverlapping(payload_bytes.as_ptr(), out_buf, payload_bytes.len());
        }
    }
    status_code as u32
}

extern "C" fn host_log_message(
    level: u32,
    source_ptr: *const u8,
    source_len: usize,
    msg_ptr: *const u8,
    msg_len: usize,
) -> u32 {
    if source_ptr.is_null()
        || source_len == 0
        || source_len > 128
        || msg_ptr.is_null()
        || msg_len == 0
        || msg_len > 4096
    {
        return SdResult::Invalid as u32;
    }
    // SAFETY: Caller passes valid UTF-8 byte ranges for `source_len` and `msg_len`.
    let source_bytes = unsafe { std::slice::from_raw_parts(source_ptr, source_len) };
    // SAFETY: Caller passes valid UTF-8 byte ranges for `msg_len`.
    let msg_bytes = unsafe { std::slice::from_raw_parts(msg_ptr, msg_len) };
    let (Ok(source), Ok(msg)) = (std::str::from_utf8(source_bytes), std::str::from_utf8(msg_bytes))
    else {
        return SdResult::Invalid as u32;
    };
    let mapped_level = match level {
        0 => LogLevel::Trace,
        1 => LogLevel::Debug,
        2 => LogLevel::Info,
        3 => LogLevel::Warning,
        _ => LogLevel::Error,
    };
    emit_log(mapped_level, LogSourceType::Mod, source, msg);
    SdResult::Ok as u32
}

static SEMANTIC_HOST_API_V2: SdSemanticHostApiV2Raw = SdSemanticHostApiV2Raw {
    size: size_of::<SdSemanticHostApiV2Raw>() as u32,
    version: 2,
    eval_ui_js: host_eval_ui_js,
    log_message: host_log_message,
};

#[repr(C)]
#[derive(Clone, Copy)]
struct SdModuleDescriptorV2Raw {
    header: SdHeaderRaw,
    host_version: u32,
    base_contract_version: u32,
    reserved0: u32,
    reserved1: u32,
    contract_sha256: [u8; 32],
}

type FnQueryV2 = extern "C" fn(u32, *mut SdModuleDescriptorV2Raw) -> u32;
type FnPrepareV2 = extern "C" fn(*const SdHostApiV2Raw) -> u32;
type FnAdmitV2 = extern "C" fn(SdHandleRaw) -> u32;
type FnQuiesceV2 = extern "C" fn(SdHandleRaw) -> u32;
type FnDrainV2 = extern "C" fn(SdHandleRaw) -> u32;

struct Win32NativeCallbacks {
    query_fn: FnQueryV2,
    prepare_fn: FnPrepareV2,
    admit_fn: FnAdmitV2,
    quiesce_fn: FnQuiesceV2,
    drain_fn: FnDrainV2,
}

impl NativeModuleCallbacks for Win32NativeCallbacks {
    fn query_v2(&self, host_version: u32, descriptor: &mut SdModuleDescriptorV2) -> SdResult {
        let mut raw = SdModuleDescriptorV2Raw {
            header: SdHeaderRaw {
                size: descriptor.header.size,
                version: descriptor.header.version,
            },
            host_version: descriptor.host_version,
            base_contract_version: descriptor.base_contract_version,
            reserved0: descriptor.reserved0,
            reserved1: descriptor.reserved1,
            contract_sha256: descriptor.contract_sha256,
        };
        let code = (self.query_fn)(host_version, &mut raw);
        descriptor.header.size = raw.header.size;
        descriptor.header.version = raw.header.version;
        descriptor.host_version = raw.host_version;
        descriptor.base_contract_version = raw.base_contract_version;
        descriptor.reserved0 = raw.reserved0;
        descriptor.reserved1 = raw.reserved1;
        descriptor.contract_sha256 = raw.contract_sha256;
        SdResult::from_repr(code).unwrap_or(SdResult::Invalid)
    }

    fn prepare_v2(&self, owner: SdHandle) -> SdResult {
        let host = SdHostApiV2Raw {
            header: SdHeaderRaw {
                size: size_of::<SdHostApiV2Raw>() as u32,
                version: SD_U2A_HOST_VERSION,
            },
            owner: SdHandleRaw {
                slot: owner.slot,
                generation: owner.generation,
            },
            lifecycle_api_ptr: 0,
            dispatch_api_ptr: 0,
            credential_api_ptr: 0,
            semantic_api_ptr: &SEMANTIC_HOST_API_V2 as *const SdSemanticHostApiV2Raw as usize,
        };
        let code = (self.prepare_fn)(&host);
        SdResult::from_repr(code).unwrap_or(SdResult::Invalid)
    }

    fn admit_v2(&self, owner: SdHandle) -> SdResult {
        let code = (self.admit_fn)(SdHandleRaw {
            slot: owner.slot,
            generation: owner.generation,
        });
        SdResult::from_repr(code).unwrap_or(SdResult::Invalid)
    }

    fn quiesce_v2(&self, owner: SdHandle) -> SdResult {
        let code = (self.quiesce_fn)(SdHandleRaw {
            slot: owner.slot,
            generation: owner.generation,
        });
        SdResult::from_repr(code).unwrap_or(SdResult::Invalid)
    }

    fn drain_v2(&self, owner: SdHandle) -> SdResult {
        let code = (self.drain_fn)(SdHandleRaw {
            slot: owner.slot,
            generation: owner.generation,
        });
        SdResult::from_repr(code).unwrap_or(SdResult::Invalid)
    }
}

static ADMITTED_NATIVE_OWNERS: OnceLock<Mutex<Vec<SdHandle>>> = OnceLock::new();

fn admitted_owners_list() -> &'static Mutex<Vec<SdHandle>> {
    ADMITTED_NATIVE_OWNERS.get_or_init(|| Mutex::new(Vec::new()))
}

/// Cooperatively quiesce and drain all active native mod generations.
pub(crate) fn shutdown_admitted_native_owners() {
    if let Some(mut owners) = admitted_owners_list().try_lock() {
        let runtime = global_simoder_runtime();
        for owner in owners.drain(..) {
            let _ = runtime.shutdown_native_module(owner, true);
        }
    }
}

/// Scan `mods_root`, validate manifests generically, prompt user trust approval per `(mod_id, sha256)`,
/// stage verified DLLs, and admit compliant `ABI v2` native modules via `LOAD_LIBRARY_SEARCH_SYSTEM32`.
pub(crate) fn scan_and_load_native_mods(
    paths: &InstallPaths,
    mods_root: &Path,
    config: &LoaderConfig,
) {
    let Ok(entries) = fs::read_dir(mods_root) else {
        emit_log(
            LogLevel::Debug,
            LogSourceType::Loader,
            "simoder-d3d9",
            &format!("Mods directory not present yet at '{}'", mods_root.display()),
        );
        return;
    };

    let trust_path = paths.data_root.join(NATIVE_TRUST_FILE_NAME);
    let mut trust_store = NativeTrustStore::load(&trust_path).unwrap_or_else(|err| {
        emit_log(
            LogLevel::Warning,
            LogSourceType::Loader,
            "simoder.native-host",
            &format!(
                "Failed loading '{}'; defaulting to empty trust store: {err}",
                trust_path.display()
            ),
        );
        NativeTrustStore::new()
    });

    let mut dirs: Vec<PathBuf> = entries.flatten().map(|e| e.path()).collect();
    dirs.sort();

    for dir in dirs {
        let Ok(meta) = reject_reparse_or_symlink(&dir) else {
            continue;
        };
        if !meta.is_dir() {
            continue;
        }
        let dir_name = dir.file_name().and_then(|n| n.to_str()).unwrap_or("unknown");
        if dir_name.eq_ignore_ascii_case("cache") {
            continue;
        }

        let def_res = if dir_name.eq_ignore_ascii_case(RESERVED_BASE_MOD_ID) {
            load_base_mod_definition(&dir)
        } else {
            load_mod_definition(&dir)
        };
        let def = match def_res {
            Ok(d) => d,
            Err(err) => {
                emit_log(
                    LogLevel::Warning,
                    LogSourceType::Mod,
                    dir_name,
                    &format!("Skipped mod directory '{dir_name}': {err}"),
                );
                if has_any_dll_file(&dir) {
                    emit_log(
                        LogLevel::Warning,
                        LogSourceType::Loader,
                        "simoder.native-host",
                        &format!(
                            "Directory '{dir_name}' contains .dll files without a valid [native] manifest declaration; refusing automatic load"
                        ),
                    );
                }
                continue;
            }
        };

        let has_scripts = def.scripts().is_some();
        let has_native = def.native().is_some();
        emit_log(
            LogLevel::Info,
            LogSourceType::Mod,
            &def.manifest.id,
            &format!(
                "Discovered mod '{}' (version={}, scripts={}, native={})",
                def.manifest.id, def.manifest.version, has_scripts, has_native
            ),
        );

        if !has_native {
            if has_any_dll_file(&dir) {
                emit_log(
                    LogLevel::Warning,
                    LogSourceType::Loader,
                    &def.manifest.id,
                    &format!(
                        "Mod '{}' contains undeclared .dll file(s); declare [native] entry = \"<name>.dll\" in mod.toml to request native trust approval",
                        def.manifest.id
                    ),
                );
            }
            continue;
        }

        if !config.enabled {
            emit_log(
                LogLevel::Info,
                LogSourceType::Loader,
                &def.manifest.id,
                "Simoder mods disabled in config.toon; skipping native module admission",
            );
            continue;
        }

        if let Some(native_cfg) = def.native() {
            try_admit_native_mod(paths, &trust_path, &mut trust_store, &def, native_cfg);
        }
    }
}

fn has_any_dll_file(dir: &Path) -> bool {
    let Ok(entries) = fs::read_dir(dir) else {
        return false;
    };
    entries.flatten().any(|entry| {
        let path = entry.path();
        path.is_file()
            && path
                .extension()
                .and_then(|e| e.to_str())
                .is_some_and(|ext| ext.eq_ignore_ascii_case("dll"))
    })
}

fn try_admit_native_mod(
    paths: &InstallPaths,
    trust_path: &Path,
    trust_store: &mut NativeTrustStore,
    def: &ModDefinition,
    native_cfg: &NativeModConfig,
) {
    let mod_id = &def.manifest.id;
    let runtime = global_simoder_runtime();
    let (bytes, mapped_image_bytes, sha256) = match read_verified_pe32(&native_cfg.resolved_path) {
        Ok(v) => v,
        Err(reason) => {
            emit_log(
                LogLevel::Error,
                LogSourceType::Loader,
                mod_id,
                &format!("Refused '{}': {reason}", native_cfg.entry),
            );
            runtime.register_native_candidate(NativeModCandidateState {
                record: NativeModUiRecord {
                    mod_id: mod_id.clone(),
                    name: def.manifest.name.clone(),
                    version: def.manifest.version.clone(),
                    entry: native_cfg.entry.clone(),
                    sha256: String::new(),
                    status: NativeModStatus::Failed,
                    diagnostic: Some(reason),
                },
                dll_path: native_cfg.resolved_path.clone(),
                mapped_image_bytes: 0,
                source_size_bytes: 0,
                owner: None,
                trust_path: trust_path.to_path_buf(),
            });
            return;
        }
    };

    if let Some(pinned) = &native_cfg.expected_sha256
        && !pinned.eq_ignore_ascii_case(&sha256)
    {
        let reason = format!(
            "manifest sha256 mismatch against disk bytes for '{}'",
            native_cfg.entry
        );
        emit_log(LogLevel::Error, LogSourceType::Loader, mod_id, &reason);
        runtime.register_native_candidate(NativeModCandidateState {
            record: NativeModUiRecord {
                mod_id: mod_id.clone(),
                name: def.manifest.name.clone(),
                version: def.manifest.version.clone(),
                entry: native_cfg.entry.clone(),
                sha256,
                status: NativeModStatus::Failed,
                diagnostic: Some(reason),
            },
            dll_path: native_cfg.resolved_path.clone(),
            mapped_image_bytes: mapped_image_bytes as u32,
            source_size_bytes: bytes.len() as u64,
            owner: None,
            trust_path: trust_path.to_path_buf(),
        });
        return;
    }

    let base_candidate = NativeModCandidateState {
        record: NativeModUiRecord {
            mod_id: mod_id.clone(),
            name: def.manifest.name.clone(),
            version: def.manifest.version.clone(),
            entry: native_cfg.entry.clone(),
            sha256: sha256.clone(),
            status: NativeModStatus::PendingApproval,
            diagnostic: None,
        },
        dll_path: native_cfg.resolved_path.clone(),
        mapped_image_bytes: mapped_image_bytes as u32,
        source_size_bytes: bytes.len() as u64,
        owner: None,
        trust_path: trust_path.to_path_buf(),
    };
    runtime.register_native_candidate(base_candidate);

    match trust_store.decision_for(mod_id, &sha256) {
        Some(NativeTrustDecision::Approved) => {
            match stage_and_admit_native_dll(
                &paths.data_root,
                mod_id,
                &native_cfg.entry,
                &bytes,
                mapped_image_bytes,
                &sha256,
            ) {
                Ok(owner) => {
                    runtime.update_native_candidate_status(
                        mod_id,
                        NativeModStatus::Active,
                        None,
                        Some(owner),
                        None,
                    );
                }
                Err(reason) => {
                    emit_log(LogLevel::Error, LogSourceType::Loader, mod_id, &reason);
                    runtime.update_native_candidate_status(
                        mod_id,
                        NativeModStatus::Failed,
                        None,
                        None,
                        Some(reason),
                    );
                }
            }
        }
        Some(NativeTrustDecision::Denied) => {
            runtime.update_native_candidate_status(
                mod_id,
                NativeModStatus::Denied,
                None,
                None,
                None,
            );
            emit_log(
                LogLevel::Info,
                LogSourceType::Loader,
                mod_id,
                &format!(
                    "Native DLL '{}' (sha256={sha256}) denied by native_trust.toon",
                    native_cfg.entry
                ),
            );
        }
        None => {
            emit_log(
                LogLevel::Warning,
                LogSourceType::Loader,
                mod_id,
                &format!(
                    "Native DLL '{}' (sha256={sha256}) registered as PendingApproval for in-game Mods UI consent",
                    native_cfg.entry
                ),
            );
            wake_ui_dispatch();
        }
    }
}

/// Approve, persist trust in `native_trust.toon`, re-hash on disk, and admit `mod_id` from the in-game `Mods` UI.
pub(crate) fn approve_and_load_native_mod(mod_id: &str, expected_sha256: &str) -> Result<(), String> {
    let runtime = global_simoder_runtime();
    let candidate = runtime
        .get_native_candidate(mod_id)
        .ok_or_else(|| format!("Unknown native mod '{mod_id}'"))?;
    if candidate.record.status == NativeModStatus::Active {
        return Ok(());
    }
    let (bytes, mapped_image_bytes, actual_sha256) = read_verified_pe32(&candidate.dll_path)?;
    if !actual_sha256.eq_ignore_ascii_case(expected_sha256) {
        runtime.update_native_candidate_status(
            mod_id,
            NativeModStatus::PendingApproval,
            Some(actual_sha256.clone()),
            None,
            Some("DLL bytes changed on disk; re-verify SHA-256 before approving".into()),
        );
        return Err(format!(
            "DLL SHA-256 changed on disk (expected {expected_sha256}, observed {actual_sha256})"
        ));
    }

    let data_root = candidate
        .trust_path
        .parent()
        .ok_or_else(|| "Invalid native_trust.toon path".to_string())?;
    let mut store = NativeTrustStore::load(&candidate.trust_path).unwrap_or_default();
    store
        .set_decision(
            mod_id,
            &candidate.record.entry,
            &actual_sha256,
            NativeTrustDecision::Approved,
        )
        .map_err(|e| e.to_string())?;
    store.save(&candidate.trust_path).map_err(|e| e.to_string())?;

    match stage_and_admit_native_dll(
        data_root,
        mod_id,
        &candidate.record.entry,
        &bytes,
        mapped_image_bytes,
        &actual_sha256,
    ) {
        Ok(owner) => {
            runtime.update_native_candidate_status(
                mod_id,
                NativeModStatus::Active,
                Some(actual_sha256),
                Some(owner),
                None,
            );
            Ok(())
        }
        Err(reason) => {
            runtime.update_native_candidate_status(
                mod_id,
                NativeModStatus::Failed,
                Some(actual_sha256),
                None,
                Some(reason.clone()),
            );
            Err(reason)
        }
    }
}

/// Record user refusal for `(mod_id, expected_sha256)` in `native_trust.toon`.
pub(crate) fn deny_native_mod(mod_id: &str, expected_sha256: &str) -> Result<(), String> {
    let runtime = global_simoder_runtime();
    let candidate = runtime
        .get_native_candidate(mod_id)
        .ok_or_else(|| format!("Unknown native mod '{mod_id}'"))?;
    if !candidate.record.sha256.eq_ignore_ascii_case(expected_sha256) {
        return Err(format!(
            "SHA-256 mismatch denying '{mod_id}': expected {}, got {expected_sha256}",
            candidate.record.sha256
        ));
    }
    let mut store = NativeTrustStore::load(&candidate.trust_path).unwrap_or_default();
    store
        .set_decision(
            mod_id,
            &candidate.record.entry,
            &candidate.record.sha256,
            NativeTrustDecision::Denied,
        )
        .map_err(|e| e.to_string())?;
    store.save(&candidate.trust_path).map_err(|e| e.to_string())?;
    runtime.update_native_candidate_status(mod_id, NativeModStatus::Denied, None, None, None);
    Ok(())
}

/// Cooperatively quiesce/drain `mod_id` and mark it denied in `native_trust.toon`.
pub(crate) fn revoke_native_mod(mod_id: &str) -> Result<(), String> {
    let runtime = global_simoder_runtime();
    let candidate = runtime
        .get_native_candidate(mod_id)
        .ok_or_else(|| format!("Unknown native mod '{mod_id}'"))?;
    if let Some(owner) = candidate.owner {
        let _ = runtime.shutdown_native_module(owner, false);
        admitted_owners_list().lock().retain(|h| *h != owner);
    }
    if !candidate.record.sha256.is_empty() {
        let mut store = NativeTrustStore::load(&candidate.trust_path).unwrap_or_default();
        store
            .set_decision(
                mod_id,
                &candidate.record.entry,
                &candidate.record.sha256,
                NativeTrustDecision::Denied,
            )
            .map_err(|e| e.to_string())?;
        store.save(&candidate.trust_path).map_err(|e| e.to_string())?;
    }
    runtime.update_native_candidate_status(
        mod_id,
        NativeModStatus::Denied,
        None,
        None,
        Some("Revoked by user in Mods UI".into()),
    );
    Ok(())
}

fn read_verified_pe32(dll_path: &Path) -> Result<(Vec<u8>, usize, String), String> {
    let meta = reject_reparse_or_symlink(dll_path).map_err(|e| e.to_string())?;
    if !meta.is_file() {
        return Err("target is not an ordinary file".into());
    }
    let bytes = fs::read(dll_path).map_err(|e| format!("read failed: {e}"))?;
    if bytes.is_empty() || bytes.len() > MAX_NATIVE_SOURCE_DLL_BYTES {
        return Err("file size out of bounds".into());
    }
    let mapped_image_bytes = Pe32Image::parse(&bytes)
        .map_err(|e| format!("PE32 validation failed ({e})"))?
        .image_size() as usize;
    let sha256 = hex::encode(Sha256::digest(&bytes));
    Ok((bytes, mapped_image_bytes, sha256))
}

fn stage_and_admit_native_dll(
    data_root: &Path,
    mod_id: &str,
    entry: &str,
    bytes: &[u8],
    mapped_image_bytes: usize,
    sha256: &str,
) -> Result<SdHandle, String> {
    let staged_path = stage_native_dll(data_root, mod_id, entry, bytes, sha256)
        .ok_or_else(|| format!("Failed staging verified native DLL '{entry}'"))?;
    let wide = to_wide_null(staged_path.as_os_str());
    // SAFETY: Verified 32-bit PE32 copied to isolated staging dir and loaded with LOAD_LIBRARY_SEARCH_SYSTEM32 after explicit user trust approval.
    let h_mod = unsafe { LoadLibraryExW(wide.as_ptr(), null_mut(), LOAD_LIBRARY_SEARCH_SYSTEM32) };
    if h_mod.is_null() {
        // SAFETY: Immediate Win32 `GetLastError` after failed `LoadLibraryExW`.
        let err_code = unsafe { GetLastError() };
        return Err(format!("LoadLibraryExW failed for '{entry}' (win32={err_code})"));
    }
    let callbacks = resolve_abi_v2_callbacks(h_mod)
        .ok_or_else(|| format!("Refused '{entry}': missing required SimoderModule_*V2 exports"))?;
    let reg = NativeModuleRegistration {
        module_id: mod_id.to_string(),
        expected_sha256: sha256.to_string(),
        observed_sha256: sha256.to_string(),
        source_size_bytes: bytes.len(),
        mapped_image_bytes,
        native_trust_approved: true,
    };
    let owner = global_simoder_runtime()
        .load_and_admit_native_module(&reg, callbacks)
        .map_err(|res| format!("NativeModuleHost rejected admission for '{entry}': {res:?}"))?;
    admitted_owners_list().lock().push(owner);
    emit_log(
        LogLevel::Info,
        LogSourceType::Loader,
        mod_id,
        &format!(
            "Admitted native module '{mod_id}' (entry='{entry}', slot={}, gen={}, sha256={sha256})",
            owner.slot, owner.generation
        ),
    );
    Ok(owner)
}

fn stage_native_dll(
    data_root: &Path,
    mod_id: &str,
    entry: &str,
    bytes: &[u8],
    expected_sha256: &str,
) -> Option<PathBuf> {
    let dll_name = Path::new(entry)
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("module.dll");
    let stage_dir = data_root
        .join("native-staging")
        .join(format!("{mod_id}-{}", &expected_sha256[..16]));
    fs::create_dir_all(&stage_dir).ok()?;
    let _ = reject_reparse_or_symlink(&stage_dir).ok()?;
    let staged_dll = stage_dir.join(dll_name);
    fs::write(&staged_dll, bytes).ok()?;
    let _ = reject_reparse_or_symlink(&staged_dll).ok()?;
    let reread = fs::read(&staged_dll).ok()?;
    let actual_sha = hex::encode(Sha256::digest(&reread));
    actual_sha
        .eq_ignore_ascii_case(expected_sha256)
        .then_some(staged_dll)
}

fn resolve_abi_v2_callbacks(h_mod: HModule) -> Option<Arc<dyn NativeModuleCallbacks>> {
    unsafe fn load_sym(h_mod: HModule, name: &str) -> Option<FarProc> {
        let cstr = CString::new(name).ok()?;
        // SAFETY: Valid loaded `HMODULE` and NUL-terminated ASCII export symbol name.
        let p = unsafe { GetProcAddress(h_mod, cstr.as_ptr()) };
        (!p.is_null()).then_some(p)
    }

    // SAFETY: Each symbol signature matches `Simoder-SDK/Native/native.h` undecorated `__cdecl` V2 exports.
    unsafe {
        let q = load_sym(h_mod, "SimoderModule_QueryV2")?;
        let p = load_sym(h_mod, "SimoderModule_PrepareV2")?;
        let a = load_sym(h_mod, "SimoderModule_AdmitV2")?;
        let qu = load_sym(h_mod, "SimoderModule_QuiesceV2")?;
        let d = load_sym(h_mod, "SimoderModule_DrainV2")?;
        Some(Arc::new(Win32NativeCallbacks {
            query_fn: transmute::<FarProc, FnQueryV2>(q),
            prepare_fn: transmute::<FarProc, FnPrepareV2>(p),
            admit_fn: transmute::<FarProc, FnAdmitV2>(a),
            quiesce_fn: transmute::<FarProc, FnQuiesceV2>(qu),
            drain_fn: transmute::<FarProc, FnDrainV2>(d),
        }))
    }
}
