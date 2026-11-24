#![allow(unsafe_code)]

//! Audited Win32 IAT hook for `OutputDebugStringA` and `OutputDebugStringW` (`docs/logging.md`).
//!
//! Captures game debug output emitted through `KERNEL32` (`SimCity.exe` and `EAWebKit.dll`)
//! into [`simoder_core::DiagnosticLogPipeline::push_output_debug_string`] while preserving
//! `GetLastError()`, calling the original Win32 API first, bounding reads to `4096` bytes via
//! checked `ReadProcessMemory`, and relying on `simoder-core`'s thread-local recursion guard.

use std::ffi::c_void;
use std::mem::size_of;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use parking_lot::Mutex;
use simoder_core::DiagnosticLogPipeline;

use crate::win32_console::{current_thread_id, unix_time_ms};
use crate::win32_hook_util::{patch_pointer_slot, read_struct};

type HModule = *mut c_void;
type Handle = *mut c_void;

const IMAGE_DIRECTORY_ENTRY_IMPORT: usize = 1;
const MAX_ODS_BYTES: usize = 4096;
const MAX_ODS_WCHARS: usize = MAX_ODS_BYTES / 2;

#[cfg(target_pointer_width = "32")]
const ORDINAL_FLAG: usize = 0x8000_0000;
#[cfg(target_pointer_width = "64")]
const ORDINAL_FLAG: usize = 0x8000_0000_0000_0000;

#[repr(C)]
#[derive(Clone, Copy)]
struct ImageImportDescriptor {
    original_first_thunk: u32,
    time_date_stamp: u32,
    forwarder_chain: u32,
    name: u32,
    first_thunk: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ImageDataDirectory {
    virtual_address: u32,
    size: u32,
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetModuleHandleW(lp_module_name: *const u16) -> HModule;
    fn GetProcAddress(h_module: HModule, lp_proc_name: *const i8) -> *const c_void;
    fn GetCurrentProcess() -> Handle;
    fn ReadProcessMemory(
        h_process: Handle,
        lp_base_address: *const c_void,
        lp_buffer: *mut c_void,
        n_size: usize,
        lp_number_of_bytes_read: *mut usize,
    ) -> i32;
    fn GetLastError() -> u32;
    fn SetLastError(dw_err_code: u32);
}

static ODS_PIPELINE: Mutex<Option<Arc<DiagnosticLogPipeline>>> = Mutex::new(None);
static PATCHED_SLOTS: Mutex<Vec<(usize, usize)>> = Mutex::new(Vec::new());
static ORIG_ODS_A: AtomicUsize = AtomicUsize::new(0);
static ORIG_ODS_W: AtomicUsize = AtomicUsize::new(0);

type FnOdsA = unsafe extern "system" fn(*const u8);
type FnOdsW = unsafe extern "system" fn(*const u16);

unsafe extern "system" fn hook_ods_a(ptr: *const u8) {
    // SAFETY: Preserves caller `GetLastError()`, invokes original `OutputDebugStringA` first,
    // and reads at most `MAX_ODS_BYTES` via checked `ReadProcessMemory` before forwarding.
    unsafe {
        let saved_err = GetLastError();
        let orig_addr = ORIG_ODS_A.load(Ordering::Acquire);
        if orig_addr != 0 {
            let orig: FnOdsA = std::mem::transmute(orig_addr);
            orig(ptr);
        }
        if !ptr.is_null()
            && let Some(text) = read_remote_ansi_cstr(ptr)
            && let Some(pipeline) = ODS_PIPELINE.lock().clone()
        {
            let _ = pipeline.capture_output_debug_string(
                unix_time_ms(),
                false,
                current_thread_id(),
                &text,
            );
        }
        SetLastError(saved_err);
    }
}

unsafe extern "system" fn hook_ods_w(ptr: *const u16) {
    // SAFETY: Preserves caller `GetLastError()`, invokes original `OutputDebugStringW` first,
    // and reads at most `MAX_ODS_WCHARS` UTF-16 code units via checked `ReadProcessMemory`.
    unsafe {
        let saved_err = GetLastError();
        let orig_addr = ORIG_ODS_W.load(Ordering::Acquire);
        if orig_addr != 0 {
            let orig: FnOdsW = std::mem::transmute(orig_addr);
            orig(ptr);
        }
        if !ptr.is_null()
            && let Some(text) = read_remote_wide_cstr(ptr)
            && let Some(pipeline) = ODS_PIPELINE.lock().clone()
        {
            let _ = pipeline.capture_output_debug_string(
                unix_time_ms(),
                true,
                current_thread_id(),
                &text,
            );
        }
        SetLastError(saved_err);
    }
}

fn read_checked_bytes(ptr: *const c_void, max_bytes: usize) -> Vec<u8> {
    if ptr.is_null() || max_bytes == 0 {
        return Vec::new();
    }
    let mut out = vec![0u8; max_bytes];
    // SAFETY: `GetCurrentProcess()` is a pseudo-handle; `ReadProcessMemory` validates readability
    // without faulting on unmapped pages.
    let proc_handle = unsafe { GetCurrentProcess() };
    for attempt_len in [max_bytes, 512, 128, 32] {
        let len = attempt_len.min(max_bytes);
        let mut read: usize = 0;
        // SAFETY: `proc_handle` is `GetCurrentProcess()` and `out` has capacity `max_bytes >= len`.
        let ok = unsafe {
            ReadProcessMemory(
                proc_handle,
                ptr,
                out.as_mut_ptr() as *mut c_void,
                len,
                &mut read,
            )
        };
        if ok != 0 && read > 0 {
            out.truncate(read);
            return out;
        }
    }
    Vec::new()
}

fn read_remote_ansi_cstr(ptr: *const u8) -> Option<String> {
    let bytes = read_checked_bytes(ptr as *const c_void, MAX_ODS_BYTES);
    if bytes.is_empty() {
        return None;
    }
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    Some(String::from_utf8_lossy(&bytes[..end]).into_owned())
}

fn read_remote_wide_cstr(ptr: *const u16) -> Option<String> {
    let bytes = read_checked_bytes(ptr as *const c_void, MAX_ODS_WCHARS * 2);
    if bytes.len() < 2 {
        return None;
    }
    let mut units = Vec::with_capacity(bytes.len() / 2);
    let (chunks, _) = bytes.as_chunks::<2>();
    for chunk in chunks {
        let u = u16::from_le_bytes(*chunk);
        if u == 0 {
            break;
        }
        units.push(u);
    }
    Some(String::from_utf16_lossy(&units))
}

unsafe fn read_cstr_at(base: usize, rva: u32) -> Option<String> {
    if rva == 0 {
        return None;
    }
    let ptr = (base + rva as usize) as *const u8;
    let mut bytes = Vec::with_capacity(64);
    for i in 0..256 {
        // SAFETY: Caller verified `rva` lies within a loaded PE module image.
        let b = unsafe { *ptr.add(i) };
        if b == 0 {
            break;
        }
        bytes.push(b);
    }
    Some(String::from_utf8_lossy(&bytes).into_owned())
}

fn is_debug_import_dll(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    lower == "kernel32.dll"
        || lower == "kernelbase.dll"
        || lower.starts_with("api-ms-win-core-debug")
}

fn patch_iat_slot(slot_addr: usize, new_fn: usize, orig_store: &AtomicUsize) -> bool {
    let Some(orig_val) = read_struct::<usize>(slot_addr) else {
        return false;
    };
    let _ = orig_store.compare_exchange(0, orig_val, Ordering::AcqRel, Ordering::Acquire);
    if !patch_pointer_slot(slot_addr, new_fn) {
        return false;
    }
    PATCHED_SLOTS.lock().push((slot_addr, orig_val));
    true
}

unsafe fn hook_module_iat(module: HModule) -> usize {
    if module.is_null() {
        return 0;
    }
    let base = module as usize;
    // SAFETY: `module` is a live loaded PE base handle returned by `GetModuleHandleW`.
    unsafe {
        if *(base as *const u16) != 0x5A4D {
            return 0;
        }
        let e_lfanew = *((base + 0x3C) as *const u32) as usize;
        let nt_headers = base + e_lfanew;
        if *(nt_headers as *const u32) != 0x0000_4550 {
            return 0;
        }
        let opt_header = nt_headers + 24;
        let magic = *(opt_header as *const u16);
        let data_dir_offset = match magic {
            0x010B => 96,  // PE32
            0x020B => 112, // PE32+
            _ => return 0,
        };
        let import_dir =
            *((opt_header + data_dir_offset + IMAGE_DIRECTORY_ENTRY_IMPORT * 8)
                as *const ImageDataDirectory);
        if import_dir.virtual_address == 0 {
            return 0;
        }

        let mut patched = 0usize;
        let mut desc_ptr = (base + import_dir.virtual_address as usize) as *const ImageImportDescriptor;
        loop {
            let desc = *desc_ptr;
            if desc.original_first_thunk == 0 && desc.first_thunk == 0 {
                break;
            }
            if let Some(dll_name) = read_cstr_at(base, desc.name)
                && is_debug_import_dll(&dll_name)
            {
                let oft_rva = if desc.original_first_thunk != 0 {
                    desc.original_first_thunk
                } else {
                    desc.first_thunk
                };
                let mut idx = 0usize;
                loop {
                    let oft_entry = *((base + oft_rva as usize + idx * size_of::<usize>()) as *const usize);
                    if oft_entry == 0 {
                        break;
                    }
                    if (oft_entry & ORDINAL_FLAG) == 0
                        && let Some(fn_name) = read_cstr_at(base, (oft_entry as u32).saturating_add(2))
                    {
                        let slot_addr = base + desc.first_thunk as usize + idx * size_of::<usize>();
                        let matched = (fn_name == "OutputDebugStringA"
                            && patch_iat_slot(
                                slot_addr,
                                hook_ods_a as *const () as usize,
                                &ORIG_ODS_A,
                            ))
                            || (fn_name == "OutputDebugStringW"
                                && patch_iat_slot(
                                    slot_addr,
                                    hook_ods_w as *const () as usize,
                                    &ORIG_ODS_W,
                                ));
                        if matched {
                            patched += 1;
                        }
                    }
                    idx += 1;
                }
            }
            desc_ptr = desc_ptr.add(1);
        }
        patched
    }
}

fn resolve_ods_targets() -> (Vec<usize>, Vec<usize>) {
    let mut a_addrs = Vec::new();
    let mut w_addrs = Vec::new();
    for dll in ["kernel32.dll\0", "kernelbase.dll\0"] {
        let wide: Vec<u16> = dll.encode_utf16().collect();
        // SAFETY: `wide` is NUL-terminated UTF-16; `"OutputDebugStringA\0"` / `"OutputDebugStringW\0"` are NUL-terminated ASCII.
        unsafe {
            let hmod = GetModuleHandleW(wide.as_ptr());
            if !hmod.is_null() {
                let pa = GetProcAddress(hmod, c"OutputDebugStringA".as_ptr()) as usize;
                if pa != 0 && !a_addrs.contains(&pa) {
                    a_addrs.push(pa);
                }
                let pw = GetProcAddress(hmod, c"OutputDebugStringW".as_ptr()) as usize;
                if pw != 0 && !w_addrs.contains(&pw) {
                    w_addrs.push(pw);
                }
            }
        }
    }
    (a_addrs, w_addrs)
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ImageSectionHeader {
    name: [u8; 8],
    virtual_size: u32,
    virtual_address: u32,
    size_of_raw_data: u32,
    pointer_to_raw_data: u32,
    pointer_to_relocations: u32,
    pointer_to_linenumbers: u32,
    number_of_relocations: u16,
    number_of_linenumbers: u16,
    characteristics: u32,
}

unsafe fn hook_unpacked_iat_pointers(
    module: HModule,
    targets_a: &[usize],
    targets_w: &[usize],
) -> usize {
    if module.is_null() || (targets_a.is_empty() && targets_w.is_empty()) {
        return 0;
    }
    let base = module as usize;
    // SAFETY: `module` is a live loaded PE base handle; section headers and `.rdata`/`.idata`
    // ranges are validated within the PE header before scanning aligned `usize` slots.
    unsafe {
        if *(base as *const u16) != 0x5A4D {
            return 0;
        }
        let e_lfanew = *((base + 0x3C) as *const u32) as usize;
        let nt_headers = base + e_lfanew;
        if *(nt_headers as *const u32) != 0x0000_4550 {
            return 0;
        }
        let num_sections = *((nt_headers + 6) as *const u16) as usize;
        let opt_size = *((nt_headers + 20) as *const u16) as usize;
        let sec_ptr = (nt_headers + 24 + opt_size) as *const ImageSectionHeader;
        let mut patched = 0usize;
        for i in 0..num_sections {
            let sec = *sec_ptr.add(i);
            let name_end = sec.name.iter().position(|&b| b == 0).unwrap_or(8);
            let sec_name = std::str::from_utf8(&sec.name[..name_end]).unwrap_or("");
            if sec_name != ".rdata" && sec_name != ".idata" {
                continue;
            }
            let start = base + sec.virtual_address as usize;
            let len = (sec.virtual_size as usize).max(sec.size_of_raw_data as usize);
            let step = size_of::<usize>();
            let mut off = 0usize;
            while off + step <= len {
                let slot_addr = start + off;
                let val = *(slot_addr as *const usize);
                let matched = (targets_a.contains(&val)
                    && patch_iat_slot(slot_addr, hook_ods_a as *const () as usize, &ORIG_ODS_A))
                    || (targets_w.contains(&val)
                        && patch_iat_slot(
                            slot_addr,
                            hook_ods_w as *const () as usize,
                            &ORIG_ODS_W,
                        ));
                if matched {
                    patched += 1;
                }
                off += step;
            }
        }
        patched
    }
}

/// Install `OutputDebugStringA`/`OutputDebugStringW` IAT hooks on the main executable and
/// `EAWebKit.dll` (when loaded), returning the number of patched IAT slots.
pub fn install_output_debug_string_hooks(pipeline: Arc<DiagnosticLogPipeline>) -> usize {
    *ODS_PIPELINE.lock() = Some(pipeline);
    let mut total = 0usize;
    // SAFETY: `GetModuleHandleW(null)` returns the host executable base; `EAWebKit.dll` is
    // queried read-only by NUL-terminated UTF-16 name.
    unsafe {
        let exe_mod = GetModuleHandleW(std::ptr::null());
        let exe_iat = hook_module_iat(exe_mod);
        if exe_iat > 0 {
            total += exe_iat;
        } else {
            let (targets_a, targets_w) = resolve_ods_targets();
            total += hook_unpacked_iat_pointers(exe_mod, &targets_a, &targets_w);
        }
        let eawebkit: Vec<u16> = "EAWebKit.dll\0".encode_utf16().collect();
        total += hook_module_iat(GetModuleHandleW(eawebkit.as_ptr()));
    }
    total
}

/// Restore all patched `OutputDebugStringA`/`OutputDebugStringW` IAT slots and clear the pipeline.
pub fn uninstall_output_debug_string_hooks() {
    let slots = std::mem::take(&mut *PATCHED_SLOTS.lock());
    for (slot_addr, orig_val) in slots {
        let _ = patch_pointer_slot(slot_addr, orig_val);
    }
    *ODS_PIPELINE.lock() = None;
}
