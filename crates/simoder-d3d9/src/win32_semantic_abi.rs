#![allow(unsafe_code)]

//! Win32 clipboard helper and `SdSemanticHostApiV2` (`v1..=v3`) FFI callbacks
//! (`eval_ui_js`, `log_message`, `list_mod_tools`, `invoke_mod_tool`).

use std::collections::BTreeSet;
use std::ffi::c_void;
use std::ptr::null_mut;

use simoder_core::{LogLevel, LogSourceType};
use simoder_mods::SdResult;

use crate::proxy_ffi::global_simoder_runtime;
use crate::win32_loader::emit_log;
use crate::win32_window_dispatch::eval_js_on_game_thread;

const CF_UNICODETEXT: u32 = 13;
const GMEM_MOVEABLE: u32 = 0x0002;
const MAX_CLIPBOARD_TEXT_BYTES: usize = 64 * 1024;
const MAX_SEMANTIC_JS_INPUT_BYTES: usize = 128 * 1024;
const MAX_HOST_TOOL_NAME_BYTES: usize = 256;
const MAX_HOST_TOOL_ARGS_BYTES: usize = 128 * 1024;
const MAX_HOST_TOOL_GRANTS_BYTES: usize = 16 * 1024;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GlobalAlloc(u_flags: u32, dw_bytes: usize) -> *mut c_void;
    fn GlobalLock(h_mem: *mut c_void) -> *mut c_void;
    fn GlobalUnlock(h_mem: *mut c_void) -> i32;
    fn GlobalFree(h_mem: *mut c_void) -> *mut c_void;
}

#[link(name = "user32")]
unsafe extern "system" {
    fn OpenClipboard(h_wnd_new_owner: *mut c_void) -> i32;
    fn CloseClipboard() -> i32;
    fn EmptyClipboard() -> i32;
    fn SetClipboardData(u_format: u32, h_mem: *mut c_void) -> *mut c_void;
}

struct ClipboardGuard;

impl Drop for ClipboardGuard {
    fn drop(&mut self) {
        // SAFETY: Balances a successful `OpenClipboard` call on the current thread.
        unsafe {
            let _ = CloseClipboard();
        }
    }
}

/// Copy UTF-8 `text` into the Windows clipboard as `CF_UNICODETEXT` (bounded to `64 KiB`).
pub(crate) fn copy_text_to_clipboard(text: &str) -> Result<(), String> {
    if text.len() > MAX_CLIPBOARD_TEXT_BYTES {
        return Err(format!(
            "clipboard payload exceeds {} byte limit",
            MAX_CLIPBOARD_TEXT_BYTES
        ));
    }
    let wide: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
    let byte_len = wide
        .len()
        .checked_mul(std::mem::size_of::<u16>())
        .ok_or_else(|| "clipboard allocation size overflow".to_string())?;

    // SAFETY: Opens the process clipboard and ensures closure via `ClipboardGuard`.
    if unsafe { OpenClipboard(null_mut()) } == 0 {
        return Err("OpenClipboard failed".to_string());
    }
    let _guard = ClipboardGuard;

    // SAFETY: Clipboard is open and owned by this thread.
    if unsafe { EmptyClipboard() } == 0 {
        return Err("EmptyClipboard failed".to_string());
    }

    // SAFETY: Allocates moveable global memory sized for UTF-16LE + NUL.
    let hmem = unsafe { GlobalAlloc(GMEM_MOVEABLE, byte_len) };
    if hmem.is_null() {
        return Err("GlobalAlloc failed".to_string());
    }

    // SAFETY: Locks newly allocated `hmem` and copies `byte_len` bytes before unlocking.
    let locked = unsafe { GlobalLock(hmem) };
    if locked.is_null() {
        // SAFETY: `hmem` was not handed to `SetClipboardData`; free it on lock failure.
        unsafe {
            let _ = GlobalFree(hmem);
        }
        return Err("GlobalLock failed".to_string());
    }
    // SAFETY: `locked` points to `byte_len` writable bytes; `wide` is `byte_len` bytes long.
    unsafe {
        std::ptr::copy_nonoverlapping(wide.as_ptr().cast::<u8>(), locked.cast::<u8>(), byte_len);
        let _ = GlobalUnlock(hmem);
    }

    // SAFETY: Transfers ownership of `hmem` to Windows clipboard on success.
    let res = unsafe { SetClipboardData(CF_UNICODETEXT, hmem) };
    if res.is_null() {
        // SAFETY: Ownership was not transferred; free `hmem` to avoid leaking.
        unsafe {
            let _ = GlobalFree(hmem);
        }
        return Err("SetClipboardData failed".to_string());
    }
    Ok(())
}

#[repr(C)]
pub(crate) struct SdSemanticHostApiV2Raw {
    pub(crate) size: u32,
    pub(crate) version: u32,
    pub(crate) eval_ui_js: extern "C" fn(
        script_ptr: *const u8,
        script_len: usize,
        arg_ptr: *const u8,
        arg_len: usize,
        out_buf: *mut u8,
        out_cap: usize,
        out_len: *mut usize,
        timeout_ms: u32,
    ) -> u32,
    pub(crate) log_message: extern "C" fn(
        level: u32,
        source_ptr: *const u8,
        source_len: usize,
        msg_ptr: *const u8,
        msg_len: usize,
    ) -> u32,
    pub(crate) list_mod_tools:
        extern "C" fn(out_buf: *mut u8, out_cap: usize, out_len: *mut usize) -> u32,
    pub(crate) invoke_mod_tool: extern "C" fn(
        name_ptr: *const u8,
        name_len: usize,
        args_ptr: *const u8,
        args_len: usize,
        grants_ptr: *const u8,
        grants_len: usize,
        out_buf: *mut u8,
        out_cap: usize,
        out_len: *mut usize,
    ) -> u32,
}

fn write_bytes_to_out_buf(
    payload: &[u8],
    out_buf: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> u32 {
    if out_len.is_null() {
        return SdResult::Invalid as u32;
    }
    // SAFETY: Verified non-null caller output length pointer.
    unsafe {
        *out_len = payload.len();
    }
    if payload.len() > out_cap || out_buf.is_null() {
        return SdResult::BufferSmall as u32;
    }
    if !payload.is_empty() {
        // SAFETY: `out_buf` is non-null with capacity `out_cap >= payload.len()`.
        unsafe {
            std::ptr::copy_nonoverlapping(payload.as_ptr(), out_buf, payload.len());
        }
    }
    SdResult::Ok as u32
}

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

    // SAFETY: Caller passes valid UTF-8 byte ranges for `script_len`.
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

extern "C" fn host_list_mod_tools(
    out_buf: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> u32 {
    let tools = global_simoder_runtime().with_tool_registry_mut(|reg| reg.list_tools());
    let projected: Vec<serde_json::Value> = tools
        .into_iter()
        .map(|t| {
            serde_json::json!({
                "ownerModId": t.owner_mod_id,
                "ownerGeneration": t.owner_generation,
                "definition": {
                    "name": t.name,
                    "description": t.description,
                    "inputSchema": t.input_schema,
                    "annotations": t.annotations,
                },
                "requiredGrants": t.required_grants,
            })
        })
        .collect();
    let Ok(bytes) = serde_json::to_vec(&projected) else {
        return SdResult::OperationFailed as u32;
    };
    write_bytes_to_out_buf(&bytes, out_buf, out_cap, out_len)
}

extern "C" fn host_invoke_mod_tool(
    name_ptr: *const u8,
    name_len: usize,
    args_ptr: *const u8,
    args_len: usize,
    grants_ptr: *const u8,
    grants_len: usize,
    out_buf: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> u32 {
    if name_ptr.is_null()
        || name_len == 0
        || name_len > MAX_HOST_TOOL_NAME_BYTES
        || args_len > MAX_HOST_TOOL_ARGS_BYTES
        || (args_len > 0 && args_ptr.is_null())
        || grants_len > MAX_HOST_TOOL_GRANTS_BYTES
        || (grants_len > 0 && grants_ptr.is_null())
        || out_len.is_null()
    {
        return SdResult::Invalid as u32;
    }
    // SAFETY: Caller passes valid UTF-8 slice for `name_len`.
    let name_bytes = unsafe { std::slice::from_raw_parts(name_ptr, name_len) };
    let Ok(tool_name) = std::str::from_utf8(name_bytes) else {
        return SdResult::Invalid as u32;
    };
    let args_val = if args_len == 0 {
        serde_json::json!({})
    } else {
        // SAFETY: Caller passes valid UTF-8 slice for `args_len`.
        let args_bytes = unsafe { std::slice::from_raw_parts(args_ptr, args_len) };
        let Ok(parsed) = serde_json::from_slice::<serde_json::Value>(args_bytes) else {
            return SdResult::Invalid as u32;
        };
        parsed
    };
    let caller_grants: BTreeSet<String> = if grants_len == 0 {
        BTreeSet::new()
    } else {
        // SAFETY: Caller passes valid UTF-8 slice for `grants_len`.
        let grants_bytes = unsafe { std::slice::from_raw_parts(grants_ptr, grants_len) };
        let Ok(parsed) = serde_json::from_slice::<Vec<String>>(grants_bytes) else {
            return SdResult::Invalid as u32;
        };
        parsed.into_iter().collect()
    };

    // Resolve and authorize while holding the registry mutex, then release it BEFORE
    // executing the handler (which may block waiting for the game UI thread).
    let handler = match global_simoder_runtime()
        .with_tool_registry_mut(|reg| reg.resolve_tool_handler(tool_name, &caller_grants))
    {
        Ok(h) => h,
        Err(code) => return code as u32,
    };

    match (handler)(&args_val) {
        Ok(val) => {
            let Ok(bytes) = serde_json::to_vec(&val) else {
                return SdResult::OperationFailed as u32;
            };
            write_bytes_to_out_buf(&bytes, out_buf, out_cap, out_len)
        }
        Err(err_msg) => {
            let write_res = write_bytes_to_out_buf(err_msg.as_bytes(), out_buf, out_cap, out_len);
            if write_res == SdResult::BufferSmall as u32 {
                SdResult::BufferSmall as u32
            } else {
                SdResult::OperationFailed as u32
            }
        }
    }
}

pub(crate) static SEMANTIC_HOST_API_V2: SdSemanticHostApiV2Raw = SdSemanticHostApiV2Raw {
    size: size_of::<SdSemanticHostApiV2Raw>() as u32,
    version: 3,
    eval_ui_js: host_eval_ui_js,
    log_message: host_log_message,
    list_mod_tools: host_list_mod_tools,
    invoke_mod_tool: host_invoke_mod_tool,
};
