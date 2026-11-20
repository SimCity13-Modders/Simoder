//! Verified 24-byte SimCity runtime PROP vector validation and safe scalar-float patching.

/// Metadata word encoding scalar PROP float type `0x000D` in SimCity's parsed runtime PROP vector.
///
/// Upper 16 bits hold `sc_properties::PropType::Float` file type ID `13` (`0x000D`), while lower
/// 16 bits hold scalar flags `0x0000` (matching verified C++ `kScalarFloatMetadata = 0x000D0000`).
pub const SCALAR_FLOAT_METADATA: u32 = 0x000D_0000;

/// Fixed byte width of one entry in SimCity's parsed in-memory runtime PROP vector.
///
/// Verified against retail `SimCity.exe` 32-bit structs (`id` + `value_bits` + 3 auxiliary `u32` words + `metadata`).
pub const RUNTIME_PROPERTY_RECORD_BYTES: usize = 24;

/// One verified 24-byte entry in SimCity's parsed in-memory PROP vector.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct RuntimePropertyRecord {
    /// Property FNV-1 identifier hash (entries must be strictly ascending by `id`).
    pub id: u32,
    /// Raw 32-bit scalar payload bits (IEEE-754 `f32` bit pattern when `metadata == SCALAR_FLOAT_METADATA`).
    pub value_bits: u32,
    /// Auxiliary runtime bookkeeping fields internal to the game engine.
    pub auxiliary: [u32; 3],
    /// Encoded PROP type and flags word (see [`SCALAR_FLOAT_METADATA`]).
    pub metadata: u32,
}

const _: () = assert!(std::mem::size_of::<RuntimePropertyRecord>() == RUNTIME_PROPERTY_RECORD_BYTES);

/// Structural validation result for a parsed runtime PROP vector.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RuntimeTableStatus {
    /// Entry count matches and all property IDs are strictly sorted ascending.
    Valid,
    /// Slice length differs from the container header count.
    WrongCount,
    /// Adjacent entries are out of order or contain duplicate IDs.
    NotStrictlySorted,
}

/// Outcome of attempting a bounded scalar-float edit in a runtime PROP table.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RuntimeFloatPatchStatus {
    /// Target value matched `expected_value` and was updated to `replacement_value`.
    Applied,
    /// Target value already held `replacement_value`.
    AlreadyApplied,
    /// Table failed count or strict-sorting validation (see [`validate_runtime_property_table`]).
    InvalidTable,
    /// Requested `identifier` was not present in the runtime property vector.
    PropertyMissing,
    /// Entry metadata did not match [`SCALAR_FLOAT_METADATA`].
    PropertyNotFloat,
    /// Existing float bit pattern matched neither `expected_value` nor `replacement_value`.
    UnexpectedValue,
}

/// Validate that `properties` has length `expected_count` and strictly ascending property IDs.
pub fn validate_runtime_property_table(
    properties: &[RuntimePropertyRecord],
    expected_count: usize,
) -> RuntimeTableStatus {
    if properties.len() != expected_count {
        return RuntimeTableStatus::WrongCount;
    }
    if !properties.is_sorted_by(|a, b| a.id < b.id) {
        return RuntimeTableStatus::NotStrictlySorted;
    }
    RuntimeTableStatus::Valid
}

/// Locate a unique entry by `identifier` in a strictly sorted [`RuntimePropertyRecord`] slice via binary search.
pub fn find_runtime_property(
    properties: &[RuntimePropertyRecord],
    identifier: u32,
) -> Option<&RuntimePropertyRecord> {
    let idx = properties.binary_search_by_key(&identifier, |prop| prop.id).ok()?;
    properties.get(idx)
}

/// Decode a scalar IEEE-754 `f32` value when `identifier` exists and has [`SCALAR_FLOAT_METADATA`].
pub fn read_runtime_float(properties: &[RuntimePropertyRecord], identifier: u32) -> Option<f32> {
    let property = find_runtime_property(properties, identifier)?;
    if property.metadata != SCALAR_FLOAT_METADATA {
        return None;
    }
    Some(f32::from_bits(property.value_bits))
}

/// Atomically verify table validity, scalar float metadata, and `expected_value` bit pattern before
/// writing `replacement_value` into the matching [`RuntimePropertyRecord`].
pub fn patch_runtime_float(
    properties: &mut [RuntimePropertyRecord],
    expected_count: usize,
    identifier: u32,
    expected_value: f32,
    replacement_value: f32,
) -> RuntimeFloatPatchStatus {
    if validate_runtime_property_table(properties, expected_count) != RuntimeTableStatus::Valid {
        return RuntimeFloatPatchStatus::InvalidTable;
    }
    let Ok(index) = properties.binary_search_by_key(&identifier, |prop| prop.id) else {
        return RuntimeFloatPatchStatus::PropertyMissing;
    };
    let property = &mut properties[index];
    if property.metadata != SCALAR_FLOAT_METADATA {
        return RuntimeFloatPatchStatus::PropertyNotFloat;
    }
    let expected_bits = expected_value.to_bits();
    let replacement_bits = replacement_value.to_bits();
    if property.value_bits == replacement_bits {
        return RuntimeFloatPatchStatus::AlreadyApplied;
    }
    if property.value_bits != expected_bits {
        return RuntimeFloatPatchStatus::UnexpectedValue;
    }
    property.value_bits = replacement_bits;
    RuntimeFloatPatchStatus::Applied
}
