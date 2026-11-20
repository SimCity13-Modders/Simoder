//! Integration tests for `simoder-core` primitives: TGI, FNV-1, SHA-256, filter, runtime PROP table, signature scanner, and logging.

use simoder_core::{
    CoreError, Digest, LogLevel, LogSourceType, ResourceFilter, ResourceId,
    RuntimeFloatPatchStatus, RuntimePropertyRecord, RuntimeTableStatus, SCALAR_FLOAT_METADATA,
    Sha256, Signature, Tgi, UniqueSignatureStatus, fnv1_name_hash, hex, parse_tgi,
    patch_runtime_float, read_runtime_float, tgi_key, validate_runtime_property_table,
};

#[test]
fn tgi_parses_and_uses_dbpf_resource_id_display() {
    let tgi: Tgi = parse_tgi("0x00B1B104:0x40404000:00000001").unwrap();
    assert_eq!(
        tgi,
        ResourceId {
            type_id: 0x00B1_B104,
            group: 0x4040_4000,
            instance: 1,
        }
    );
    assert_eq!(tgi.to_string(), "00B1B104:40404000:00000001");
    assert_eq!(tgi_key(tgi), (0x00B1_B104, 0x4040_4000, 1));
}

#[test]
fn tgi_rejects_malformed_inputs() {
    assert!(matches!(
        parse_tgi("00B1B104:40404000"),
        Err(CoreError::InvalidTgiFormat(_))
    ));
    assert!(matches!(
        parse_tgi("ZZZZ:40404000:1"),
        Err(CoreError::InvalidHexComponent(_))
    ));
}

#[test]
fn resource_filter_matches_exact_and_partial_subsets() {
    let target = Tgi {
        type_id: 0x00B1_B104,
        group: 0x61EF_C000,
        instance: 0x7194_36BD,
    };
    let exact = ResourceFilter::exact(target);
    assert!(exact.matches(target));
    assert!(!exact.matches(Tgi {
        instance: 0x1111_1111,
        ..target
    }));

    let group_only = ResourceFilter::new(None, Some(target.group), None);
    assert!(group_only.matches(target));
    assert!(!group_only.matches(Tgi {
        group: 0x2222_2222,
        ..target
    }));
}

#[test]
fn runtime_property_table_validates_and_patches_floats() {
    let source = [
        RuntimePropertyRecord {
            id: 0x0100_0000,
            value_bits: 1.0_f32.to_bits(),
            auxiliary: [0; 3],
            metadata: SCALAR_FLOAT_METADATA,
        },
        RuntimePropertyRecord {
            id: 0x09AE_19D7,
            value_bits: 300.0_f32.to_bits(),
            auxiliary: [0; 3],
            metadata: SCALAR_FLOAT_METADATA,
        },
        RuntimePropertyRecord {
            id: 0x0AFB_9882,
            value_bits: 72.0_f32.to_bits(),
            auxiliary: [0; 3],
            metadata: SCALAR_FLOAT_METADATA,
        },
        RuntimePropertyRecord {
            id: 0x0FD1_6C15,
            value_bits: 200.0_f32.to_bits(),
            auxiliary: [0; 3],
            metadata: SCALAR_FLOAT_METADATA,
        },
    ];

    assert_eq!(
        validate_runtime_property_table(&source, source.len()),
        RuntimeTableStatus::Valid
    );
    assert_eq!(read_runtime_float(&source, 0x09AE_19D7), Some(300.0));

    let mut owned = source;
    assert_eq!(
        patch_runtime_float(&mut owned, source.len(), 0x09AE_19D7, 300.0, 345.0),
        RuntimeFloatPatchStatus::Applied
    );
    assert_eq!(read_runtime_float(&source, 0x09AE_19D7), Some(300.0));
    assert_eq!(read_runtime_float(&owned, 0x09AE_19D7), Some(345.0));
    assert_eq!(
        patch_runtime_float(&mut owned, source.len(), 0x09AE_19D7, 300.0, 345.0),
        RuntimeFloatPatchStatus::AlreadyApplied
    );

    let mut wrong_expected = source;
    assert_eq!(
        patch_runtime_float(&mut wrong_expected, source.len(), 0x09AE_19D7, 301.0, 345.0),
        RuntimeFloatPatchStatus::UnexpectedValue
    );
    assert_eq!(
        validate_runtime_property_table(&source, source.len() + 1),
        RuntimeTableStatus::WrongCount
    );
    assert_eq!(
        patch_runtime_float(&mut wrong_expected, source.len(), 0x1111_1111, 0.0, 1.0),
        RuntimeFloatPatchStatus::PropertyMissing
    );

    let mut wrong_type = source;
    wrong_type[1].metadata = 0x000C_0000;
    assert_eq!(
        patch_runtime_float(&mut wrong_type, source.len(), 0x09AE_19D7, 300.0, 345.0),
        RuntimeFloatPatchStatus::PropertyNotFloat
    );

    let mut unsorted = source;
    unsorted.swap(1, 2);
    assert_eq!(
        validate_runtime_property_table(&unsorted, unsorted.len()),
        RuntimeTableStatus::NotStrictlySorted
    );
}

#[test]
fn signature_scanner_handles_wildcards_ambiguity_and_unique_matches() {
    let sig = Signature::parse("8B ?? 89").unwrap();
    let bytes = [0x8B_u8, 0x01, 0x89, 0x90, 0x8B, 0xFF, 0x89];

    assert_eq!(sig.find_all(&bytes), vec![0, 4]);
    let ambiguous = sig.find_unique(&bytes);
    assert_eq!(ambiguous.status, UniqueSignatureStatus::Ambiguous);
    assert_eq!(ambiguous.match_count, 2);

    let absent = Signature::parse("DE AD BE EF").unwrap();
    assert!(absent.find_all(&bytes).is_empty());
    let not_found = absent.find_unique(&bytes);
    assert_eq!(not_found.status, UniqueSignatureStatus::NotFound);
    assert_eq!(not_found.match_count, 0);

    let unique = sig.find_unique(&bytes[..3]);
    assert_eq!(unique.status, UniqueSignatureStatus::Matched);
    assert_eq!(unique.offset, 0);
    assert_eq!(unique.match_count, 1);

    assert!(matches!(
        Signature::parse(""),
        Err(CoreError::InvalidSignature(_))
    ));
    assert!(matches!(
        Signature::parse("8B Z"),
        Err(CoreError::InvalidSignature(_))
    ));
}

#[test]
fn logging_labels_colors_and_thresholds_match_contract() {
    assert_eq!(LogSourceType::Game.as_str(), "GAME");
    assert_eq!(LogSourceType::Loader.as_str(), "LOADER");
    assert_eq!(LogSourceType::Mod.as_str(), "MOD");

    assert_eq!(LogLevel::parse("debug"), Some(LogLevel::Debug));
    assert_eq!(LogLevel::parse("Debug"), Some(LogLevel::Debug));
    assert_eq!(LogLevel::parse("warn"), Some(LogLevel::Warning));
    assert_eq!(LogLevel::parse("WARN"), Some(LogLevel::Warning));
    assert_eq!(LogLevel::parse("Warning"), Some(LogLevel::Warning));
    assert_eq!(LogLevel::parse("patch"), None);
    assert_eq!(LogLevel::parse("verbose"), None);
    assert!(LogLevel::Warning.passes_minimum(LogLevel::Info));
    assert!(LogLevel::Patch.passes_minimum(LogLevel::Info));
    assert!(!LogLevel::Trace.passes_minimum(LogLevel::Info));

    assert_eq!(LogLevel::Error.colored_label(), "\x1b[1m\x1b[31mERROR\x1b[0m");
    assert_eq!(LogLevel::Warning.colored_label(), "\x1b[1m\x1b[33mWARN\x1b[0m");
    assert_eq!(LogLevel::Info.colored_label(), "\x1b[32mINFO\x1b[0m");
    assert_eq!(LogLevel::Debug.colored_label(), "\x1b[36mDEBUG\x1b[0m");
    assert_eq!(LogLevel::Patch.colored_label(), "\x1b[35mPATCH\x1b[0m");
    assert_eq!(LogLevel::Trace.colored_label(), "\x1b[37mTRACE\x1b[0m");

    assert_eq!(
        LogLevel::Error.format_event(LogSourceType::Loader, "boom", false),
        "[LOADER][ERROR] boom"
    );
    assert_eq!(
        LogLevel::Error.format_event(LogSourceType::Loader, "boom", true),
        "[LOADER][\x1b[1m\x1b[31mERROR\x1b[0m] boom"
    );
}

#[test]
fn fnv1_matches_verified_openscp_table_id() {
    assert_eq!(fnv1_name_hash("openscp/strings"), 0xBF82_F7CE);
    assert_eq!(fnv1_name_hash("OpenSCP/Strings"), 0xBF82_F7CE);
}

#[test]
fn sha256_reexport_matches_standard_vector() {
    let digest_hex = hex::encode(Sha256::digest(b""));
    assert_eq!(
        digest_hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
}
