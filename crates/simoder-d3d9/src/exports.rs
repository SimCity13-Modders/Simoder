//! Windows `d3d9.dll` 23-ordinal ABI contract (`16..=38`) and module-definition (`.def`) renderer.

use std::fmt::Write as _;

use crate::config::{D3D9_EXPORT_COUNT, D3D9_ORDINAL_BASE, D3D9_ORDINAL_LAST};

/// Windows system `d3d9.dll` export slot (`Ordinal Base = 16`, ordinals `16..=38`).
///
/// System `d3d9.dll` begins its PE Export Directory at ordinal `16` and exports six internal
/// slots (`16..=19` and `22..=23`) strictly by ordinal (`NONAME` without ASCII symbol strings
/// in the PE export name table), while `20..=21` and `24..=38` expose named Windows APIs.
#[repr(u16)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum D3d9Export {
    /// Internal Windows `d3d9.dll` `NONAME` export at ordinal 16.
    Ordinal16 = 16,
    /// Internal Windows `d3d9.dll` `NONAME` export at ordinal 17.
    Ordinal17 = 17,
    /// Internal Windows `d3d9.dll` `NONAME` export at ordinal 18.
    Ordinal18 = 18,
    /// Internal Windows `d3d9.dll` `NONAME` export at ordinal 19.
    Ordinal19 = 19,
    /// Windows 10+ Direct3D 9-on-12 creation entrypoint at ordinal 20.
    Direct3DCreate9On12 = 20,
    /// Extended Direct3D 9-on-12 creation entrypoint at ordinal 21.
    Direct3DCreate9On12Ex = 21,
    /// Internal Windows `d3d9.dll` `NONAME` export at ordinal 22.
    Ordinal22 = 22,
    /// Internal Windows `d3d9.dll` `NONAME` export at ordinal 23.
    Ordinal23 = 23,
    /// Direct3D 9 shader validator factory at ordinal 24.
    Direct3DShaderValidatorCreate9 = 24,
    /// Internal/legacy PSGP error hook export at ordinal 25.
    PSGPError = 25,
    /// Internal/legacy PSGP texture sampling export at ordinal 26.
    PSGPSampleTexture = 26,
    /// PIX/profiler performance event begin marker at ordinal 27.
    D3DPERF_BeginEvent = 27,
    /// PIX/profiler performance event end marker at ordinal 28.
    D3DPERF_EndEvent = 28,
    /// PIX/profiler capture status query at ordinal 29.
    D3DPERF_GetStatus = 29,
    /// PIX/profiler repeat-frame query at ordinal 30.
    D3DPERF_QueryRepeatFrame = 30,
    /// PIX/profiler instant event marker at ordinal 31.
    D3DPERF_SetMarker = 31,
    /// PIX/profiler options setter at ordinal 32.
    D3DPERF_SetOptions = 32,
    /// PIX/profiler region color marker at ordinal 33.
    D3DPERF_SetRegion = 33,
    /// Direct3D debug level control export at ordinal 34.
    DebugSetLevel = 34,
    /// Direct3D debug mute toggle export at ordinal 35.
    DebugSetMute = 35,
    /// Maximized windowed mode compatibility shim at ordinal 36.
    Direct3D9EnableMaximizedWindowedModeShim = 36,
    /// Primary Direct3D 9 factory entrypoint (`IDirect3D9*`) triggering Simoder bootstrap at ordinal 37.
    Direct3DCreate9 = 37,
    /// Primary Direct3D 9Ex factory entrypoint (`HRESULT`) triggering Simoder bootstrap at ordinal 38.
    Direct3DCreate9Ex = 38,
}

impl D3d9Export {
    /// Complete ordered list of all 23 Windows `d3d9.dll` export slots (`16..=38`).
    pub const ALL: [Self; D3D9_EXPORT_COUNT] = [
        Self::Ordinal16,
        Self::Ordinal17,
        Self::Ordinal18,
        Self::Ordinal19,
        Self::Direct3DCreate9On12,
        Self::Direct3DCreate9On12Ex,
        Self::Ordinal22,
        Self::Ordinal23,
        Self::Direct3DShaderValidatorCreate9,
        Self::PSGPError,
        Self::PSGPSampleTexture,
        Self::D3DPERF_BeginEvent,
        Self::D3DPERF_EndEvent,
        Self::D3DPERF_GetStatus,
        Self::D3DPERF_QueryRepeatFrame,
        Self::D3DPERF_SetMarker,
        Self::D3DPERF_SetOptions,
        Self::D3DPERF_SetRegion,
        Self::DebugSetLevel,
        Self::DebugSetMute,
        Self::Direct3D9EnableMaximizedWindowedModeShim,
        Self::Direct3DCreate9,
        Self::Direct3DCreate9Ex,
    ];

    /// Resolve a [`D3d9Export`] variant from its numeric PE ordinal (`16..=38`).
    pub const fn from_ordinal(ordinal: u16) -> Option<Self> {
        if ordinal >= D3D9_ORDINAL_BASE && ordinal <= D3D9_ORDINAL_LAST {
            Some(Self::ALL[(ordinal - D3D9_ORDINAL_BASE) as usize])
        } else {
            None
        }
    }

    /// Return the numeric PE export ordinal (`16..=38`).
    pub const fn ordinal(self) -> u16 {
        self as u16
    }

    /// Return the zero-based forwarding table index (`ordinal - 16`, in `0..23`).
    pub const fn index(self) -> usize {
        (self.ordinal() - D3D9_ORDINAL_BASE) as usize
    }

    /// Return `true` on factory entrypoints ([`Self::Direct3DCreate9`], [`Self::Direct3DCreate9Ex`])
    /// that initiate Simoder runtime bootstrap.
    pub const fn triggers_bootstrap(self) -> bool {
        matches!(self, Self::Direct3DCreate9 | Self::Direct3DCreate9Ex)
    }

    /// Return the Windows PE export symbol name, or `None` for `NONAME` ordinal-only exports (`16..=19`, `22..=23`).
    pub const fn symbol_name(self) -> Option<&'static str> {
        match self {
            Self::Ordinal16
            | Self::Ordinal17
            | Self::Ordinal18
            | Self::Ordinal19
            | Self::Ordinal22
            | Self::Ordinal23 => None,
            Self::Direct3DCreate9On12 => Some("Direct3DCreate9On12"),
            Self::Direct3DCreate9On12Ex => Some("Direct3DCreate9On12Ex"),
            Self::Direct3DShaderValidatorCreate9 => Some("Direct3DShaderValidatorCreate9"),
            Self::PSGPError => Some("PSGPError"),
            Self::PSGPSampleTexture => Some("PSGPSampleTexture"),
            Self::D3DPERF_BeginEvent => Some("D3DPERF_BeginEvent"),
            Self::D3DPERF_EndEvent => Some("D3DPERF_EndEvent"),
            Self::D3DPERF_GetStatus => Some("D3DPERF_GetStatus"),
            Self::D3DPERF_QueryRepeatFrame => Some("D3DPERF_QueryRepeatFrame"),
            Self::D3DPERF_SetMarker => Some("D3DPERF_SetMarker"),
            Self::D3DPERF_SetOptions => Some("D3DPERF_SetOptions"),
            Self::D3DPERF_SetRegion => Some("D3DPERF_SetRegion"),
            Self::DebugSetLevel => Some("DebugSetLevel"),
            Self::DebugSetMute => Some("DebugSetMute"),
            Self::Direct3D9EnableMaximizedWindowedModeShim => {
                Some("Direct3D9EnableMaximizedWindowedModeShim")
            }
            Self::Direct3DCreate9 => Some("Direct3DCreate9"),
            Self::Direct3DCreate9Ex => Some("Direct3DCreate9Ex"),
        }
    }
}

/// Return all 23 Windows `d3d9.dll` exports ordered by ordinal (`16..=38`).
pub const fn all_d3d9_exports() -> [D3d9Export; D3D9_EXPORT_COUNT] {
    D3d9Export::ALL
}

/// Look up an export variant by its numeric PE ordinal (`16..=38`).
pub const fn lookup_export_by_ordinal(ordinal: u16) -> Option<D3d9Export> {
    D3d9Export::from_ordinal(ordinal)
}

/// Look up an export variant by its exact public Windows `d3d9.dll` symbol name.
pub fn lookup_export_by_name(name: &str) -> Option<D3d9Export> {
    D3d9Export::ALL
        .into_iter()
        .find(|export| export.symbol_name() == Some(name))
}

/// Render a linker module-definition file (`.def`) preserving all 23 Windows `d3d9.dll` ordinals
/// and `stdcall` factory decorations (`@4` / `@8`).
pub fn render_module_def() -> String {
    let mut out = String::from("LIBRARY d3d9\nEXPORTS\n");
    for export in D3d9Export::ALL {
        let ordinal = export.ordinal();
        match export {
            D3d9Export::Direct3DCreate9 => {
                let _ = writeln!(out, "    Direct3DCreate9=_SimoderDirect3DCreate9@4 @{ordinal}");
            }
            D3d9Export::Direct3DCreate9Ex => {
                let _ = writeln!(out, "    Direct3DCreate9Ex=_SimoderDirect3DCreate9Ex@8 @{ordinal}");
            }
            _ => match export.symbol_name() {
                Some(name) => {
                    let _ = writeln!(out, "    {name}=SimoderProxyOrdinal{ordinal} @{ordinal}");
                }
                None => {
                    let _ = writeln!(
                        out,
                        "    SimoderOrdinal{ordinal}=SimoderProxyOrdinal{ordinal} @{ordinal} NONAME"
                    );
                }
            },
        }
    }
    out
}
