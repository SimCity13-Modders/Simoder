//! Static configuration, retail build fingerprint, ordinal, and D3D9 shader constants for `simoder-d3d9`.

/// Current Rust runtime ABI surface version exposed by `simoder-d3d9`.
pub const SIMODER_RUST_ABI_VERSION: u32 = 1;

/// Expected host executable file name allowed to initialize the Simoder runtime.
pub const TARGET_EXECUTABLE_NAME: &str = "SimCity.exe";

/// Genuine Windows Direct3D 9 system DLL filename resolved inside `System32` / `SysWOW64`.
pub const SYSTEM_D3D9_DLL_NAME: &str = "d3d9.dll";

/// Sibling data directory created next to the `SimCity/` folder for logs, config, and mods.
pub const SIMODER_DATA_DIR_NAME: &str = "simoder";

/// Optional sentinel filename inside [`SIMODER_DATA_DIR_NAME`] that disables runtime initialization
/// while keeping transparent D3D9 graphics forwarding active.
pub const PROXY_DISABLED_MARKER_FILE: &str = "proxy.disabled";

/// First exported ordinal of the Windows `d3d9.dll` proxy table (`16..=38`).
pub const D3D9_ORDINAL_BASE: u16 = 16;

/// Final exported ordinal of the Windows `d3d9.dll` proxy table (`Direct3DCreate9Ex`).
pub const D3D9_ORDINAL_LAST: u16 = 38;

/// Total number of forwarded exports spanning ordinals [`D3D9_ORDINAL_BASE`]..=[`D3D9_ORDINAL_LAST`].
pub const D3D9_EXPORT_COUNT: usize = 23;

/// Expected retail `SimCity.exe` PE file version matching verified hook addresses and schemas.
pub const SUPPORTED_GAME_VERSION: &str = "10.3.6.0";

/// Verified lowercase SHA-256 digest of the retail `SimCity.exe` (`10.3.6.0`) PE32 file on disk.
pub const SUPPORTED_GAME_FILE_SHA256: &str =
    "4e5f136a7bf58a52c4b97ed9c9a615a118d4a696e13aa8caa6a0a51fd4516d35";

/// Verified lowercase SHA-256 digest of the retail `SimCity.exe` (`10.3.6.0`) loaded `.text` section.
pub const SUPPORTED_GAME_TEXT_SHA256: &str =
    "d187c19201972abebcaa2fa5d8e38263d7a0b0fc3b6803d646636afb14c49d39";

/// Lower user-mode pointer threshold (`SMALL_MAX = 0x10000` from OpenSCP `hook_d3d9.js`)
/// used to reject small scalar integers before dereferencing candidate COM vftables.
pub const MIN_VALID_USER_ADDRESS: u32 = 0x0001_0000;

/// High 16-bit D3D9 Pixel Shader bytecode version token prefix (`0xFFFF_xxxx`), per Microsoft D3D9 shader spec
/// and verified in OpenSCP `hook_d3d9.js`.
pub const PIXEL_SHADER_VERSION_PREFIX: u16 = 0xFFFF;

/// High 16-bit D3D9 Vertex Shader bytecode version token prefix (`0xFFFE_xxxx`), per Microsoft D3D9 shader spec
/// and verified in OpenSCP `hook_d3d9.js`.
pub const VERTEX_SHADER_VERSION_PREFIX: u16 = 0xFFFE;

/// Minimum byte length of a valid DWORD-aligned D3D9 shader bytecode blob (`len + version + end token`).
pub const SHADER_BLOB_MIN_BYTES: usize = 16;

/// Defensive maximum byte length (4 MiB) for a captured D3D9 shader bytecode blob.
pub const SHADER_BLOB_MAX_BYTES: usize = 4 * 1024 * 1024;
