//! Bounded TOON 4.1+ codec wrapper backed by [`serde_toon`] with size and BOM guards.

use std::fs::File;
use std::io::Read as _;
use std::path::Path;

use serde::de::DeserializeOwned;

pub use serde_toon::{
    DecodeOptions, Error as ToonError, Number as ToonNumber, Value as ToonValue,
    from_str as toon_from_str, from_str_with_options as toon_from_str_with_options,
    to_string as toon_to_string,
};

use crate::FormatError;

/// Maximum byte size (1 MiB) accepted when reading or parsing a single `.toon` document.
///
/// Enforces a defensive 1 MiB input ceiling (`TOON_MAX_FILE_BYTES`) to reject accidentally
/// oversized or corrupted manifests before allocating parser state.
pub const TOON_MAX_FILE_BYTES: usize = 1024 * 1024;

/// Optional UTF-8 Byte Order Mark (`0xEF 0xBB 0xBF`) stripped prior to passing TOON text to [`serde_toon`].
pub const UTF8_BOM: &str = "\u{FEFF}";

/// Parse a UTF-8 TOON document into a dynamic [`ToonValue`] using [`DecodeOptions::strict`].
///
/// Returns an error if `text` exceeds [`TOON_MAX_FILE_BYTES`] or fails TOON v4.1 strict syntax/structure validation.
pub fn parse_toon(text: &str) -> Result<ToonValue, FormatError> {
    parse_toon_typed(text)
}

/// Deserialize a UTF-8 TOON document directly into a typed [`serde::de::DeserializeOwned`] model
/// using [`DecodeOptions::strict`].
///
/// Strips a leading [`UTF8_BOM`] if present and rejects inputs larger than [`TOON_MAX_FILE_BYTES`].
pub fn parse_toon_typed<T: DeserializeOwned>(text: &str) -> Result<T, FormatError> {
    if text.len() > TOON_MAX_FILE_BYTES {
        return Err(FormatError::ConfigValidation(format!(
            "TOON document exceeds the {} byte limit",
            TOON_MAX_FILE_BYTES
        )));
    }
    let clean = text.strip_prefix(UTF8_BOM).unwrap_or(text);
    Ok(toon_from_str_with_options(clean, DecodeOptions::strict())?)
}

/// Load a `.toon` file from disk within [`TOON_MAX_FILE_BYTES`] and parse it into a [`ToonValue`].
pub fn load_toon_file(path: &Path) -> Result<ToonValue, FormatError> {
    let content = read_bounded_toon_file(path)?;
    parse_toon(&content)
}

/// Load a `.toon` file from disk within [`TOON_MAX_FILE_BYTES`] and deserialize it into `T`.
pub fn load_toon_file_typed<T: DeserializeOwned>(path: &Path) -> Result<T, FormatError> {
    let content = read_bounded_toon_file(path)?;
    parse_toon_typed(&content)
}

fn read_bounded_toon_file(path: &Path) -> Result<String, FormatError> {
    let file = File::open(path)?;
    if file.metadata()?.len() > TOON_MAX_FILE_BYTES as u64 {
        return Err(FormatError::ConfigValidation(format!(
            "TOON file exceeds the {} byte limit",
            TOON_MAX_FILE_BYTES
        )));
    }
    let mut raw = Vec::new();
    file.take(TOON_MAX_FILE_BYTES as u64 + 1)
        .read_to_end(&mut raw)?;
    if raw.len() > TOON_MAX_FILE_BYTES {
        return Err(FormatError::ConfigValidation(format!(
            "TOON file exceeds the {} byte limit",
            TOON_MAX_FILE_BYTES
        )));
    }
    String::from_utf8(raw).map_err(|err| {
        FormatError::ConfigValidation(format!("TOON file is not valid UTF-8: {err}"))
    })
}
