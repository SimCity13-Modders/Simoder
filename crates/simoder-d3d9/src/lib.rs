#![forbid(unsafe_code)]

//! Thin 32-bit x86 `d3d9.dll` proxy foundation for Simoder (`i686-pc-windows-msvc`):
//! export forwarding contract, PE32 retail build verification, COM vftable/shader validation,
//! and single-owner pinned bootstrap coordination.

pub mod bootstrap;
pub mod config;
pub mod exports;
pub mod pe;
pub mod vftable;

use std::path::Path;

pub use bootstrap::*;
pub use config::*;
pub use exports::*;
pub use pe::*;
pub use vftable::*;

/// Errors returned by `simoder-d3d9` installation path checks, PE32 parsing, and bootstrap orchestration.
#[derive(Debug, thiserror::Error)]
pub enum D3d9Error {
    /// Proxy installation directory or system DLL path failed validation.
    #[error("invalid install path layout: {0}")]
    InvalidInstallPath(String),
    /// Host or fixture executable failed 32-bit PE32 header or section bounds checks.
    #[error("invalid PE32 binary: {0}")]
    InvalidPe(String),
    /// Failure seeding or validating `config.toon`.
    #[error("format/config error: {0}")]
    Format(#[from] simoder_formats::FormatError),
    /// Filesystem I/O failure while inspecting host binaries or paths.
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
}

/// Verify whether `executable_path` matches the configured target [`TARGET_EXECUTABLE_NAME`] (`SimCity.exe`).
pub fn is_simcity_executable(executable_path: &Path) -> bool {
    executable_path
        .file_name()
        .and_then(|name| name.to_str())
        .is_some_and(|name| name.eq_ignore_ascii_case(TARGET_EXECUTABLE_NAME))
}

/// Return the compiled Rust runtime ABI version from [`config::SIMODER_RUST_ABI_VERSION`].
pub const fn runtime_abi_version() -> u32 {
    SIMODER_RUST_ABI_VERSION
}
