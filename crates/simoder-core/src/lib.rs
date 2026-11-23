#![forbid(unsafe_code)]

//! Core identifiers, hashing re-exports, filters, runtime PROP table helpers, signature scanner, and logging primitives for Simoder.

pub mod filter;
pub mod hash;
pub mod logging;
pub mod redaction;
pub mod runtime_table;
pub mod signature;

pub use dbpf::ResourceId;
pub use dbpf::ResourceId as Tgi;
pub use {filter::*, hash::*, logging::*, redaction::*, runtime_table::*, signature::*};
pub use hex;
pub use sha2::{Digest, Sha256};

/// Errors returned when parsing or validating core Simoder primitives.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum CoreError {
    /// Input TGI string did not contain three colon-separated components.
    #[error("invalid TGI format '{0}'; expected TYPE:GROUP:INSTANCE")]
    InvalidTgiFormat(String),
    /// A TGI component could not be parsed as a 32-bit hexadecimal integer.
    #[error("invalid hexadecimal TGI component '{0}'")]
    InvalidHexComponent(String),
    /// An IDA-style byte signature pattern failed lexical or length validation.
    #[error("invalid signature pattern: {0}")]
    InvalidSignature(String),
}

/// Parse a Maxis DBPF / SimCityPak `TYPE:GROUP:INSTANCE` string into a [`Tgi`] ([`dbpf::ResourceId`]).
///
/// Formatting (`Display`) is reused directly from [`dbpf::ResourceId`] (`"{:08X}:{:08X}:{:08X}"`)
/// to avoid DRY duplication. Each component may optionally include a `0x` or `0X` hexadecimal prefix.
pub fn parse_tgi(raw: &str) -> Result<Tgi, CoreError> {
    let mut parts = raw.split(':');
    let (Some(t), Some(g), Some(i), None) =
        (parts.next(), parts.next(), parts.next(), parts.next())
    else {
        return Err(CoreError::InvalidTgiFormat(raw.to_string()));
    };
    Ok(Tgi {
        type_id: parse_hex_u32(t)?,
        group: parse_hex_u32(g)?,
        instance: parse_hex_u32(i)?,
    })
}

/// Return a deterministic `(type_id, group, instance)` sorting tuple for [`Tgi`] ([`dbpf::ResourceId`]).
pub const fn tgi_key(id: Tgi) -> (u32, u32, u32) {
    (id.type_id, id.group, id.instance)
}

/// Parse one 32-bit hexadecimal TGI field with optional `0x` / `0X` prefix.
fn parse_hex_u32(raw: &str) -> Result<u32, CoreError> {
    let trimmed = raw.trim();
    let hex_digits = trimmed
        .strip_prefix("0x")
        .or_else(|| trimmed.strip_prefix("0X"))
        .unwrap_or(trimmed);
    if hex_digits.is_empty()
        || hex_digits.len() > 8
        || !hex_digits.bytes().all(|b| b.is_ascii_hexdigit())
    {
        return Err(CoreError::InvalidHexComponent(raw.to_string()));
    }
    u32::from_str_radix(hex_digits, 16)
        .map_err(|_| CoreError::InvalidHexComponent(raw.to_string()))
}
