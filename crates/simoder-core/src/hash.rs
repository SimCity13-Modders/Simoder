//! GlassBox 32-bit FNV-1 hashing shared across SimCity rules, locales, and UI commands.

/// IETF 32-bit FNV-1 offset basis used by `EA::StdC::FNV1_String8` and `ResourceMan::HashName32`.
///
/// Used across EcoGame ER2/ERZ rule IDs, custom `0x0A98EAF0` mod locale table IDs, and the retail UI
/// command dispatcher (`SimCity.exe` VA `0x8C73F0`).
pub const GLASSBOX_FNV1_OFFSET_BASIS: u32 = 0x811C_9DC5;

/// IETF 32-bit FNV-1 prime (`2^24 + 2^8 + 0x93`) paired with [`GLASSBOX_FNV1_OFFSET_BASIS`].
///
/// External crates such as `fnv` implement FNV-1a (XOR then multiply), whereas GlassBox requires
/// classic FNV-1 (multiply then XOR) over ASCII-lowercased bytes.
pub const GLASSBOX_FNV1_PRIME: u32 = 0x0100_0193;

/// Compute the GlassBox resource/rule 32-bit FNV-1 hash matching retail `HashName32` over ASCII-lowercased bytes.
pub fn fnv1_name_hash(name: &str) -> u32 {
    let mut hash = GLASSBOX_FNV1_OFFSET_BASIS;
    for byte in name.bytes().map(|b| b.to_ascii_lowercase()) {
        hash = hash.wrapping_mul(GLASSBOX_FNV1_PRIME) ^ u32::from(byte);
    }
    hash
}
