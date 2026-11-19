//! Integration tests for `simoder-d3d9` host validation and configuration constants.

use std::path::Path;

use simoder_d3d9::{SIMODER_RUST_ABI_VERSION, is_simcity_executable, runtime_abi_version};

#[test]
fn matches_simcity_executable_case_insensitively() {
    assert!(is_simcity_executable(Path::new(
        r"C:\Games\SimCity\SimCity\SimCity.exe"
    )));
    assert!(is_simcity_executable(Path::new("simcity.EXE")));
    assert!(!is_simcity_executable(Path::new(
        r"C:\Games\SimCity\EADesktop.exe"
    )));
    assert!(!is_simcity_executable(Path::new("")));
}

#[test]
fn returns_configured_abi_version() {
    assert_eq!(runtime_abi_version(), SIMODER_RUST_ABI_VERSION);
}
