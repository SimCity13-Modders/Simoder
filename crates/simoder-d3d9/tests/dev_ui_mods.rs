use std::collections::BTreeSet;
use std::fs;

use serde_json::json;
use simoder_d3d9::{
    discover_dev_ui_mods, load_dev_ui_keybind_manifest, load_dev_ui_tool_manifest,
    read_dev_ui_script_with_sha256, register_dev_ui_keybinds, register_dev_ui_tools,
};
use simoder_mods::{
    HostToolRegistry, KeybindRegistry, NativeTrustDecision, NativeTrustStore, SdResult,
};
use tempfile::tempdir;

#[test]
fn discovers_dev_ui_mod_and_registers_and_revokes_tools() {
    let dir = tempdir().unwrap();
    let mod_dir = dir.path().join("simoder.ui_inspector");
    let dev_ui = mod_dir.join("dev_ui");
    fs::create_dir_all(&dev_ui).unwrap();

    fs::write(
        mod_dir.join("mod.toml"),
        r#"schemaVersion = 2
format = "simoder.authoring"

[metadata]
id = "simoder.ui_inspector"
name = "Simoder UI Inspector"
version = "0.1.0"
author = "Simoder"

[scripts]
language = "js"
entry = "dev_ui/inspector.js"
"#,
    )
    .unwrap();
    fs::write(dev_ui.join("inspector.js"), "(function(){return '{}';})").unwrap();
    let tools_json = r#"[
  {
    "name": "get_selection",
    "op": "get_selection",
    "description": "Read inspected element",
    "inputSchema": {"type": "object", "properties": {}},
    "requiredGrants": ["session.read"],
    "annotations": {
      "destructiveHint": false,
      "idempotentHint": true,
      "openWorldHint": false,
      "readOnlyHint": true
    }
  }
]"#;
    fs::write(dev_ui.join("mcp_tools.json"), tools_json).unwrap();

    let mods = discover_dev_ui_mods(dir.path());
    assert_eq!(mods.len(), 1);
    assert_eq!(mods[0].mod_id, "simoder.ui_inspector");
    assert!(mods[0].tools_path.is_some());

    let entries = load_dev_ui_tool_manifest(tools_json).unwrap();
    let mut reg = HostToolRegistry::new();
    let registered = register_dev_ui_tools(&mut reg, &mods[0].mod_id, 1, &entries, |op, _| {
        Ok(json!({ "op": op, "ok": true }))
    })
    .unwrap();
    assert_eq!(registered, 1);

    let list = reg.list_tools();
    assert_eq!(list.len(), 1);
    assert_eq!(list[0].name, "simoder.ui_inspector.get_selection");

    let grants: BTreeSet<String> = ["session.read".to_string()].into_iter().collect();
    let out = reg
        .invoke_tool("simoder.ui_inspector.get_selection", &json!({}), &grants)
        .unwrap();
    assert_eq!(out["op"], "get_selection");

    assert_eq!(reg.revoke_mod_generation("simoder.ui_inspector", 1), 1);
    assert!(reg.list_tools().is_empty());
    assert_eq!(
        reg.invoke_tool("simoder.ui_inspector.get_selection", &json!({}), &grants),
        Err(SdResult::Invalid)
    );
}

#[test]
fn rejects_path_traversal_and_invalid_tool_manifests() {
    let dir = tempdir().unwrap();
    let mod_dir = dir.path().join("bad.mod");
    fs::create_dir_all(mod_dir.join("dev_ui")).unwrap();
    fs::write(
        mod_dir.join("mod.toml"),
        r#"schemaVersion = 2
format = "simoder.authoring"

[metadata]
id = "bad.mod"
name = "Bad"
version = "0.1.0"
author = "Test"

[scripts]
language = "js"
entry = "dev_ui/../escape.js"
"#,
    )
    .unwrap();
    assert!(discover_dev_ui_mods(dir.path()).is_empty());
    assert!(load_dev_ui_tool_manifest(r#"[{"name":"","op":"x","description":"y","inputSchema":{},"annotations":{"destructiveHint":false,"idempotentHint":true,"openWorldHint":false,"readOnlyHint":true}}]"#).is_err());
}

#[test]
fn discovers_and_registers_dev_ui_keybinds() {
    let dir = tempdir().unwrap();
    let mod_dir = dir.path().join("simoder.ui_inspector");
    let dev_ui = mod_dir.join("dev_ui");
    fs::create_dir_all(&dev_ui).unwrap();
    fs::write(
        mod_dir.join("mod.toml"),
        r#"schemaVersion = 2
format = "simoder.authoring"

[metadata]
id = "simoder.ui_inspector"
name = "UI Inspector"
version = "0.1.0"
author = "Simoder"

[scripts]
language = "js"
entry = "dev_ui/inspector.js"
"#,
    )
    .unwrap();
    fs::write(dev_ui.join("inspector.js"), "(function(){return '{}';})").unwrap();
    let kb_json = r#"[
  {
    "id": "toggle_panel",
    "title": "Toggle Panel",
    "description": "Toggle inspector overlay",
    "context": "global",
    "defaultKey": "Ctrl+Alt+I"
  }
]"#;
    fs::write(dev_ui.join("keybinds.json"), kb_json).unwrap();

    let mods = discover_dev_ui_mods(dir.path());
    assert_eq!(mods.len(), 1);
    assert!(mods[0].keybinds_path.is_some());

    let entries = load_dev_ui_keybind_manifest(kb_json).unwrap();
    let mut reg = KeybindRegistry::new();
    let count = register_dev_ui_keybinds(&mut reg, &mods[0].mod_id, &entries).unwrap();
    assert_eq!(count, 1);
    let map = reg.effective_map_for_mod("simoder.ui_inspector");
    assert_eq!(map.get("toggle_panel").map(String::as_str), Some("Ctrl+Alt+I"));
}

#[test]
fn dev_ui_script_sha256_pins_and_invalidates_native_trust_store() {
    let dir = tempdir().unwrap();
    let script_path = dir.path().join("inspector.js");
    fs::write(&script_path, "(function(){ return 'v1'; })").unwrap();

    let (script, sha_v1) = read_dev_ui_script_with_sha256(&script_path).unwrap();
    assert!(script.contains("v1"));
    assert_eq!(sha_v1.len(), 64);

    let trust_path = dir.path().join("native_trust.toon");
    let mut store = NativeTrustStore::new();
    store
        .set_decision(
            "simoder.ui_inspector",
            "dev_ui/inspector.js",
            &sha_v1,
            NativeTrustDecision::Approved,
        )
        .unwrap();
    store.save(&trust_path).unwrap();

    let loaded = NativeTrustStore::load(&trust_path).unwrap();
    assert_eq!(
        loaded.decision_for("simoder.ui_inspector", &sha_v1),
        Some(NativeTrustDecision::Approved)
    );

    // Modifying the script invalidates the old SHA-256 approval.
    fs::write(&script_path, "(function(){ return 'v2'; })").unwrap();
    let (_script_v2, sha_v2) = read_dev_ui_script_with_sha256(&script_path).unwrap();
    assert_ne!(sha_v1, sha_v2);
    assert_eq!(loaded.decision_for("simoder.ui_inspector", &sha_v2), None);
}

#[test]
fn mods_ui_snapshot_separates_native_and_dev_ui_candidates() {
    use simoder_d3d9::{IntegratedSimoderRuntime, NativeModCandidateState};
    use simoder_mods::{NativeModStatus, NativeModUiRecord};

    let rt = IntegratedSimoderRuntime::new();
    rt.register_native_candidate(NativeModCandidateState {
        record: NativeModUiRecord {
            mod_id: "simoder.devtools".into(),
            name: "MCP".into(),
            version: "0.2.0".into(),
            entry: "simoder_mcp.dll".into(),
            sha256: "a".repeat(64),
            status: NativeModStatus::Active,
            diagnostic: None,
        },
        dll_path: std::path::PathBuf::from("simoder_mcp.dll"),
        trust_path: std::path::PathBuf::from("native_trust.toon"),
        source_size_bytes: 100,
        mapped_image_bytes: 4096,
        owner: None,
    });
    rt.register_dev_ui_candidate(NativeModCandidateState {
        record: NativeModUiRecord {
            mod_id: "simoder.ui_inspector".into(),
            name: "UI Inspector".into(),
            version: "0.1.0".into(),
            entry: "dev_ui/inspector.js".into(),
            sha256: "b".repeat(64),
            status: NativeModStatus::PendingApproval,
            diagnostic: None,
        },
        dll_path: std::path::PathBuf::from("dev_ui/inspector.js"),
        trust_path: std::path::PathBuf::from("native_trust.toon"),
        source_size_bytes: 100,
        mapped_image_bytes: 0,
        owner: None,
    });

    let snap = rt.mods_ui_snapshot();
    assert_eq!(snap.native_mods.len(), 1);
    assert_eq!(snap.native_mods[0].mod_id, "simoder.devtools");
    assert_eq!(snap.dev_ui_mods.len(), 1);
    assert_eq!(snap.dev_ui_mods[0].mod_id, "simoder.ui_inspector");
}
