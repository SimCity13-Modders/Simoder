//! Direct3D 9 COM vftable validation and shader bytecode identification derived from `Simoder-OpenSCP`.

use crate::config::{
    MIN_VALID_USER_ADDRESS, PIXEL_SHADER_VERSION_PREFIX, SHADER_BLOB_MAX_BYTES,
    SHADER_BLOB_MIN_BYTES, VERTEX_SHADER_VERSION_PREFIX,
};

/// Minimum number of initial COM vftable slots (`IUnknown` + initial device/shader methods) validated
/// before accepting a pointer as a genuine D3D9 COM vtable (`looksLikeComObj` in `hook_d3d9.js`).
pub const COM_VFTABLE_MIN_SLOTS: usize = 5;

/// Half-open 32-bit virtual address range `[base, base + size)` for a loaded PE module or executable section.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ModuleRange {
    /// Start address (`0` when module is absent).
    pub base: u32,
    /// Span in bytes (`0` creates an empty range).
    pub size: u32,
}

impl ModuleRange {
    /// Construct a half-open [`ModuleRange`] starting at `base` spanning `size` bytes.
    pub const fn new(base: u32, size: u32) -> Self {
        Self { base, size }
    }

    /// Report whether `address` falls inside `[base, base + size)` without overflowing `u32`.
    pub const fn contains(self, address: u32) -> bool {
        self.size > 0
            && address >= self.base
            && address.saturating_sub(self.base) < self.size
    }
}

/// Shader program stage decoded from the high half of a D3D9 shader version token.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShaderKind {
    /// Pixel shader (`0xFFFF_xx_yy`).
    Pixel,
    /// Vertex shader (`0xFFFE_xx_yy`).
    Vertex,
}

/// Structural summary of one validated D3D9 shader bytecode blob.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ShaderBytecodeSummary {
    /// Pixel vs Vertex shader stage.
    pub kind: ShaderKind,
    /// Declared DWORD-0 total byte length including header.
    pub total_byte_len: u32,
    /// Shader model major version (for example `3` in `ps_3_0`).
    pub major_version: u8,
    /// Shader model minor version (for example `0` in `ps_3_0`).
    pub minor_version: u8,
}

/// Validate a candidate D3D9 COM object's vftable pointer and initial method entries.
///
/// Mirrors the verified strategy in `Simoder-OpenSCP/tools/dynamic/hook_d3d9.js`:
/// genuine D3D9 COM vftables reside in `.rdata` (non-executable memory inside `d3d9.dll` or
/// `SimCity.exe` engine wrappers), whereas their initial method pointers target executable
/// code inside `d3d9_exec`. Requiring the vtable itself to be executable falsely rejects
/// valid devices while admitting import jump tables.
pub fn validate_com_vftable(
    vftable_addr: u32,
    method_slots: &[u32],
    d3d9_module: ModuleRange,
    d3d9_exec: ModuleRange,
    game_module: ModuleRange,
) -> bool {
    if vftable_addr < MIN_VALID_USER_ADDRESS {
        return false;
    }
    let vtable_in_known_module = d3d9_exec.contains(vftable_addr)
        || d3d9_module.contains(vftable_addr)
        || game_module.contains(vftable_addr);
    if !vtable_in_known_module || method_slots.len() < COM_VFTABLE_MIN_SLOTS {
        return false;
    }
    method_slots[..COM_VFTABLE_MIN_SLOTS]
        .iter()
        .all(|&method_addr| d3d9_exec.contains(method_addr))
}

/// Inspect a captured SimCity/D3D9 shader container blob (`[total_len: u32][version_token: u32]...`).
///
/// Only requires the 8-byte header prefix (`total_byte_len` and `version_token`) without requiring
/// full trailing shader instructions; follows `isShaderBlob` in `Simoder-OpenSCP/tools/dynamic/hook_d3d9.js`
/// by enforcing DWORD alignment, bounds `SHADER_BLOB_MIN_BYTES..=SHADER_BLOB_MAX_BYTES`,
/// and checking the D3D9 version token prefix (`0xFFFF` pixel shader, `0xFFFE` vertex shader)
/// to distinguish true shaders from similarly shaped vertex declarations.
pub fn inspect_shader_blob(bytes: &[u8]) -> Option<ShaderBytecodeSummary> {
    if bytes.len() < 8 {
        return None;
    }
    let total_byte_len = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    let len_usize = total_byte_len as usize;
    if !(SHADER_BLOB_MIN_BYTES..=SHADER_BLOB_MAX_BYTES).contains(&len_usize)
        || (total_byte_len & 3) != 0
    {
        return None;
    }

    let version_token = u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]);
    let prefix = (version_token >> 16) as u16;
    let kind = match prefix {
        PIXEL_SHADER_VERSION_PREFIX => ShaderKind::Pixel,
        VERTEX_SHADER_VERSION_PREFIX => ShaderKind::Vertex,
        _ => return None,
    };

    let major_version = ((version_token >> 8) & 0xFF) as u8;
    let minor_version = (version_token & 0xFF) as u8;

    Some(ShaderBytecodeSummary {
        kind,
        total_byte_len,
        major_version,
        minor_version,
    })
}
