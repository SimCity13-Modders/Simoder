#![allow(unsafe_code)]

//! Windows `d3d9.dll` C ABI proxy forwarding table and exported entrypoints (`ordinals 16..=38`).
//!
//! When `SimCity.exe` loads `d3d9.dll` and invokes [`Direct3DCreate9`] or [`Direct3DCreate9Ex`],
//! the proxy ensures the genuine system `d3d9.dll` is loaded, forwards the Direct3D9 factory call,
//! triggers the one-shot Simoder background bootstrap off the graphics stack, and keeps the DLL
//! pinned for process lifetime.

use std::sync::OnceLock;
use std::sync::atomic::{AtomicU64, Ordering};

use parking_lot::Mutex;

use crate::exports::{D3D9_EXPORT_COUNT, D3d9Export};
use crate::runtime::IntegratedSimoderRuntime;

/// Signature of `Direct3DCreate9(UINT SDKVersion) -> IDirect3D9*`.
pub type FnDirect3DCreate9 = extern "system" fn(u32) -> usize;

/// Signature of `Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) -> HRESULT`.
pub type FnDirect3DCreate9Ex = extern "system" fn(u32, usize) -> i32;

/// Signature of `Direct3DCreate9On12(UINT, D3D9ON12_ARGS*, UINT) -> IDirect3D9*`.
pub type FnDirect3DCreate9On12 = extern "system" fn(u32, usize, u32) -> usize;

/// Signature of `Direct3DCreate9On12Ex(UINT, D3D9ON12_ARGS*, UINT, IDirect3D9Ex**) -> HRESULT`.
pub type FnDirect3DCreate9On12Ex = extern "system" fn(u32, usize, u32, usize) -> i32;

/// Signature of `D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName) -> int`.
pub type FnD3dPerfBeginEvent = extern "system" fn(u32, usize) -> i32;

/// Signature of `D3DPERF_EndEvent() -> int`.
pub type FnD3dPerfEndEvent = extern "system" fn() -> i32;

/// Signature of `D3DPERF_GetStatus() -> DWORD`.
pub type FnD3dPerfGetStatus = extern "system" fn() -> u32;

/// Signature of `D3DPERF_QueryRepeatFrame() -> BOOL`.
pub type FnD3dPerfQueryRepeatFrame = extern "system" fn() -> i32;

/// Signature of `D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName)`.
pub type FnD3dPerfSetMarker = extern "system" fn(u32, usize);

/// Signature of `D3DPERF_SetOptions(DWORD dwOptions)`.
pub type FnD3dPerfSetOptions = extern "system" fn(u32);

/// Signature of `D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName)`.
pub type FnD3dPerfSetRegion = extern "system" fn(u32, usize);

/// Signature of `Direct3D9EnableMaximizedWindowedModeShim(BOOL) -> BOOL`.
pub type FnMaximizedWindowedShim = extern "system" fn(i32) -> i32;

/// Thread-safe forwarding table holding resolved genuine Windows `System32\d3d9.dll` targets
/// and invocation counters for all 23 `d3d9.dll` ordinals (`16..=38`).
pub struct D3d9ForwardingTable {
    create9_target: Mutex<Option<FnDirect3DCreate9>>,
    create9ex_target: Mutex<Option<FnDirect3DCreate9Ex>>,
    raw_ordinal_targets: Mutex<[usize; D3D9_EXPORT_COUNT]>,
    factory_call_count: AtomicU64,
    last_sdk_version: AtomicU64,
}

impl Default for D3d9ForwardingTable {
    fn default() -> Self {
        Self::new()
    }
}

impl D3d9ForwardingTable {
    /// Construct an empty [`D3d9ForwardingTable`] with safe fallback targets.
    pub const fn new() -> Self {
        Self {
            create9_target: Mutex::new(None),
            create9ex_target: Mutex::new(None),
            raw_ordinal_targets: Mutex::new([0; D3D9_EXPORT_COUNT]),
            factory_call_count: AtomicU64::new(0),
            last_sdk_version: AtomicU64::new(0),
        }
    }

    /// Bind the genuine system `Direct3DCreate9` callback target.
    pub fn set_create9_target(&self, target: Option<FnDirect3DCreate9>) {
        *self.create9_target.lock() = target;
    }

    /// Bind the genuine system `Direct3DCreate9Ex` callback target.
    pub fn set_create9ex_target(&self, target: Option<FnDirect3DCreate9Ex>) {
        *self.create9ex_target.lock() = target;
    }

    /// Bind a raw function address for a specific [`D3d9Export`] ordinal slot.
    pub fn set_ordinal_target(&self, export: D3d9Export, address: usize) {
        self.raw_ordinal_targets.lock()[export.index()] = address;
    }

    /// Read the bound raw function address for a specific [`D3d9Export`] ordinal slot.
    pub fn ordinal_target(&self, export: D3d9Export) -> usize {
        self.raw_ordinal_targets.lock()[export.index()]
    }

    /// Return the total number of `Direct3DCreate9` / `Direct3DCreate9Ex` factory calls observed.
    pub fn factory_call_count(&self) -> u64 {
        self.factory_call_count.load(Ordering::Acquire)
    }

    /// Return the `SDKVersion` argument passed to the most recent factory call (`32` = `D3D_SDK_VERSION`).
    pub fn last_sdk_version(&self) -> u32 {
        self.last_sdk_version.load(Ordering::Acquire) as u32
    }

    /// Dispatch `Direct3DCreate9(sdk_version)` through the configured target (or return `0` / `NULL` if unbound).
    pub fn invoke_create9(&self, sdk_version: u32) -> usize {
        self.factory_call_count.fetch_add(1, Ordering::AcqRel);
        self.last_sdk_version
            .store(u64::from(sdk_version), Ordering::Release);
        let target = *self.create9_target.lock();
        target.map_or(0, |f| f(sdk_version))
    }

    /// Dispatch `Direct3DCreate9Ex(sdk_version, out_d3d_ptr)` through the configured target (or return `E_FAIL` if unbound).
    pub fn invoke_create9ex(&self, sdk_version: u32, out_d3d_ptr: usize) -> i32 {
        self.factory_call_count.fetch_add(1, Ordering::AcqRel);
        self.last_sdk_version
            .store(u64::from(sdk_version), Ordering::Release);
        let target = *self.create9ex_target.lock();
        target.map_or(-2147467259, |f| f(sdk_version, out_d3d_ptr))
    }
}

static GLOBAL_RUNTIME: OnceLock<IntegratedSimoderRuntime> = OnceLock::new();
static GLOBAL_FORWARDER: OnceLock<D3d9ForwardingTable> = OnceLock::new();

/// Access the process-wide [`IntegratedSimoderRuntime`] singleton hosted inside `d3d9.dll`.
pub fn global_simoder_runtime() -> &'static IntegratedSimoderRuntime {
    GLOBAL_RUNTIME.get_or_init(IntegratedSimoderRuntime::new)
}

/// Access the process-wide [`D3d9ForwardingTable`] singleton hosted inside `d3d9.dll`.
pub fn global_d3d9_forwarder() -> &'static D3d9ForwardingTable {
    GLOBAL_FORWARDER.get_or_init(D3d9ForwardingTable::new)
}

#[cfg(all(windows, not(test)))]
/// Windows DLL entry point recording the proxy `HMODULE` and handling safe detach.
// SAFETY: Standard Win32 `DllMain` entrypoint called under loader lock; does no `LoadLibrary` or blocking joins.
#[unsafe(no_mangle)]
pub extern "system" fn DllMain(
    instance: *mut core::ffi::c_void,
    reason: u32,
    lpv_reserved: *mut core::ffi::c_void,
) -> i32 {
    const DLL_PROCESS_ATTACH: u32 = 1;
    const DLL_PROCESS_DETACH: u32 = 0;
    if reason == DLL_PROCESS_ATTACH {
        crate::win32_loader::on_process_attach(instance);
    } else if reason == DLL_PROCESS_DETACH && lpv_reserved.is_null() {
        crate::win32_loader::shutdown_loaded_native_modules();
    }
    1
}

/// Exported Windows `d3d9.dll` ordinal 37 factory entrypoint (`Direct3DCreate9`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `Direct3DCreate9(UINT SDKVersion)` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn Direct3DCreate9(sdk_version: u32) -> usize {
    #[cfg(all(windows, not(test)))]
    crate::win32_loader::ensure_system_d3d9_loaded();

    let result = global_d3d9_forwarder().invoke_create9(sdk_version);

    #[cfg(all(windows, not(test)))]
    if result != 0 {
        crate::win32_loader::spawn_bootstrap_once();
    }

    result
}

/// Exported Windows `d3d9.dll` ordinal 38 factory entrypoint (`Direct3DCreate9Ex`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `Direct3DCreate9Ex(UINT, IDirect3D9Ex**)` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn Direct3DCreate9Ex(sdk_version: u32, out_d3d_ptr: usize) -> i32 {
    #[cfg(all(windows, not(test)))]
    crate::win32_loader::ensure_system_d3d9_loaded();

    let result = global_d3d9_forwarder().invoke_create9ex(sdk_version, out_d3d_ptr);

    #[cfg(all(windows, not(test)))]
    if result >= 0 {
        crate::win32_loader::spawn_bootstrap_once();
    }

    result
}

/// Exported Windows `d3d9.dll` ordinal 20 creation entrypoint (`Direct3DCreate9On12`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `Direct3DCreate9On12` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn Direct3DCreate9On12(
    sdk_version: u32,
    override_list: usize,
    num_overrides: u32,
) -> usize {
    #[cfg(all(windows, not(test)))]
    crate::win32_loader::ensure_system_d3d9_loaded();
    let addr = global_d3d9_forwarder().ordinal_target(D3d9Export::Direct3DCreate9On12);
    if addr != 0 {
        // SAFETY: `addr` resolved from system `d3d9.dll!Direct3DCreate9On12`.
        let f: FnDirect3DCreate9On12 = unsafe { std::mem::transmute(addr) };
        f(sdk_version, override_list, num_overrides)
    } else {
        0
    }
}

/// Exported Windows `d3d9.dll` ordinal 21 creation entrypoint (`Direct3DCreate9On12Ex`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `Direct3DCreate9On12Ex` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn Direct3DCreate9On12Ex(
    sdk_version: u32,
    override_list: usize,
    num_overrides: u32,
    out_d3d_ptr: usize,
) -> i32 {
    #[cfg(all(windows, not(test)))]
    crate::win32_loader::ensure_system_d3d9_loaded();
    let addr = global_d3d9_forwarder().ordinal_target(D3d9Export::Direct3DCreate9On12Ex);
    if addr != 0 {
        // SAFETY: `addr` resolved from system `d3d9.dll!Direct3DCreate9On12Ex`.
        let f: FnDirect3DCreate9On12Ex = unsafe { std::mem::transmute(addr) };
        f(sdk_version, override_list, num_overrides, out_d3d_ptr)
    } else {
        -2147467259
    }
}

/// Exported Windows `d3d9.dll` ordinal 27 PIX profiler entrypoint (`D3DPERF_BeginEvent`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_BeginEvent` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_BeginEvent(color: u32, name_ptr: usize) -> i32 {
    let addr = global_d3d9_forwarder().ordinal_target(D3d9Export::D3DPERF_BeginEvent);
    if addr != 0 {
        // SAFETY: `addr` resolved from system `d3d9.dll!D3DPERF_BeginEvent`.
        let f: FnD3dPerfBeginEvent = unsafe { std::mem::transmute(addr) };
        f(color, name_ptr)
    } else {
        0
    }
}

/// Exported Windows `d3d9.dll` ordinal 28 PIX profiler entrypoint (`D3DPERF_EndEvent`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_EndEvent` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_EndEvent() -> i32 {
    let addr = global_d3d9_forwarder().ordinal_target(D3d9Export::D3DPERF_EndEvent);
    if addr != 0 {
        // SAFETY: `addr` resolved from system `d3d9.dll!D3DPERF_EndEvent`.
        let f: FnD3dPerfEndEvent = unsafe { std::mem::transmute(addr) };
        f()
    } else {
        0
    }
}

/// Exported Windows `d3d9.dll` ordinal 29 PIX profiler entrypoint (`D3DPERF_GetStatus`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_GetStatus` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_GetStatus() -> u32 {
    let addr = global_d3d9_forwarder().ordinal_target(D3d9Export::D3DPERF_GetStatus);
    if addr != 0 {
        // SAFETY: `addr` resolved from system `d3d9.dll!D3DPERF_GetStatus`.
        let f: FnD3dPerfGetStatus = unsafe { std::mem::transmute(addr) };
        f()
    } else {
        0
    }
}

/// Exported Windows `d3d9.dll` ordinal 30 PIX profiler entrypoint (`D3DPERF_QueryRepeatFrame`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_QueryRepeatFrame` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_QueryRepeatFrame() -> i32 {
    let addr = global_d3d9_forwarder().ordinal_target(D3d9Export::D3DPERF_QueryRepeatFrame);
    if addr != 0 {
        // SAFETY: `addr` resolved from system `d3d9.dll!D3DPERF_QueryRepeatFrame`.
        let f: FnD3dPerfQueryRepeatFrame = unsafe { std::mem::transmute(addr) };
        f()
    } else {
        0
    }
}

/// Exported Windows `d3d9.dll` ordinal 31 PIX profiler entrypoint (`D3DPERF_SetMarker`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_SetMarker` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_SetMarker(color: u32, name_ptr: usize) {
    let addr = global_d3d9_forwarder().ordinal_target(D3d9Export::D3DPERF_SetMarker);
    if addr != 0 {
        // SAFETY: `addr` resolved from system `d3d9.dll!D3DPERF_SetMarker`.
        let f: FnD3dPerfSetMarker = unsafe { std::mem::transmute(addr) };
        f(color, name_ptr);
    }
}

/// Exported Windows `d3d9.dll` ordinal 32 PIX profiler entrypoint (`D3DPERF_SetOptions`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_SetOptions` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_SetOptions(options: u32) {
    let addr = global_d3d9_forwarder().ordinal_target(D3d9Export::D3DPERF_SetOptions);
    if addr != 0 {
        // SAFETY: `addr` resolved from system `d3d9.dll!D3DPERF_SetOptions`.
        let f: FnD3dPerfSetOptions = unsafe { std::mem::transmute(addr) };
        f(options);
    }
}

/// Exported Windows `d3d9.dll` ordinal 33 PIX profiler entrypoint (`D3DPERF_SetRegion`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_SetRegion` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_SetRegion(color: u32, name_ptr: usize) {
    let addr = global_d3d9_forwarder().ordinal_target(D3d9Export::D3DPERF_SetRegion);
    if addr != 0 {
        // SAFETY: `addr` resolved from system `d3d9.dll!D3DPERF_SetRegion`.
        let f: FnD3dPerfSetRegion = unsafe { std::mem::transmute(addr) };
        f(color, name_ptr);
    }
}

/// Exported Windows `d3d9.dll` ordinal 34 debug entrypoint (`DebugSetLevel`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `DebugSetLevel` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn DebugSetLevel() -> i32 {
    0
}

/// Exported Windows `d3d9.dll` ordinal 35 debug entrypoint (`DebugSetMute`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `DebugSetMute` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn DebugSetMute() {}

/// Exported Windows `d3d9.dll` ordinal 36 compatibility shim (`Direct3D9EnableMaximizedWindowedModeShim`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `Direct3D9EnableMaximizedWindowedModeShim` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn Direct3D9EnableMaximizedWindowedModeShim(enable: i32) -> i32 {
    #[cfg(all(windows, not(test)))]
    crate::win32_loader::ensure_system_d3d9_loaded();
    let addr =
        global_d3d9_forwarder().ordinal_target(D3d9Export::Direct3D9EnableMaximizedWindowedModeShim);
    if addr != 0 {
        // SAFETY: `addr` resolved from system `d3d9.dll!Direct3D9EnableMaximizedWindowedModeShim`.
        let f: FnMaximizedWindowedShim = unsafe { std::mem::transmute(addr) };
        f(enable)
    } else {
        0
    }
}

/// Exported Windows `d3d9.dll` ordinal 24 shader validator factory (`Direct3DShaderValidatorCreate9`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `Direct3DShaderValidatorCreate9` ABI.
#[unsafe(no_mangle)]
pub extern "system" fn Direct3DShaderValidatorCreate9() -> usize {
    0
}
