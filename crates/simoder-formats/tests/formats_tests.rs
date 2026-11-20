//! Integration tests for `simoder-formats`: DBPF/RefPack, PROP, ERZ, `serde_toon` and config codecs.

use std::fs;

use simoder_core::{LogLevel, Tgi};
use simoder_formats::{
    Format, FormatError, Kind, LoaderConfig, PropType, PropertyFile, Value, build_resource_overlay,
    default_config_text, erz_parse_summary, load_or_create_config, mod_locale_table_id, parse_toon,
    probe, refpack_decompress,
};

#[test]
fn builds_valid_dbpf_overlay_and_probes_signature() {
    let target = Tgi {
        type_id: 0x00B1_B104,
        group: 0x0987_8A01,
        instance: 0x1234_5678,
    };
    let bytes = build_resource_overlay(&[(target, vec![0, 0, 0, 0])]).unwrap();
    assert_eq!(probe(&bytes).unwrap(), Format::Dbpf);
}

#[test]
fn refpack_decodes_literal_stream_and_rejects_truncated_input() {
    let compressed: [u8; 11] = [
        0x10, 0xFB, 0x00, 0x00, 0x04, // Header: decompressed size = 4
        0xE0, 0x11, 0x22, 0x33, 0x44, // 4 literal bytes (0xE0 -> ((0xE0 & 0x1F) + 1) * 4 = 4)
        0xFC, // Stop opcode with 0 trailing literals
    ];
    let decoded = refpack_decompress(&compressed, 4).unwrap();
    assert_eq!(decoded, vec![0x11, 0x22, 0x33, 0x44]);

    assert!(refpack_decompress(&compressed[..7], 4).is_err());
}

#[test]
fn prop_file_parses_modifies_and_reencodes_canonically() {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&2_u32.to_be_bytes());
    // Property 1: float 0x09AE19D7 = 300.0
    bytes.extend_from_slice(&0x09AE_19D7_u32.to_be_bytes());
    bytes.extend_from_slice(&PropType::Float.file_type().to_be_bytes());
    bytes.extend_from_slice(&0_u16.to_be_bytes());
    bytes.extend_from_slice(&300.0_f32.to_be_bytes());
    // Property 2: uint32 0xDEADBEEF = 0x12345678
    bytes.extend_from_slice(&0xDEAD_BEEF_u32.to_be_bytes());
    bytes.extend_from_slice(&PropType::UInt32.file_type().to_be_bytes());
    bytes.extend_from_slice(&0_u16.to_be_bytes());
    bytes.extend_from_slice(&0x1234_5678_u32.to_be_bytes());

    let mut prop = PropertyFile::parse(&bytes).unwrap();
    assert_eq!(prop.claimed_count, 2);
    assert_eq!(
        prop.get(0x09AE_19D7).map(|p| &p.kind),
        Some(&Kind::Scalar(Value::Float(300.0)))
    );

    if let Some(entry) = prop.values.iter_mut().find(|p| p.hash == 0x09AE_19D7) {
        entry.kind = Kind::Scalar(Value::Float(345.0));
    }
    let encoded = prop.encode_canonical().unwrap();
    let reparsed = PropertyFile::parse(&encoded).unwrap();
    assert_eq!(
        reparsed.get(0x09AE_19D7).map(|p| &p.kind),
        Some(&Kind::Scalar(Value::Float(345.0)))
    );
}

#[test]
fn erz_parses_minimal_layout_14_fixture() {
    let mut data = Vec::new();
    let push_u32 = |buf: &mut Vec<u8>, v: u32| buf.extend_from_slice(&v.to_be_bytes());
    push_u32(&mut data, 5); // major
    push_u32(&mut data, 2); // minor
    push_u32(&mut data, 14); // layout 14
    data.push(0); // flag
    push_u32(&mut data, 0); // rule_count
    push_u32(&mut data, 5);
    push_u32(&mut data, 0); // section A
    push_u32(&mut data, 8);
    push_u32(&mut data, 0); // section B
    push_u32(&mut data, 2);
    push_u32(&mut data, 0); // section C
    push_u32(&mut data, 1); // 1 constant
    push_u32(&mut data, 0xDEAD_BEEF);
    push_u32(&mut data, 100_000);
    push_u32(&mut data, 0); // D section
    push_u32(&mut data, 0); // E section
    let pool = b"SC_RULE_TEST\0";
    push_u32(&mut data, pool.len() as u32);
    data.extend_from_slice(pool);

    let summary = erz_parse_summary(&data).unwrap();
    assert_eq!((summary.major, summary.minor, summary.layout), (5, 2, 14));
    assert!(summary.exact);
    assert_eq!(summary.constants, vec![(0xDEAD_BEEF, 100_000)]);
    assert_eq!(summary.strings, vec!["SC_RULE_TEST"]);
}

#[test]
fn toon_parses_mod_manifest_and_rejects_malformed_inputs_via_serde_toon() {
    let source = "\
id: dawid.better-sanitizer
name: Better Sanitizer
version: 1.0.0
developerConsole:
  enabled: false
patches[1]:
  - target:
      type: 0x00B1B104
      group: 0x61EFC000
      instance: 0x719436BD
    properties[1]{id,type,operation,value}:
      0x09AE19D7,float,set,345
";
    let doc = parse_toon(source).unwrap();
    let root = doc.as_object().unwrap();
    assert_eq!(root.get("id").and_then(|v| v.as_str()), Some("dawid.better-sanitizer"));
    assert_eq!(
        root.get("developerConsole")
            .and_then(|v| v.as_object())
            .and_then(|c| c.get("enabled"))
            .and_then(|v| v.as_bool()),
        Some(false)
    );
    let patches = root.get("patches").and_then(|v| v.as_array()).unwrap();
    assert_eq!(patches.len(), 1);
    let patch_obj = patches[0].as_object().unwrap();
    assert_eq!(
        patch_obj
            .get("target")
            .and_then(|t| t.as_object())
            .and_then(|t| t.get("type"))
            .and_then(|v| v.as_str()),
        Some("0x00B1B104")
    );
    let props = patch_obj.get("properties").and_then(|v| v.as_array()).unwrap();
    assert_eq!(
        props[0].as_object().and_then(|p| p.get("value")).and_then(|v| v.as_i64()),
        Some(345)
    );

    let err_len = parse_toon("items[2]: one\n").unwrap_err();
    assert!(err_len.to_string().contains("line 1"));

    let err_indent = parse_toon("root:\n   child: bad-indent\n").unwrap_err();
    assert!(err_indent.to_string().contains("line 2"));

    let err_dup = parse_toon("key: one\nkey: two\n").unwrap_err();
    assert!(err_dup.to_string().contains("line 2"));
}

#[test]
fn derives_deterministic_mod_locale_table_id() {
    assert_eq!(mod_locale_table_id("openscp"), 0xBF82_F7CE);
    assert_ne!(mod_locale_table_id("road-builder"), 0x6C96_9DEE);
}

#[test]
fn creates_loads_and_validates_config_toon_and_toml() {
    assert!(LoaderConfig::from_toon(&default_config_text()).is_ok());

    let dir = tempfile::tempdir().unwrap();
    let cfg = load_or_create_config(dir.path()).unwrap();
    assert!(!cfg.developer_console.enabled);
    assert!(cfg.developer_console.capture_game_logs);
    assert_eq!(cfg.developer_console.level, LogLevel::Info);
    assert!(dir.path().join("config.toon").is_file());

    fs::write(
        dir.path().join("config.toon"),
        "developerConsole:\n  enabled: true\n  captureGameLogs: false\n  captureLoaderLogs: true\n  captureModLogs: true\n  level: debug\n",
    )
    .unwrap();
    let updated = load_or_create_config(dir.path()).unwrap();
    assert!(updated.developer_console.enabled);
    assert!(!updated.developer_console.capture_game_logs);
    assert_eq!(updated.developer_console.level, LogLevel::Debug);

    fs::write(
        dir.path().join("config.toon"),
        "developerConsole:\n  enabled: yes\n",
    )
    .unwrap();
    assert!(matches!(
        load_or_create_config(dir.path()),
        Err(FormatError::Toon(_))
    ));

    fs::write(
        dir.path().join("config.toon"),
        "developerConsole:\n  level: verbose\n",
    )
    .unwrap();
    assert!(matches!(
        load_or_create_config(dir.path()),
        Err(FormatError::Toon(_))
    ));

    fs::write(
        dir.path().join("config.toon"),
        "developerConsole:\n  level: patch\n",
    )
    .unwrap();
    assert!(matches!(
        load_or_create_config(dir.path()),
        Err(FormatError::ConfigValidation(_))
    ));

    let toml_cfg = LoaderConfig::from_toml("default_locale = \"pl-PL\"\ndev_mode = true\n").unwrap();
    assert!(toml_cfg.enabled);
    assert_eq!(toml_cfg.default_locale, "pl-PL");
    assert!(toml_cfg.dev_mode);
    assert_eq!(toml_cfg.mods_dir, "mods");
}
