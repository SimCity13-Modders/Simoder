#![allow(unsafe_code)]

//! Audited Win32 vtable hook for `EAWebKitClient::DebugLog` (`vtable[3]` / `+0x0C`).
//!
//! Resolves `gEAWebKitClient` dynamically from the loaded `EAWebKit.dll` via
//! [`crate::eawebkit_debuglog_resolver::resolve_eawebkit_client_global`] (String-XREF
//! anchor, zero hardcoded RVAs), verifies the live `EAWebKitClient` vtable pointer,
//! and atomically swaps only the 4-byte `DebugLog` slot (`+0x0C`) while preserving
//! `GetLastError()`, chaining to the original slot, and restoring on shutdown.

#[cfg(not(target_arch = "x86"))]
use std::sync::Arc;

#[cfg(not(target_arch = "x86"))]
use simoder_core::DiagnosticLogPipeline;

/// Install the `EAWebKitClient::DebugLog` vtable hook on 32-bit `x86` Windows builds.
///
/// Returns `Ok(true)` when newly installed, `Ok(false)` when already installed, or
/// `Err(reason)` if `EAWebKit.dll` or `gEAWebKitClient` is not ready or fails validation.
#[cfg(not(target_arch = "x86"))]
pub(crate) fn install_eawebkit_debuglog_hook(
    _pipeline: Arc<DiagnosticLogPipeline>,
) -> Result<bool, String> {
    Ok(false)
}

/// Restore the original `EAWebKitClient::DebugLog` vtable slot if patched.
#[cfg(not(target_arch = "x86"))]
pub(crate) fn uninstall_eawebkit_debuglog_hook() {}

#[cfg(target_arch = "x86")]
mod x86_impl {
    use std::cell::Cell;
    use std::ffi::c_void;
    use std::sync::Arc;
    use std::sync::atomic::{AtomicUsize, Ordering};

    use parking_lot::Mutex;
    use simoder_core::{DiagnosticLogPipeline, LogLevel, LogSourceType};
    use simoder_crash::EAWEBKIT_CLIENT_DEBUGLOG_VTABLE_OFFSET;

    use crate::config::MIN_VALID_USER_ADDRESS;
    use crate::pe::resolve_eawebkit_client_global;
    use crate::win32_console::{current_thread_id, unix_time_ms};
    use crate::win32_hook_util::{
        patch_pointer_slot, read_bytes_exact, read_remote_cstr, read_struct, snapshot_loaded_module,
    };

    const MAX_DEBUGLOG_BYTES: usize = 4096;
    const EAWEBKIT_DEBUGLOG_TYPE_JAVASCRIPT: i32 = 1;

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct EaWebKitDebugLogInfo {
        mp_view: usize,
        mp_user_data: usize,
        mp_log_text: *const u8,
        m_type: i32,
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetLastError() -> u32;
        fn SetLastError(dw_err_code: u32);
    }

    type FnDebugLog = unsafe extern "thiscall" fn(*mut c_void, *const EaWebKitDebugLogInfo);

    static DEBUGLOG_PIPELINE: Mutex<Option<Arc<DiagnosticLogPipeline>>> = Mutex::new(None);
    static PATCHED_VTABLE_SLOT: Mutex<Option<(usize, usize)>> = Mutex::new(None);
    static ORIG_DEBUGLOG_FN: AtomicUsize = AtomicUsize::new(0);

    thread_local! {
        static IN_DEBUGLOG: Cell<bool> = const { Cell::new(false) };
    }

    unsafe extern "thiscall" fn hook_eawebkit_debuglog(
        this: *mut c_void,
        info_ptr: *const EaWebKitDebugLogInfo,
    ) {
        // SAFETY: Preserves caller `GetLastError()`, chains to the original `DebugLog` slot,
        // guards against re-entrancy, and reads `EaWebKitDebugLogInfo` + text via checked `ReadProcessMemory`.
        unsafe {
            let saved_err = GetLastError();
            let orig_addr = ORIG_DEBUGLOG_FN.load(Ordering::Acquire);
            if orig_addr != 0 {
                let orig: FnDebugLog = std::mem::transmute(orig_addr);
                orig(this, info_ptr);
            }

            let entered = IN_DEBUGLOG.with(|flag| {
                if flag.get() {
                    false
                } else {
                    flag.set(true);
                    true
                }
            });

            if entered {
                if !info_ptr.is_null()
                    && let Some(info) = read_struct::<EaWebKitDebugLogInfo>(info_ptr as usize)
                    && !info.mp_log_text.is_null()
                    && let Some(raw_text) =
                        read_remote_cstr(info.mp_log_text as usize, MAX_DEBUGLOG_BYTES)
                {
                    let trimmed = raw_text.trim_end_matches(['\r', '\n']);
                    if !trimmed.is_empty()
                        && let Some(pipeline) = DEBUGLOG_PIPELINE.lock().clone()
                    {
                        let level = if trimmed.starts_with("JSError:") {
                            LogLevel::Error
                        } else if trimmed.starts_with("JSWarning:") {
                            LogLevel::Warning
                        } else {
                            LogLevel::Info
                        };
                        let source = if info.m_type == EAWEBKIT_DEBUGLOG_TYPE_JAVASCRIPT {
                            "EAWebKit::Console"
                        } else {
                            "EAWebKit::DebugLog"
                        };
                        let _ = pipeline.push(
                            unix_time_ms(),
                            LogSourceType::Game,
                            source,
                            level,
                            current_thread_id(),
                            trimmed,
                        );
                    }
                }
                IN_DEBUGLOG.with(|flag| flag.set(false));
            }

            SetLastError(saved_err);
        }
    }

    pub(crate) fn install_eawebkit_debuglog_hook(
        pipeline: Arc<DiagnosticLogPipeline>,
    ) -> Result<bool, String> {
        let mut slot_guard = PATCHED_VTABLE_SLOT.lock();
        if slot_guard.is_some() {
            return Ok(false);
        }

        let snap = snapshot_loaded_module(Some("EAWebKit.dll"))?;
        let resolved = resolve_eawebkit_client_global(
            snap.image_base,
            snap.text_rva,
            &snap.text,
            snap.rdata_rva,
            &snap.rdata,
        )
        .ok_or_else(|| {
            "EAWebKitClient::DebugLog String-XREF anchor did not match uniquely".to_string()
        })?;

        let client_ptr = read_struct::<u32>(resolved.client_global_ptr_va as usize)
            .filter(|&p| p >= MIN_VALID_USER_ADDRESS)
            .ok_or_else(|| "gEAWebKitClient is null or uninitialized".to_string())?;

        let vtable_ptr = read_struct::<u32>(client_ptr as usize)
            .filter(|&p| p >= MIN_VALID_USER_ADDRESS)
            .ok_or_else(|| "EAWebKitClient vtable pointer is invalid".to_string())?;

        let slot_addr = vtable_ptr
            .checked_add(EAWEBKIT_CLIENT_DEBUGLOG_VTABLE_OFFSET)
            .ok_or_else(|| "EAWebKitClient vtable slot overflow".to_string())?;

        let orig_fn = read_struct::<u32>(slot_addr as usize)
            .filter(|&p| p >= MIN_VALID_USER_ADDRESS)
            .ok_or_else(|| "EAWebKitClient::DebugLog slot target is invalid".to_string())?;

        let mut code_probe = [0u8; 3];
        if !read_bytes_exact(orig_fn as usize, &mut code_probe) {
            return Err("EAWebKitClient::DebugLog slot target is unreadable".to_string());
        }

        *DEBUGLOG_PIPELINE.lock() = Some(pipeline);
        ORIG_DEBUGLOG_FN.store(orig_fn as usize, Ordering::Release);

        let hook_addr = hook_eawebkit_debuglog as *const () as usize;
        if !patch_pointer_slot(slot_addr as usize, hook_addr) {
            ORIG_DEBUGLOG_FN.store(0, Ordering::Release);
            *DEBUGLOG_PIPELINE.lock() = None;
            return Err("VirtualProtect failed on EAWebKitClient vtable slot".to_string());
        }

        *slot_guard = Some((slot_addr as usize, orig_fn as usize));
        Ok(true)
    }

    pub(crate) fn uninstall_eawebkit_debuglog_hook() {
        let mut slot_guard = PATCHED_VTABLE_SLOT.lock();
        if let Some((slot_addr, orig_fn)) = slot_guard.take() {
            let _ = patch_pointer_slot(slot_addr, orig_fn);
        }
        ORIG_DEBUGLOG_FN.store(0, Ordering::Release);
        *DEBUGLOG_PIPELINE.lock() = None;
    }
}

#[cfg(target_arch = "x86")]
pub(crate) use x86_impl::{install_eawebkit_debuglog_hook, uninstall_eawebkit_debuglog_hook};
