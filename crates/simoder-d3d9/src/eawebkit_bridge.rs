#![allow(unsafe_code)]

//! Verified in-process `EAWebKit` JS execution bridge (`SimCity.exe 10.3.6.0`, `i686-pc-windows-msvc`).
//!
//! Matches exact executable SHA-256 + 5 immutable entry signatures, validates page readability via
//! `VirtualQuery`, verifies owning-thread affinity against `0x00E596F8`, allocates RAII 12-byte
//! `ValueScope` wrappers, and compiles/invokes ES5 scripts safely.

#[cfg(target_pointer_width = "32")]
use std::ffi::{CString, c_void};
#[cfg(target_pointer_width = "32")]
use std::mem::{size_of, transmute, zeroed};
#[cfg(target_pointer_width = "32")]
use std::ptr::null_mut;
use std::sync::atomic::{AtomicUsize, Ordering};

static VERIFIED_BASE: AtomicUsize = AtomicUsize::new(0);

#[cfg(target_pointer_width = "32")]
const PREFERRED_BASE: usize = 0x0040_0000;
#[cfg(target_pointer_width = "32")]
const NATIVE_THREAD_SLOT_VA: usize = 0x00E5_96F8;
#[cfg(target_pointer_width = "32")]
const VIEW_ROOT_SLOT_VA: usize = 0x00FE_13C4;
#[cfg(target_pointer_width = "32")]
const VA_VALUE_SCOPE_CTOR: usize = 0x00A3_85C0;
#[cfg(target_pointer_width = "32")]
const VA_VALUE_SCOPE_DTOR: usize = 0x00A3_8500;
#[cfg(target_pointer_width = "32")]
const VA_TEXT_ASSIGN: usize = 0x0066_6430;
#[cfg(target_pointer_width = "32")]
const VA_VIEW_COUNT: usize = 0x0093_5F10;
#[cfg(target_pointer_width = "32")]
const VA_CONTEXT_AT: usize = 0x00A3_BB80;
#[cfg(target_pointer_width = "32")]
const VA_COMPILE: usize = 0x00A3_6220;
#[cfg(target_pointer_width = "32")]
const VA_INVOKE: usize = 0x00A3_50A0;

#[cfg(target_pointer_width = "32")]
const MEM_COMMIT: u32 = 0x1000;
#[cfg(target_pointer_width = "32")]
const PAGE_NOACCESS: u32 = 0x01;
#[cfg(target_pointer_width = "32")]
const PAGE_GUARD: u32 = 0x100;

#[cfg(target_pointer_width = "32")]
#[repr(C)]
struct MemoryBasicInformation {
    base_address: usize,
    allocation_base: usize,
    allocation_protect: u32,
    region_size: usize,
    state: u32,
    protect: u32,
    type_: u32,
}

#[cfg(target_pointer_width = "32")]
const _: () = assert!(size_of::<MemoryBasicInformation>() == 28);

#[cfg(target_pointer_width = "32")]
#[repr(C)]
struct ValueScopeStorage {
    value: *mut c_void,
    pool: *mut c_void,
    owned: u8,
    padding: [u8; 3],
}

#[cfg(target_pointer_width = "32")]
const _: () = assert!(size_of::<ValueScopeStorage>() == 12);

#[cfg(target_pointer_width = "32")]
#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetModuleHandleW(lp_module_name: *const u16) -> *mut c_void;
    fn GetCurrentThreadId() -> u32;
    fn GetThreadId(h_thread: *mut c_void) -> u32;
    fn VirtualQuery(
        lp_address: *const c_void,
        lp_buffer: *mut MemoryBasicInformation,
        dw_length: usize,
    ) -> usize;
}

#[cfg(target_pointer_width = "32")]
#[link(name = "user32")]
unsafe extern "system" {
    fn GetWindowThreadProcessId(hwnd: *mut c_void, lpdw_process_id: *mut u32) -> u32;
}

/// Verify retail `SimCity.exe 10.3.6.0` base address and all 5 immutable `EAWebKit` entry signatures.
pub fn initialize_eawebkit_bridge(verified_retail_file: bool) -> Result<(), String> {
    VERIFIED_BASE.store(0, Ordering::Release);
    if !verified_retail_file {
        return Err("EAWebKit bridge requires verified SimCity.exe 10.3.6.0 SHA-256".into());
    }
    #[cfg(not(target_pointer_width = "32"))]
    {
        Err("EAWebKit native bridge requires 32-bit x86 host process".into())
    }
    #[cfg(target_pointer_width = "32")]
    {
        // SAFETY: Reads main module handle and checks 5 immutable opcode sequences via `VirtualQuery`.
        let base = unsafe { GetModuleHandleW(std::ptr::null()) } as usize;
        if base != PREFERRED_BASE {
            return Err(format!(
                "EAWebKit bridge requires preferred load base 0x{PREFERRED_BASE:08X}, got 0x{base:08X}"
            ));
        }
        let checks: &[(usize, [u8; 8])] = &[
            (VA_VALUE_SCOPE_CTOR, [0x8b, 0x44, 0x24, 0x04, 0x55, 0x56, 0x8b, 0xf1]),
            (VA_VALUE_SCOPE_DTOR, [0x80, 0x79, 0x08, 0x00, 0x74, 0x0f, 0x8b, 0x01]),
            (VA_TEXT_ASSIGN, [0x6a, 0xff, 0x68, 0x88, 0x9b, 0xc4, 0x00, 0x64]),
            (VA_COMPILE, [0x56, 0x8b, 0xf1, 0x83, 0xbe, 0xb0, 0x00, 0x00]),
            (VA_INVOKE, [0x53, 0x8b, 0xd9, 0x8b, 0x4c, 0x24, 0x08, 0x85]),
        ];
        for &(va, expected) in checks {
            let ptr = base + va - PREFERRED_BASE;
            if !is_readable(ptr, expected.len()) {
                return Err(format!("EAWebKit entry signature at 0x{va:08X} unreadable"));
            }
            // SAFETY: Verified readable by `is_readable(ptr, 8)`.
            let actual = unsafe { std::slice::from_raw_parts(ptr as *const u8, expected.len()) };
            if actual != expected {
                return Err(format!("EAWebKit entry signature mismatch at 0x{va:08X}"));
            }
        }
        VERIFIED_BASE.store(base, Ordering::Release);
        Ok(())
    }
}

/// Return true after [`initialize_eawebkit_bridge`] succeeded on the verified game build.
pub fn is_eawebkit_bridge_ready() -> bool {
    VERIFIED_BASE.load(Ordering::Acquire) != 0
}

/// Execute `script_source` (pure ES5 JS function `(packetText) -> string`) with `packet_json`
/// inside the highest-ranked active `EAWebKit` view context on the verified owning UI thread.
///
/// The script itself must support `UI_OBSERVE_PACKET_JSON` (`{"op":"observe"}`) for view selection.
pub fn execute_eawebkit_packet(
    hwnd_val: usize,
    script_source: &str,
    packet_json: &str,
) -> Result<String, String> {
    if packet_json.is_empty() {
        return Err("UI packet size out of bounds".into());
    }
    eval_active_view_inner(hwnd_val, None, script_source, packet_json)
}

/// Evaluate an arbitrary strict-ES5 JS function `(argText) -> string` inside the highest-ranked
/// active `EAWebKit` view (`window.Client && window.simcity`) on the verified owning UI thread.
pub fn eval_active_view_script(
    hwnd_val: usize,
    script_fn: &str,
    arg_json: &str,
) -> Result<String, String> {
    eval_active_view_inner(hwnd_val, Some(VIEW_PROBE_ES5), script_fn, arg_json)
}

#[cfg(target_pointer_width = "32")]
const VIEW_PROBE_ES5: &str = "(function(){try{var g=window.simcity,c=window.Client;var ok=!!(g&&c);var r=0;if(ok){if(g.gGlobalUIHandler)r=4;else if(g.cLauncher3D)r=3;else if(g.gUpdater)r=2;else r=1;}return JSON.stringify({ok:ok,rank:r});}catch(e){return '{\"ok\":false,\"rank\":0}';}})";
#[cfg(not(target_pointer_width = "32"))]
const VIEW_PROBE_ES5: &str = "";

fn eval_active_view_inner(
    hwnd_val: usize,
    probe_script: Option<&str>,
    script_source: &str,
    arg_text: &str,
) -> Result<String, String> {
    let base = VERIFIED_BASE.load(Ordering::Acquire);
    if base == 0 {
        return Err("EAWebKit bridge is not initialized".into());
    }
    #[cfg(not(target_pointer_width = "32"))]
    {
        let _ = (hwnd_val, probe_script, script_source, arg_text);
        Err("EAWebKit bridge requires 32-bit x86 host process".into())
    }
    #[cfg(target_pointer_width = "32")]
    {
        execute_packet_x86(base, hwnd_val, probe_script, script_source, arg_text)
    }
}

#[cfg(target_pointer_width = "32")]
fn is_readable(address: usize, len: usize) -> bool {
    if address == 0 {
        return false;
    }
    let Some(end) = address.checked_add(len) else {
        return false;
    };
    let mut cursor = address;
    while cursor < end {
        // SAFETY: Valid `VirtualQuery` inspection without dereferencing caller memory.
        let mut mbi: MemoryBasicInformation = unsafe { zeroed() };
        // SAFETY: Valid output buffer.
        let n = unsafe {
            VirtualQuery(
                cursor as *const c_void,
                &mut mbi,
                size_of::<MemoryBasicInformation>(),
            )
        };
        if n == 0 || mbi.state != MEM_COMMIT || (mbi.protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 {
            return false;
        }
        let next = mbi.base_address.saturating_add(mbi.region_size);
        if next <= cursor {
            return false;
        }
        cursor = next;
    }
    true
}

#[cfg(target_pointer_width = "32")]
fn vtable_method(base: usize, obj: *mut c_void, offset: usize, expected_va: usize) -> Result<usize, String> {
    let obj_addr = obj as usize;
    if !is_readable(obj_addr, size_of::<usize>()) {
        return Err("Native UI object unreadable".into());
    }
    // SAFETY: Verified readable object pointer.
    let vtable = unsafe { *(obj_addr as *const usize) };
    let slot = vtable.saturating_add(offset);
    if !is_readable(slot, size_of::<usize>()) {
        return Err("Native UI vtable unreadable".into());
    }
    // SAFETY: Verified readable vtable entry.
    let target = unsafe { *(slot as *const usize) };
    if expected_va != 0 && target != base + expected_va - PREFERRED_BASE {
        return Err(format!("Native vtable mismatch at offset 0x{offset:X}"));
    }
    Ok(target)
}

#[cfg(target_pointer_width = "32")]
struct ValueScope {
    storage: ValueScopeStorage,
    base: usize,
}

#[cfg(target_pointer_width = "32")]
impl ValueScope {
    fn new(base: usize, context: *mut c_void) -> Self {
        type FnCtor = unsafe extern "thiscall" fn(*mut ValueScopeStorage, *mut c_void);
        let mut storage = ValueScopeStorage {
            value: null_mut(),
            pool: null_mut(),
            owned: 0,
            padding: [0; 3],
        };
        // SAFETY: Verified `ValueScope` constructor signature (`0x00A385C0`).
        unsafe {
            let ctor: FnCtor = transmute(base + VA_VALUE_SCOPE_CTOR - PREFERRED_BASE);
            ctor(&mut storage, context);
        }
        Self { storage, base }
    }
}

#[cfg(target_pointer_width = "32")]
impl Drop for ValueScope {
    fn drop(&mut self) {
        type FnDtor = unsafe extern "thiscall" fn(*mut ValueScopeStorage);
        // SAFETY: Verified `ValueScope` destructor (`0x00A38500`).
        unsafe {
            let dtor: FnDtor = transmute(self.base + VA_VALUE_SCOPE_DTOR - PREFERRED_BASE);
            dtor(&mut self.storage);
        }
    }
}

#[cfg(target_pointer_width = "32")]
fn copy_value_utf8(base: usize, value: *mut c_void) -> Result<String, String> {
    if value.is_null() {
        return Err("EAWebKit returned null JSValue".into());
    }
    type FnType = unsafe extern "thiscall" fn(*mut c_void) -> i32;
    type FnString = unsafe extern "thiscall" fn(*mut c_void, *mut u32) -> *const u16;
    let type_addr = vtable_method(base, value, 4, 0)?;
    // SAFETY: Verified `JSValue::Type()` slot.
    let val_type = unsafe {
        let f: FnType = transmute(type_addr);
        f(value)
    };
    if val_type != 4 {
        return Err(format!("EAWebKit UI script returned non-string type {val_type}"));
    }
    let str_addr = vtable_method(base, value, 0x38, 0)?;
    let mut len: u32 = 0;
    // SAFETY: Verified `JSValue::GetString()` slot.
    let ptr = unsafe {
        let f: FnString = transmute(str_addr);
        f(value, &mut len)
    };
    if len == 0 {
        return Ok(String::new());
    }
    if len > 1_048_576 || !is_readable(ptr as usize, (len as usize) * 2) {
        return Err("Native UI JSON result empty or exceeds 1 MiB bound".into());
    }
    // SAFETY: Verified readable UTF-16 slice within bounds.
    let slice = unsafe { std::slice::from_raw_parts(ptr, len as usize) };
    String::from_utf16(slice).map_err(|_| "Invalid UTF-16 returned by EAWebKit UI script".into())
}

#[cfg(target_pointer_width = "32")]
fn execute_packet_x86(
    base: usize,
    hwnd_val: usize,
    probe_script: Option<&str>,
    script_source: &str,
    packet_json: &str,
) -> Result<String, String> {
    if packet_json.len() > 131_072 {
        return Err("UI packet size out of bounds".into());
    }
    let script_c = CString::new(script_source).map_err(|_| "script contains NUL".to_string())?;
    let packet_c = CString::new(packet_json).map_err(|_| "packet contains NUL".to_string())?;
    let probe_source = probe_script.unwrap_or(script_source);
    let probe_c = CString::new(probe_source).map_err(|_| "probe script contains NUL".to_string())?;
    let observe_c = CString::new(crate::ui_patch_host::UI_OBSERVE_PACKET_JSON)
        .map_err(|_| "observe packet NUL".to_string())?;

    // SAFETY: Verify owning thread affinity against `0x00E596F8` and `hwnd_val`.
    let (current_tid, native_tid, window_tid) = unsafe {
        let cur = GetCurrentThreadId();
        let thread_slot = (base + NATIVE_THREAD_SLOT_VA - PREFERRED_BASE) as *const *mut c_void;
        let nat = if is_readable(thread_slot as usize, 4) && !(*thread_slot).is_null() {
            GetThreadId(*thread_slot)
        } else {
            0
        };
        let win = GetWindowThreadProcessId(hwnd_val as *mut c_void, null_mut());
        (cur, nat, win)
    };
    if native_tid == 0 || native_tid != current_tid || window_tid != current_tid {
        return Err(format!(
            "Thread affinity mismatch: cur={current_tid}, win={window_tid}, native={native_tid}"
        ));
    }

    let root_slot = base + VIEW_ROOT_SLOT_VA - PREFERRED_BASE;
    if !is_readable(root_slot, 4) {
        return Err("UI root slot unreadable".into());
    }
    // SAFETY: Verified readable pointer slot.
    let root = unsafe { *(root_slot as *const *mut c_void) };
    if root.is_null() {
        return Err("Native UI root not ready".into());
    }

    type FnCount = unsafe extern "thiscall" fn(*mut c_void) -> i32;
    type FnContext = unsafe extern "thiscall" fn(*mut c_void, i32) -> *mut c_void;
    type FnCompile = unsafe extern "thiscall" fn(*mut c_void, *const i8, *mut ValueScopeStorage) -> bool;
    type FnInvoke = unsafe extern "thiscall" fn(*mut c_void, *mut ValueScopeStorage, *mut ValueScopeStorage, *mut ValueScopeStorage) -> bool;
    type FnText = unsafe extern "C" fn(*const i8, *mut ValueScopeStorage);

    let count_addr = vtable_method(base, root, 0x54, VA_VIEW_COUNT)?;
    // SAFETY: Verified `GetViewCount`.
    let count = unsafe {
        let f: FnCount = transmute(count_addr);
        f(root)
    };
    if count <= 0 || count > 16 {
        return Err(format!("Native UI view count {count} out of range"));
    }
    let ctx_fn: FnContext = unsafe { transmute(vtable_method(base, root, 0x4C, VA_CONTEXT_AT)?) };
    let text_fn: FnText = unsafe { transmute(base + VA_TEXT_ASSIGN - PREFERRED_BASE) };

    let mut selected_ctx: *mut c_void = null_mut();
    let mut selected_rank: i32 = -1;

    for idx in 0..count {
        // SAFETY: Verified `GetViewContext`.
        let ctx = unsafe { ctx_fn(root, idx) };
        if !is_readable(ctx as usize, 0x15c) {
            continue;
        }
        let frames_slot = (root as usize) + 0x40;
        if !is_readable(frames_slot, 4) {
            continue;
        }
        // SAFETY: Reads active frame flag at `frames[idx] + 8` matching verified C++ bridge.
        let skip = unsafe {
            let frames = *(frames_slot as *const *const *const u8);
            !is_readable((frames as usize) + (idx as usize) * 4, 4)
                || !is_readable((*frames.add(idx as usize) as usize) + 8, 1)
                || *(*frames.add(idx as usize)).add(8) != 0
        };
        if skip {
            continue;
        }

        let Ok(compile_addr) = vtable_method(base, ctx, 0x58, VA_COMPILE) else {
            continue;
        };
        let Ok(invoke_addr) = vtable_method(base, ctx, 0x50, VA_INVOKE) else {
            continue;
        };
        let compile: FnCompile = unsafe { transmute(compile_addr) };
        let invoke: FnInvoke = unsafe { transmute(invoke_addr) };

        let mut func_scope = ValueScope::new(base, ctx);
        // SAFETY: Verified `Compile` on active EAWebKit view context.
        if !unsafe { compile(ctx, probe_c.as_ptr(), &mut func_scope.storage) } {
            continue;
        }
        let mut arg_scope = ValueScope::new(base, ctx);
        let mut res_scope = ValueScope::new(base, ctx);
        // SAFETY: Verified `Text` assign + `Invoke`.
        unsafe {
            text_fn(observe_c.as_ptr(), &mut arg_scope.storage);
            if !invoke(
                ctx,
                &mut func_scope.storage,
                &mut res_scope.storage,
                &mut arg_scope.storage,
            ) {
                continue;
            }
        }
        let Ok(obs_json) = copy_value_utf8(base, res_scope.storage.value) else {
            continue;
        };
        if !obs_json.contains("\"ok\":true") {
            continue;
        }
        let rank = if probe_script.is_some() {
            if let Ok(parsed) = serde_json::from_str::<serde_json::Value>(&obs_json) {
                parsed.get("rank").and_then(|v| v.as_i64()).unwrap_or(1) as i32
            } else {
                1
            }
        } else if obs_json.contains("\"ingame.options\"") {
            4
        } else if obs_json.contains("\"main.menu\"") {
            3
        } else if obs_json.contains("\"launcher.menu\"") {
            2
        } else {
            1
        };
        if rank > selected_rank {
            selected_rank = rank;
            selected_ctx = ctx;
        }
    }

    if selected_ctx.is_null() {
        return Err("No active EAWebKit view accepted UI patch script".into());
    }

    let compile: FnCompile = unsafe { transmute(vtable_method(base, selected_ctx, 0x58, VA_COMPILE)?) };
    let invoke: FnInvoke = unsafe { transmute(vtable_method(base, selected_ctx, 0x50, VA_INVOKE)?) };
    let mut func_scope = ValueScope::new(base, selected_ctx);
    // SAFETY: Verified `Compile` for final sync packet.
    if !unsafe { compile(selected_ctx, script_c.as_ptr(), &mut func_scope.storage) } {
        return Err("Selected EAWebKit view failed compiling UI patch applicator".into());
    }
    let mut arg_scope = ValueScope::new(base, selected_ctx);
    let mut res_scope = ValueScope::new(base, selected_ctx);
    // SAFETY: Verified `Text` assign + `Invoke`.
    unsafe {
        text_fn(packet_c.as_ptr(), &mut arg_scope.storage);
        if !invoke(
            selected_ctx,
            &mut func_scope.storage,
            &mut res_scope.storage,
            &mut arg_scope.storage,
        ) {
            return Err("EAWebKit UI patch invoke failed".into());
        }
    }
    copy_value_utf8(base, res_scope.storage.value)
}
