#![forbid(unsafe_code)]

//! Mod lifecycle management, manifest validation, asset packaging, persistent state tracking,
//! and ordered TGI patch conflict resolution for Simoder.

pub mod assets;
pub mod config;
pub mod manager;
pub mod manifest;
pub mod overlay;
pub mod patch_registry;
pub mod state_store;
pub mod types;

pub use {
    assets::*, config::*, manager::*, manifest::*, overlay::*, patch_registry::*, state_store::*,
    types::*,
};
