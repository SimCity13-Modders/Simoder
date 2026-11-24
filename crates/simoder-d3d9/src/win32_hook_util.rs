#![allow(unsafe_code)]

//! Shared audited Win32 memory snapshot and pointer-slot patching helpers for `simoder-d3d9`.
//!
//! Centralizes bounded `ReadProcessMemory` reads, loaded PE32 `.text`/`.rdata` snapshotting
//! into [`simoder_core::LoadedModuleView`], and atomic 4-byte `VirtualProtect` pointer swaps
//! so all native bridges and hooks share one audited FFI boundary.

use std::ffi::c_void;
use std::mem::{MaybeUninit, size_of};
use std::ptr::{null, write_volatile};

use simoder_core::LoadedModuleView;

use crate::config::MIN_VALID_USER_ADDRESS;
use crate::pe::parse_loaded_pe_text_and_rdata_ranges;
use crate::win32_loader::to_wide_null;

type HModule = *mut c_void;
type Handle = *mut c_void;

const PAGE_READWRITE: u32 = 0x04;
const PE_HEADER_PROBE_BYTES: usize = 4096;
const MAX_SECTION_READ_BYTES: usize = 32 * 1024 * 1024;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetModuleHandleW(lp_module_name: *const u16) -> HModule;
    fn GetCurrentProcess() -> Handle;
    fn ReadProcessMemory(
        h_process: Handle,
        lp_base_address: *const c_void,
        lp_buffer: *mut c_void,
        n_size: usize,
        lp_number_of_bytes_read: *mut usize,
    ) -> i32;
    fn VirtualProtect(
        lp_address: *mut c_void,
        dw_size: usize,
        fl_new_protect: u32,
        lpfl_old_protect: *mut u32,
    ) -> i32;
}

/// Owned in-memory snapshot of a loaded PE32 module's `.text` and `.rdata` sections.
pub(crate) struct OwnedModuleSnapshot {
    pub(crate) image_base: u32,
    pub(crate) text_rva: u32,
    pub(crate) text: Vec<u8>,
    pub(crate) rdata_rva: u32,
    pub(crate) rdata: Vec<u8>,
}

impl OwnedModuleSnapshot {
    /// Borrow this snapshot as a [`simoder_core::LoadedModuleView`] for symbol resolution.
    pub(crate) fn as_view(&self) -> LoadedModuleView<'_> {
        LoadedModuleView {
            image_base: self.image_base,
            text_rva: self.text_rva,
            text: &self.text,
            rdata_rva: self.rdata_rva,
            rdata: &self.rdata,
        }
    }
}

/// Snapshot the `.text` and `.rdata` sections of a loaded 32-bit PE module (`None` = host `.exe`).
pub(crate) fn snapshot_loaded_module(
    module_name: Option<&str>,
) -> Result<OwnedModuleSnapshot, String> {
    let label = module_name.unwrap_or("main executable");
    // SAFETY: Queries loaded module handle via `GetModuleHandleW` (null for host `.exe`).
    let module = unsafe {
        match module_name {
            Some(name) => {
                let wide = to_wide_null(std::ffi::OsStr::new(name));
                GetModuleHandleW(wide.as_ptr())
            }
            None => GetModuleHandleW(null()),
        }
    };
    if module.is_null() {
        return Err(format!("{label} not loaded yet"));
    }
    let image_base = u32::try_from(module as usize)
        .map_err(|_| format!("{label} base address exceeds 32-bit range"))?;

    let mut header_buf = [0u8; PE_HEADER_PROBE_BYTES];
    if !read_bytes_exact(image_base as usize, &mut header_buf) {
        return Err(format!("failed to read {label} PE header"));
    }
    let ((text_rva, text_size), (rdata_rva, rdata_size)) =
        parse_loaded_pe_text_and_rdata_ranges(&header_buf)
            .ok_or_else(|| format!("failed to parse {label} .text/.rdata sections"))?;

    let text_len = usize::try_from(text_size)
        .ok()
        .filter(|&n| n > 0 && n <= MAX_SECTION_READ_BYTES)
        .ok_or_else(|| format!("{label} .text section size out of bounds"))?;
    let rdata_len = usize::try_from(rdata_size)
        .ok()
        .filter(|&n| n > 0 && n <= MAX_SECTION_READ_BYTES)
        .ok_or_else(|| format!("{label} .rdata section size out of bounds"))?;

    let text_va = image_base
        .checked_add(text_rva)
        .ok_or_else(|| format!("{label} .text VA overflow"))?;
    let rdata_va = image_base
        .checked_add(rdata_rva)
        .ok_or_else(|| format!("{label} .rdata VA overflow"))?;

    let mut text = vec![0u8; text_len];
    let mut rdata = vec![0u8; rdata_len];
    if !read_bytes_exact(text_va as usize, &mut text)
        || !read_bytes_exact(rdata_va as usize, &mut rdata)
    {
        return Err(format!("failed to read {label} .text/.rdata memory"));
    }

    Ok(OwnedModuleSnapshot {
        image_base,
        text_rva,
        text,
        rdata_rva,
        rdata,
    })
}

/// Safely read `out.len()` bytes from `addr` in the current process via `ReadProcessMemory`.
pub(crate) fn read_bytes_exact(addr: usize, out: &mut [u8]) -> bool {
    if addr < MIN_VALID_USER_ADDRESS as usize || out.is_empty() {
        return false;
    }
    let mut read_len = 0usize;
    // SAFETY: `GetCurrentProcess()` pseudo-handle with bounds-checked destination slice.
    let ok = unsafe {
        ReadProcessMemory(
            GetCurrentProcess(),
            addr as *const c_void,
            out.as_mut_ptr() as *mut c_void,
            out.len(),
            &mut read_len,
        )
    };
    ok != 0 && read_len == out.len()
}

/// Safely read a `Copy` value of type `T` from `addr` via `ReadProcessMemory`.
pub(crate) fn read_struct<T: Copy>(addr: usize) -> Option<T> {
    if addr < MIN_VALID_USER_ADDRESS as usize {
        return None;
    }
    let mut value = MaybeUninit::<T>::uninit();
    let mut read_len = 0usize;
    // SAFETY: `ReadProcessMemory` writes at most `size_of::<T>()` bytes into `value`.
    let ok = unsafe {
        ReadProcessMemory(
            GetCurrentProcess(),
            addr as *const c_void,
            value.as_mut_ptr() as *mut c_void,
            size_of::<T>(),
            &mut read_len,
        )
    };
    if ok != 0 && read_len == size_of::<T>() {
        // SAFETY: Exact byte count was initialized by `ReadProcessMemory`.
        Some(unsafe { value.assume_init() })
    } else {
        None
    }
}

/// Safely read a bounded NUL-terminated ANSI/UTF-8 C string from `addr` via `ReadProcessMemory`.
pub(crate) fn read_remote_cstr(addr: usize, max_bytes: usize) -> Option<String> {
    if addr < MIN_VALID_USER_ADDRESS as usize || max_bytes == 0 {
        return None;
    }
    let mut buf = Vec::with_capacity(128);
    // SAFETY: `GetCurrentProcess()` returns the current process pseudo-handle.
    let proc_handle = unsafe { GetCurrentProcess() };
    for offset in 0..max_bytes {
        let mut byte = 0u8;
        let mut read_len = 0usize;
        // SAFETY: Reads one byte at a time from `addr + offset` via `ReadProcessMemory`.
        let ok = unsafe {
            ReadProcessMemory(
                proc_handle,
                addr.checked_add(offset)? as *const c_void,
                (&mut byte as *mut u8) as *mut c_void,
                1,
                &mut read_len,
            )
        };
        if ok == 0 || read_len != 1 {
            return None;
        }
        if byte == 0 {
            break;
        }
        buf.push(byte);
    }
    Some(String::from_utf8_lossy(&buf).into_owned())
}

/// Atomically swap a pointer-sized slot (`vtable` or `IAT`) with `VirtualProtect` and restore protection.
pub(crate) fn patch_pointer_slot(slot_addr: usize, value: usize) -> bool {
    if slot_addr < MIN_VALID_USER_ADDRESS as usize {
        return false;
    }
    let mut old_protect: u32 = 0;
    // SAFETY: Temporarily marks the pointer slot writable, updates it, and restores protection.
    unsafe {
        if VirtualProtect(
            slot_addr as *mut c_void,
            size_of::<usize>(),
            PAGE_READWRITE,
            &mut old_protect,
        ) == 0
        {
            return false;
        }
        write_volatile(slot_addr as *mut usize, value);
        let mut unused: u32 = 0;
        let _ = VirtualProtect(
            slot_addr as *mut c_void,
            size_of::<usize>(),
            old_protect,
            &mut unused,
        );
        true
    }
}
