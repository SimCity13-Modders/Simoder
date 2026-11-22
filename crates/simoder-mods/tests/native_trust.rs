//! Integration tests for `[native]` / `[scripts]` manifest parsing and `NativeTrustStore` (`native_trust.toon`).

use std::fs;

use simoder_core::hex;
use simoder_mods::{
    ModError, NativeTrustDecision, NativeTrustStore, load_base_mod_definition, load_mod_definition,
    sha256_bytes,
};

#[test]
fn native_and_script_manifests_validate_paths_extensions_and_aliases() {
    let dir = tempfile::tempdir().unwrap();

    // 1. Generic native mod with `[native] entry = "custom_addon.dll"`
    let addon_dir = dir.path().join("custom_addon");
    fs::create_dir_all(&addon_dir).unwrap();
    let dll_bytes = b"fake-pe32-payload";
    let sha = hex::encode(sha256_bytes(dll_bytes));
    fs::write(addon_dir.join("custom_addon.dll"), dll_bytes).unwrap();
    fs::write(
        addon_dir.join("mod.toml"),
        format!(
            "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
             [metadata]\nid = \"dawid.native-addon\"\nname = \"Native Addon\"\nversion = \"1.0.0\"\n\n\
             [native]\nentry = \"custom_addon.dll\"\nabi = 2\nsha256 = \"{sha}\"\n"
        ),
    )
    .unwrap();
    let def = load_mod_definition(&addon_dir).unwrap();
    let native = def.native().expect("[native] block should be parsed");
    assert_eq!(native.entry, "custom_addon.dll");
    assert_eq!(native.abi, 2);
    assert_eq!(native.expected_sha256.as_deref(), Some(sha.as_str()));

    // 2. Also accept `format = "simoder.native-devtool"` + `[native] module = ...`, `contractVersion = 2`
    fs::write(
        addon_dir.join("mod.toml"),
        format!(
            "schemaVersion = 2\nformat = \"simoder.native-devtool\"\nkind = \"developer-mod\"\nhostAbiVersion = 2\n\n\
             [metadata]\nid = \"simoder.devtools\"\nname = \"DevTools\"\nversion = \"0.2.0\"\n\n\
             [native]\nmodule = \"custom_addon.dll\"\ncontractVersion = 2\nsha256 = \"{sha}\"\n"
        ),
    )
    .unwrap();
    let devtools_def = load_mod_definition(&addon_dir).unwrap();
    assert_eq!(devtools_def.native().unwrap().entry, "custom_addon.dll");

    // 3. Reject non-.dll native entry
    fs::write(addon_dir.join("not_a_dll.exe"), dll_bytes).unwrap();
    fs::write(
        addon_dir.join("mod.toml"),
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"dawid.native-addon\"\nname = \"Native Addon\"\nversion = \"1.0.0\"\n\n\
         [native]\nentry = \"not_a_dll.exe\"\nabi = 2\n",
    )
    .unwrap();
    assert!(matches!(
        load_mod_definition(&addon_dir),
        Err(ModError::ManifestValidation(_))
    ));

    // 4. Reject path traversal in native entry
    fs::write(
        addon_dir.join("mod.toml"),
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"dawid.native-addon\"\nname = \"Native Addon\"\nversion = \"1.0.0\"\n\n\
         [native]\nentry = \"../escape.dll\"\nabi = 2\n",
    )
    .unwrap();
    assert!(matches!(
        load_mod_definition(&addon_dir),
        Err(ModError::UnsafePath(_))
    ));

    // 5. load_base_mod_definition permits `simoder.base` while load_mod_definition rejects it
    let base_dir = dir.path().join("simoder.base");
    fs::create_dir_all(base_dir.join("scripts")).unwrap();
    fs::write(base_dir.join("scripts").join("init.luau"), "return {}").unwrap();
    fs::write(
        base_dir.join("mod.toml"),
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"simoder.base\"\nname = \"Simoder Base\"\nversion = \"0.2.0\"\n\n\
         [scripts]\nlanguage = \"luau\"\nentry = \"scripts/init.luau\"\nmemoryMiB = 8\nbudgetMs = 2\n",
    )
    .unwrap();
    assert!(matches!(
        load_mod_definition(&base_dir),
        Err(ModError::ManifestValidation(_))
    ));
    let base_def = load_base_mod_definition(&base_dir).unwrap();
    assert_eq!(base_def.scripts().unwrap().language, "luau");
    assert_eq!(base_def.scripts().unwrap().memory_mib, Some(8));
}

#[test]
fn native_trust_store_persists_decisions_and_invalidates_on_sha256_change() {
    let dir = tempfile::tempdir().unwrap();
    let trust_path = dir.path().join("simoder").join("native_trust.toon");

    let empty = NativeTrustStore::load(&trust_path).unwrap();
    assert!(empty.entries().is_empty());

    let hash_v1 = "a".repeat(64);
    let hash_v2 = "b".repeat(64);

    let mut store = NativeTrustStore::new();
    store
        .set_decision(
            "dawid.custom-native",
            "addon.dll",
            &hash_v1,
            NativeTrustDecision::Approved,
        )
        .unwrap();
    store
        .set_decision(
            "dawid.untrusted",
            "evil.dll",
            &hash_v2,
            NativeTrustDecision::Denied,
        )
        .unwrap();
    store.save(&trust_path).unwrap();

    let reloaded = NativeTrustStore::load(&trust_path).unwrap();
    assert_eq!(
        reloaded.decision_for("dawid.custom-native", &hash_v1),
        Some(NativeTrustDecision::Approved)
    );
    // Changing DLL hash invalidates trust approval (`None`), requiring fresh user approval.
    assert_eq!(
        reloaded.decision_for("dawid.custom-native", &hash_v2),
        None
    );
    assert_eq!(
        reloaded.decision_for("dawid.untrusted", &hash_v2),
        Some(NativeTrustDecision::Denied)
    );
}
