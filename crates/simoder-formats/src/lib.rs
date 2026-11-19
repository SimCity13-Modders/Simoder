#![forbid(unsafe_code)]

//! Unified SimCity asset and configuration format layer built atop `Simoder-OpenSCP` and `crates.io` codecs.

pub mod config;
pub mod toon;

use simoder_core::{Tgi, fnv1_name_hash};

pub use config::{
    CONFIG_TOON_FILE_NAME, DEFAULT_LOCALE_TAG, DEFAULT_MODS_DIR, DeveloperConsoleConfig,
    LoaderConfig, default_config_text, load_or_create_config,
};
pub use dbpf;
pub use dbpf::{
    CachedPackage, ERZ_BINARY_RULE_TYPE, ERZ_TEXT_RULE_TYPE, ErzError, ErzSummary, IndexEntry,
    OverlayEntry, Package, ResourceId, erz_parse_summary, refpack_decompress,
    refpack_stream_header, write_uncompressed_overlay,
};
pub use rw4;
pub use rw4::{FileType as Rw4FileType, Rw4File};
pub use sc_format;
pub use sc_format::{Format, probe};
pub use sc_properties;
pub use sc_properties::{
    Key, Kind, PropType, Property, PropertyFile, Text, Value,
    debug_tools::{DebugToolPatch, build_debug_tools_overlay},
    inherit::flatten_parent_inheritance,
    locale::{
        LOCALE_RESOURCE_TYPE, Locale, LocaleItem, parse_locale_items, parse_string_table,
        serialize_locale_items,
    },
};
pub use toon::{
    DecodeOptions, TOON_MAX_FILE_BYTES, ToonError, ToonNumber, ToonValue, UTF8_BOM,
    load_toon_file, load_toon_file_typed, parse_toon, parse_toon_typed, toon_from_str,
    toon_from_str_with_options, toon_to_string,
};

/// Errors returned by Simoder asset and configuration codecs.
#[derive(Debug, thiserror::Error)]
pub enum FormatError {
    /// Failure reading a DBPF container or decoding RefPack streams.
    #[error("dbpf read error: {0}")]
    Dbpf(#[from] dbpf::Error),
    /// Failure writing a DBPF overlay package.
    #[error("dbpf write error: {0}")]
    DbpfWriter(#[from] dbpf::WriterError),
    /// Failure decoding a compiled ERZ/ER2 rule library.
    #[error("erz decode error: {0}")]
    Erz(#[from] ErzError),
    /// Failure decoding or encoding a `0x00B1B104` property list.
    #[error("property decode error: {0}")]
    Property(#[from] sc_properties::Error),
    /// Failure parsing TOML loader configuration.
    #[error("loader config parse error: {0}")]
    ConfigParse(#[from] toml::de::Error),
    /// Failure decoding or validating TOON configuration/manifest input via [`serde_toon`].
    #[error("toon parse error: {0}")]
    Toon(#[from] ToonError),
    /// Semantic validation failure for loader configuration fields or file limits.
    #[error("loader config validation error: {0}")]
    ConfigValidation(String),
    /// Filesystem I/O failure while loading or atomically saving configuration files.
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
}

/// Derive a collision-free custom locale table instance ID for a mod outside the vanilla `Locale/` directory.
///
/// Computes `fnv1_name_hash("<mod_id>/strings")` so mod localization packages mounted in `SimCityData/`
/// do not collide with base-game `0x0A98EAF0` tables (`0x6C969DEE`, `0x50AA0BEA`, `0x4B54417A`).
pub fn mod_locale_table_id(mod_id: &str) -> u32 {
    fnv1_name_hash(&format!("{mod_id}/strings"))
}

/// Build a deterministic DBPF v3 overlay package without package-info metadata.
///
/// Omits `0x00B1B104` package-info records in group `0x40404000` so retail `SP::AddPackageIfValid`
/// bypasses RSA signature enforcement cleanly.
pub fn build_resource_overlay(entries: &[(Tgi, Vec<u8>)]) -> Result<Vec<u8>, FormatError> {
    let overlay_entries: Vec<OverlayEntry> = entries
        .iter()
        .map(|(tgi, data)| OverlayEntry::new(*tgi, data.clone()))
        .collect();
    Ok(write_uncompressed_overlay(&overlay_entries)?)
}
