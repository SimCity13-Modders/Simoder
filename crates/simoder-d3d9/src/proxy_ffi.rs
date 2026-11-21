//! Windows `d3d9.dll` C ABI proxy forwarding table and exported entrypoints (`ordinals 16..=38`).
//!
//! When `SimCity.exe` loads `d3d9.dll` and invokes [`Direct3DCreate9`] or [`Direct3DCreate9Ex`],
//! the proxy records the factory call, triggers the one-shot Simoder runtime bootstrap, and delegates
//! to the registered system `d3d9.dll` function pointers in [`D3d9ForwardingTable`].

use std::sync::OnceLock;
use std::sync::atomic::{AtomicU64, Ordering};

use parking_lot::Mutex;

use crate::exports::{D3D9_EXPORT_COUNT, D3d9Export};
use crate::runtime::IntegratedSimoderRuntime;

/// Signature of `Direct3DCreate9(UINT SDKVersion) -> IDirect3D9*`.
pub type FnDirect3DCreate9 = extern "system" fn(u32) -> usize;

/// Signature of `Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) -> HRESULT`.
pub type FnDirect3DCreate9Ex = extern "system" fn(u32, usize) -> i32;

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
        target.map_or(-2147467259, |f| f(sdk_version, out_d3d_ptr)) // 0x80004005 (E_FAIL)
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

/// Exported Windows `d3d9.dll` ordinal 37 factory entrypoint (`Direct3DCreate9`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `Direct3DCreate9(UINT SDKVersion)` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn Direct3DCreate9(sdk_version: u32) -> usize {
    global_d3d9_forwarder().invoke_create9(sdk_version)
}

/// Exported Windows `d3d9.dll` ordinal 38 factory entrypoint (`Direct3DCreate9Ex`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `Direct3DCreate9Ex(UINT, IDirect3D9Ex**)` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn Direct3DCreate9Ex(sdk_version: u32, out_d3d_ptr: usize) -> i32 {
    global_d3d9_forwarder().invoke_create9ex(sdk_version, out_d3d_ptr)
}

/// Exported Windows `d3d9.dll` ordinal 27 PIX profiler entrypoint (`D3DPERF_BeginEvent`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_BeginEvent` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_BeginEvent(_color: u32, _name_ptr: usize) -> i32 {
    0
}

/// Exported Windows `d3d9.dll` ordinal 28 PIX profiler entrypoint (`D3DPERF_EndEvent`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_EndEvent` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_EndEvent() -> i32 {
    0
}

/// Exported Windows `d3d9.dll` ordinal 29 PIX profiler entrypoint (`D3DPERF_GetStatus`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_GetStatus` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_GetStatus() -> u32 {
    0
}

/// Exported Windows `d3d9.dll` ordinal 30 PIX profiler entrypoint (`D3DPERF_QueryRepeatFrame`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_QueryRepeatFrame` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_QueryRepeatFrame() -> i32 {
    0
}

/// Exported Windows `d3d9.dll` ordinal 31 PIX profiler entrypoint (`D3DPERF_SetMarker`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_SetMarker` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_SetMarker(_color: u32, _name_ptr: usize) {}

/// Exported Windows `d3d9.dll` ordinal 32 PIX profiler entrypoint (`D3DPERF_SetOptions`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_SetOptions` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_SetOptions(_options: u32) {}

/// Exported Windows `d3d9.dll` ordinal 33 PIX profiler entrypoint (`D3DPERF_SetRegion`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `D3DPERF_SetRegion` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn D3DPERF_SetRegion(_color: u32, _name_ptr: usize) {}

/// Exported Windows `d3d9.dll` ordinal 34 debug entrypoint (`DebugSetLevel`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `DebugSetLevel` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn DebugSetLevel(_level: u32) -> i32 {
    0
}

/// Exported Windows `d3d9.dll` ordinal 35 debug entrypoint (`DebugSetMute`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `DebugSetMute` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn DebugSetMute() {}

/// Exported Windows `d3d9.dll` ordinal 36 compatibility shim (`Direct3D9EnableMaximizedWindowedModeShim`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `Direct3D9EnableMaximizedWindowedModeShim` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn Direct3D9EnableMaximizedWindowedModeShim(_enable: i32) -> i32 {
    0
}

/// Exported Windows `d3d9.dll` ordinal 24 shader validator factory (`Direct3DShaderValidatorCreate9`).
// SAFETY: Exported C/system symbol matching Windows `d3d9.dll` `Direct3DShaderValidatorCreate9` ABI.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "system" fn Direct3DShaderValidatorCreate9() -> usize {
    0
}
