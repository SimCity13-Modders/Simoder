//! Static configuration constants for the `simoder-d3d9` bootstrap crate.

/// Current Rust runtime ABI surface version exposed by `simoder-d3d9`.
pub const SIMODER_RUST_ABI_VERSION: u32 = 1;

/// Expected host executable file name allowed to initialize the Simoder runtime.
pub const TARGET_EXECUTABLE_NAME: &str = "SimCity.exe";
