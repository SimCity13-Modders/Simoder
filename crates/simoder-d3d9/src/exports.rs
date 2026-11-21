#![forbid(unsafe_code)]

//! Windows `d3d9.dll` 23-ordinal ABI contract (`16..=38`) and module-definition (`.def`) renderer.

use std::str::FromStr;

use strum::{EnumString, FromRepr, IntoStaticStr, VariantArray};

/// Windows system `d3d9.dll` export slot (`Ordinal Base = 16`, ordinals `16..=38`).
///
/// Each variant is the single source of truth for its ordinal (`= N`), forwarding slot
/// (`ordinal - 16`), and PE export symbol name (derived via `strum`; internal `Ordinal*`
/// variants represent `NONAME` ordinal-only Windows exports).
#[repr(u16)]
#[allow(non_camel_case_types)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, EnumString, FromRepr, IntoStaticStr, VariantArray)]
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

/// Total number of forwarded Windows `d3d9.dll` exports defined on [`D3d9Export`].
pub const D3D9_EXPORT_COUNT: usize = D3d9Export::VARIANTS.len();

/// First exported ordinal of the Windows `d3d9.dll` proxy table, derived from [`D3d9Export`].
pub const D3D9_ORDINAL_BASE: u16 = D3d9Export::VARIANTS[0].ordinal();

/// Final exported ordinal of the Windows `d3d9.dll` proxy table, derived from [`D3d9Export`].
pub const D3D9_ORDINAL_LAST: u16 = D3d9Export::VARIANTS[D3D9_EXPORT_COUNT - 1].ordinal();

impl D3d9Export {
    /// Resolve a [`D3d9Export`] variant from its numeric PE ordinal (`16..=38`).
    pub const fn from_ordinal(ordinal: u16) -> Option<Self> {
        Self::from_repr(ordinal)
    }

    /// Return the numeric PE export ordinal (`16..=38`).
    pub const fn ordinal(self) -> u16 {
        self as u16
    }

    /// Return the zero-based forwarding table index (`ordinal - D3D9_ORDINAL_BASE`, in `0..23`).
    pub const fn index(self) -> usize {
        (self.ordinal() - D3D9_ORDINAL_BASE) as usize
    }

    /// Return `true` on factory entrypoints ([`Self::Direct3DCreate9`], [`Self::Direct3DCreate9Ex`])
    /// that initiate Simoder runtime bootstrap.
    pub const fn triggers_bootstrap(self) -> bool {
        matches!(self, Self::Direct3DCreate9 | Self::Direct3DCreate9Ex)
    }

    /// Return the Windows PE export symbol name, or `None` for `NONAME` ordinal-only exports (`Ordinal*`).
    pub fn symbol_name(self) -> Option<&'static str> {
        let name: &'static str = self.into();
        (!name.starts_with("Ordinal")).then_some(name)
    }
}

/// Return all Windows `d3d9.dll` exports ordered by declaration via [`VariantArray::VARIANTS`].
pub const fn all_d3d9_exports() -> &'static [D3d9Export] {
    D3d9Export::VARIANTS
}

/// Look up an export variant by its numeric PE ordinal (`16..=38`).
pub const fn lookup_export_by_ordinal(ordinal: u16) -> Option<D3d9Export> {
    D3d9Export::from_repr(ordinal)
}

/// Look up an export variant by its exact public Windows `d3d9.dll` symbol name via [`EnumString`].
pub fn lookup_export_by_name(name: &str) -> Option<D3d9Export> {
    D3d9Export::from_str(name)
        .ok()
        .filter(|export| export.symbol_name().is_some())
}

/// Render a linker module-definition file (`.def`) preserving all 23 Windows `d3d9.dll` ordinals
/// and `stdcall` factory decorations (`@4` / `@8`).
pub fn render_module_def() -> String {
    let mut out = String::from("LIBRARY d3d9\nEXPORTS\n");
    for &export in D3d9Export::VARIANTS {
        let ordinal = export.ordinal();
        let line = match (export, export.symbol_name()) {
            (D3d9Export::Direct3DCreate9, _) => {
                format!("    Direct3DCreate9=_SimoderDirect3DCreate9@4 @{ordinal}\n")
            }
            (D3d9Export::Direct3DCreate9Ex, _) => {
                format!("    Direct3DCreate9Ex=_SimoderDirect3DCreate9Ex@8 @{ordinal}\n")
            }
            (_, Some(name)) => format!("    {name}=SimoderProxyOrdinal{ordinal} @{ordinal}\n"),
            (_, None) => format!(
                "    SimoderOrdinal{ordinal}=SimoderProxyOrdinal{ordinal} @{ordinal} NONAME\n"
            ),
        };
        out.push_str(&line);
    }
    out
}
