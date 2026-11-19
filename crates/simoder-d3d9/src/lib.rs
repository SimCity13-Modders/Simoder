#![forbid(unsafe_code)]

//! Thin 32-bit x86 `cdylib` bootstrap foundation for Simoder (`i686-pc-windows-msvc`).

pub mod config;

use std::path::Path;

pub use config::{SIMODER_RUST_ABI_VERSION, TARGET_EXECUTABLE_NAME};

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
