#![forbid(unsafe_code)]

//! Mod lifecycle management, manifest validation, asset packaging, persistent state tracking,
//! and ordered TGI patch conflict resolution for Simoder.

pub mod assets;
pub mod config;
pub mod keybinds;
pub mod manager;
pub mod manifest;
pub mod native_host;
pub mod native_trust;
pub mod overlay;
pub mod patch_registry;
pub mod state_store;
pub mod types;
pub mod ui_patch;

pub use {
    assets::*, config::*, keybinds::*, manager::*, manifest::*, native_host::*, native_trust::*,
    overlay::*, patch_registry::*, state_store::*, types::*, ui_patch::*,
};
