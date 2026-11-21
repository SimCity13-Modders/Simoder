//! Schema 2 authoring & Schema 3 raw-resources manifest parsing, PNG/GLB/material validation,
//! and vanilla-copy prevention integration tests.

mod common;

use std::borrow::Cow;
use std::collections::HashSet;
use std::fs;
use std::sync::Arc;

use simoder_core::hex;
use simoder_mods::{
    ModError, ModManager, ModState, PatchOperation, PatchRegistry, RuntimeCapability,
    audit_mod_directory_no_vanilla_copies, load_mod_definition, load_stable_candidate,
    sha256_bytes, validate_no_vanilla_copies,
};

use common::{minimal_valid_glb, minimal_valid_png, target_tgi, write_authoring_patch_mod};

#[test]
fn manifest_validates_symbol_aliases_hex_forms_and_rejects_invalid_authoring() {
    let dir = tempfile::tempdir().unwrap();
    let mod_dir = dir.path().join("alias_mod");
    fs::create_dir_all(&mod_dir).unwrap();

    fs::write(
        mod_dir.join("mod.toml"),
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"dawid.alias-mod\"\nname = \"Alias Mod\"\nversion = \"1.2.0\"\n\n\
         [[symbols.resources]]\nname = \"sanitizer\"\nsource = \"SimCity_Game.package\"\n\
         type = 11645188\ngroup = \"0x61EFC000\"\ninstance = \"0x719436BD\"\n\n\
         [[symbols.properties]]\nname = \"maintenance_cost\"\nid = \"0x09AE19D7\"\n\
         type = \"FLOAT\"\nsource = \"EcoGame\"\ndescription = \"Hourly cost\"\nunit = \"simoleons\"\norigin = \"verified\"\n\n\
         [[patches]]\n[patches.target]\nname = \"sanitizer\"\n\
         [[patches.properties]]\nname = \"maintenance_cost\"\ntype = \"Float\"\noperation = \"ADD\"\nvalue = 25\n",
    )
    .unwrap();
    let (def, _fp) = load_stable_candidate(&mod_dir).unwrap();
    let patches = def.patches();
    assert_eq!(patches.len(), 1);
    assert_eq!(patches[0].target, target_tgi(0x7194_36BD));
    assert_eq!(
        patches[0].symbol.as_ref().map(|s| s.name.as_str()),
        Some("sanitizer")
    );
    assert_eq!(patches[0].properties[0].property_id, 0x09AE_19D7);
    assert_eq!(patches[0].properties[0].operation, PatchOperation::Add);
    assert_eq!(patches[0].properties[0].value, 25.0);
    assert_eq!(
        patches[0].properties[0]
            .symbol
            .as_ref()
            .and_then(|s| s.unit.as_deref()),
        Some("simoleons")
    );

    fs::write(
        mod_dir.join("mod.toml"),
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"dawid.alias-mod\"\nname = \"Alias Mod\"\nversion = \"1.0.0\"\n\n\
         [[symbols.properties]]\nname = \"cost\"\nid = \"0x09AE19D7\"\nsource = \"EcoGame\"\n\n\
         [[patches]]\ntarget = \"00B1B104:61EFC000:719436BD\"\n\
         [[patches.properties]]\nname = \"cost\"\nid = \"0x11111111\"\ntype = \"float\"\noperation = \"set\"\nvalue = 10.0\n",
    )
    .unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::PatchValidation(_))
    ));

    fs::write(
        mod_dir.join("mod.toml"),
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"dawid.alias-mod\"\nname = \"Alias Mod\"\nversion = \"1.0.0\"\n\n\
         [[patches]]\n[patches.target]\nname = \"missing_resource\"\n\
         [[patches.properties]]\nid = \"0x09AE19D7\"\ntype = \"float\"\noperation = \"set\"\nvalue = 10.0\n",
    )
    .unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::PatchValidation(_))
    ));

    fs::write(
        mod_dir.join("mod.toml"),
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"dawid.alias-mod\"\nname = \"Alias Mod\"\nversion = \"1.0.0\"\n\n\
         [[patches]]\ntarget = \"00B1B104:61EFC000:719436BD\"\n\
         [[patches.properties]]\nid = \"0x09AE19D7\"\ntype = \"float\"\noperation = \"set\"\nvalue = 10.0\n\
         [[patches]]\ntarget = \"00B1B104:61EFC000:719436BD\"\n\
         [[patches.properties]]\nid = \"0x09AE19D7\"\ntype = \"float\"\noperation = \"add\"\nvalue = 5.0\n",
    )
    .unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::PatchValidation(_))
    ));

    fs::write(
        mod_dir.join("mod.toml"),
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"dawid.alias-mod\"\nname = \"Alias Mod\"\nversion = \"1.0.0\"\n\n\
         [[patches]]\ntarget = \"00B1B104:61EFC000:719436BD\"\n\
         [[patches.properties]]\nid = \"0x09AE19D7\"\ntype = \"float\"\noperation = \"divide\"\nvalue = 0.0\n",
    )
    .unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::PatchValidation(_))
    ));

    fs::write(
        mod_dir.join("mod.toml"),
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"simoder.base\"\nname = \"Base\"\nversion = \"1.0.0\"\n\n\
         [[patches]]\ntarget = \"00B1B104:61EFC000:719436BD\"\n\
         [[patches.properties]]\nid = \"0x09AE19D7\"\ntype = \"float\"\noperation = \"set\"\nvalue = 1.0\n",
    )
    .unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::ManifestValidation(_))
    ));

    fs::write(
        mod_dir.join("mod.toml"),
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"dawid.empty-mod\"\nname = \"Empty\"\nversion = \"1.0.0\"\n",
    )
    .unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::ManifestValidation(_))
    ));

    write_authoring_patch_mod(&mod_dir, "dawid.alias-mod", "Alias Mod", 10.0);
    fs::write(mod_dir.join("mod.toon"), "id: legacy\n").unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::ManifestValidation(_))
    ));
    fs::remove_file(mod_dir.join("mod.toon")).unwrap();
    fs::write(mod_dir.join("project.toml"), "schemaVersion = 1\n").unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::ManifestValidation(_))
    ));
}

#[test]
fn new_png_glb_material_and_vanilla_reference_validation_enforces_no_vanilla_copies() {
    let dir = tempfile::tempdir().unwrap();
    let mod_dir = dir.path().join("custom_clinic");
    let assets_dir = mod_dir.join("assets");
    fs::create_dir_all(&assets_dir).unwrap();

    let png_diffuse = minimal_valid_png(128, 128);
    let png_norm = minimal_valid_png(64, 64);
    let glb_mesh = minimal_valid_glb(true);
    let params_json = br#"{"shader":"StandardBuilding","roughness":0.6}"#.to_vec();

    fs::write(assets_dir.join("diffuse.png"), &png_diffuse).unwrap();
    fs::write(assets_dir.join("normal.png"), &png_norm).unwrap();
    fs::write(assets_dir.join("clinic.glb"), &glb_mesh).unwrap();
    fs::write(assets_dir.join("clinic.mat.json"), &params_json).unwrap();

    let sha_diff = hex::encode(sha256_bytes(&png_diffuse));
    let sha_norm = hex::encode(sha256_bytes(&png_norm));
    let sha_glb = hex::encode(sha256_bytes(&glb_mesh));
    let sha_mat = hex::encode(sha256_bytes(&params_json));
    let vanilla_sha = hex::encode(sha256_bytes(b"vanilla-night-map-texture-bytes"));

    let valid_toml = format!(
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"dawid.custom-clinic\"\nname = \"Custom Clinic\"\nversion = \"1.0.0\"\n\n\
         [[assets.references]]\n\
         alias = \"vanilla_night_glow\"\n\
         target = \"2F7D0004:40E00000:AABBCCDD\"\n\
         source = \"SimCity_Graphics.package\"\n\
         expected_sha256 = \"{vanilla_sha}\"\n\n\
         [[assets.new]]\n\
         id = \"clinic_diffuse\"\nkind = \"png\"\npath = \"assets/diffuse.png\"\n\
         sha256 = \"{sha_diff}\"\nsize = {}\n\n\
         [[assets.new]]\n\
         id = \"clinic_normal\"\nkind = \"png\"\npath = \"assets/normal.png\"\n\
         sha256 = \"{sha_norm}\"\nsize = {}\n\n\
         [[assets.new]]\n\
         id = \"clinic_mesh\"\nkind = \"glb\"\npath = \"assets/clinic.glb\"\n\
         sha256 = \"{sha_glb}\"\nsize = {}\n\n\
         [[assets.new]]\n\
         id = \"clinic_mat_params\"\nkind = \"material-params\"\npath = \"assets/clinic.mat.json\"\n\
         sha256 = \"{sha_mat}\"\nsize = {}\n\n\
         [[assets.materials]]\n\
         id = \"clinic_main_mat\"\nparams = \"clinic_mat_params\"\n\
         slots = [\"clinic_diffuse\", \"clinic_normal\", \"clinic_diffuse\", \"vanilla_night_glow\", \"vanilla_night_glow\"]\n\
         uses_tint_texcoord_1 = true\n",
        png_diffuse.len(),
        png_norm.len(),
        glb_mesh.len(),
        params_json.len(),
    );
    fs::write(mod_dir.join("mod.toml"), &valid_toml).unwrap();

    let def = load_mod_definition(&mod_dir).unwrap();
    assert_eq!(def.new_assets().len(), 4);
    assert_eq!(def.vanilla_references().len(), 1);
    assert_eq!(def.materials().len(), 1);
    assert!(def.materials()[0].uses_tint_texcoord_1);

    let known_vanilla: HashSet<String> = [vanilla_sha.clone()].into_iter().collect();
    assert!(validate_no_vanilla_copies(def.new_assets(), &known_vanilla).is_ok());
    assert!(
        audit_mod_directory_no_vanilla_copies(&mod_dir, &known_vanilla)
            .unwrap()
            .is_empty()
    );

    let bad_missing = valid_toml.replace("assets/normal.png", "assets/missing.png");
    fs::write(mod_dir.join("mod.toml"), &bad_missing).unwrap();
    assert!(load_mod_definition(&mod_dir).is_err());

    let bad_sha = valid_toml.replace(&sha_diff, &"0".repeat(64));
    fs::write(mod_dir.join("mod.toml"), &bad_sha).unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::AssetValidation(_))
    ));

    let bad_size = valid_toml.replacen(
        &format!("size = {}", png_diffuse.len()),
        &format!("size = {}", png_diffuse.len() + 1),
        1,
    );
    fs::write(mod_dir.join("mod.toml"), &bad_size).unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::AssetValidation(_))
    ));

    let mut bad_crc_png = minimal_valid_png(8, 8);
    bad_crc_png[29] ^= 0xFF;
    for bad_png in [b"not-a-valid-png-datastream".to_vec(), bad_crc_png] {
        let bad_png_sha = hex::encode(sha256_bytes(&bad_png));
        fs::write(assets_dir.join("bad.png"), &bad_png).unwrap();
        let bad_png_toml = valid_toml
            .replace("assets/normal.png", "assets/bad.png")
            .replace(&sha_norm, &bad_png_sha)
            .replace(&format!("size = {}", png_norm.len()), &format!("size = {}", bad_png.len()));
        fs::write(mod_dir.join("mod.toml"), &bad_png_toml).unwrap();
        assert!(matches!(
            load_mod_definition(&mod_dir),
            Err(ModError::AssetValidation(_))
        ));
    }

    for corrupt_glb in [
        b"bad!bad!bad!bad!bad!".to_vec(),
        br#"{"asset":{"version":"2.0"}}"#.to_vec(),
        {
            let mut g = minimal_valid_glb(false);
            g[4..8].copy_from_slice(&1_u32.to_le_bytes());
            g
        },
        {
            let mut g = minimal_valid_glb(true);
            g.pop();
            g
        },
        gltf::binary::Glb {
            header: gltf::binary::Header {
                magic: *b"glTF",
                version: 2,
                length: 0,
            },
            json: Cow::Borrowed(br#"{"not_asset":true}"#),
            bin: None,
        }
        .to_vec()
        .unwrap(),
    ] {
        let sha = hex::encode(sha256_bytes(&corrupt_glb));
        fs::write(assets_dir.join("bad.glb"), &corrupt_glb).unwrap();
        let toml_text = valid_toml
            .replace("assets/clinic.glb", "assets/bad.glb")
            .replace(&sha_glb, &sha)
            .replace(&format!("size = {}", glb_mesh.len()), &format!("size = {}", corrupt_glb.len()));
        fs::write(mod_dir.join("mod.toml"), &toml_text).unwrap();
        assert!(matches!(
            load_mod_definition(&mod_dir),
            Err(ModError::AssetValidation(_))
        ));
    }

    for unsafe_path in [
        "assets/../mod.toml",
        "assets\\\\diffuse.png",
        "assets/C:diffuse.png",
        "textures/diffuse.png",
    ] {
        let unsafe_toml = valid_toml.replace("assets/diffuse.png", unsafe_path);
        fs::write(mod_dir.join("mod.toml"), &unsafe_toml).unwrap();
        assert!(matches!(
            load_mod_definition(&mod_dir),
            Err(ModError::UnsafePath(_))
        ));
    }

    let ref_with_path = valid_toml.replace(
        "source = \"SimCity_Graphics.package\"",
        "source = \"SimCity_Graphics.package\"\npath = \"assets/diffuse.png\"",
    );
    fs::write(mod_dir.join("mod.toml"), &ref_with_path).unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::VanillaCopyViolation(_))
    ));

    let copied_vanilla = minimal_valid_png(16, 16);
    let copied_sha = hex::encode(sha256_bytes(&copied_vanilla));
    fs::write(assets_dir.join("copied_vanilla.png"), &copied_vanilla).unwrap();
    let vanilla_copy_toml = valid_toml
        .replace(&vanilla_sha, &copied_sha)
        .replace("assets/diffuse.png", "assets/copied_vanilla.png")
        .replace(&sha_diff, &copied_sha)
        .replace(
            &format!("size = {}", png_diffuse.len()),
            &format!("size = {}", copied_vanilla.len()),
        );
    fs::write(mod_dir.join("mod.toml"), &vanilla_copy_toml).unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::VanillaCopyViolation(_))
    ));

    let disk_vanilla_set: HashSet<String> = [copied_sha].into_iter().collect();
    let flagged = audit_mod_directory_no_vanilla_copies(&mod_dir, &disk_vanilla_set).unwrap();
    assert_eq!(flagged.len(), 1);
    assert!(flagged[0].ends_with("copied_vanilla.png"));
}

#[test]
fn schema3_raw_resources_validates_format_and_is_blocked_from_activation() {
    let dir = tempfile::tempdir().unwrap();
    let mods_dir = dir.path().join("mods");
    let raw_mod_dir = mods_dir.join("RawOverlayMod");
    let assets_dir = raw_mod_dir.join("assets");
    fs::create_dir_all(&assets_dir).unwrap();

    let payload = b"raw-binary-resource-bytes";
    let sha = hex::encode(sha256_bytes(payload));
    fs::write(assets_dir.join("custom.bin"), payload).unwrap();

    let valid_raw = format!(
        "schemaVersion = 3\ncontract = \"simoder.raw-resources\"\n\
         requiredCapabilities = [\"resource.register\"]\n\n\
         [metadata]\nid = \"dawid.raw-mod\"\nname = \"Raw Mod\"\nversion = \"1.0.0\"\n\n\
         [[resources]]\n\
         id = \"00b1b104:40404000:000000001234abcd\"\n\
         operation = \"add\"\n\
         sha256 = \"{sha}\"\n\
         size = {}\n\
         payload = \"assets/custom.bin\"\n",
        payload.len()
    );
    fs::write(raw_mod_dir.join("mod.toml"), &valid_raw).unwrap();

    let parsed = load_mod_definition(&raw_mod_dir).unwrap();
    let raw_data = parsed.raw_resources().unwrap();
    assert_eq!(raw_data.resources.len(), 1);
    assert_eq!(
        raw_data.resources[0].id,
        "00B1B104:40404000:000000001234ABCD"
    );
    assert_eq!(
        raw_data.required_capabilities,
        vec![RuntimeCapability::ResourceRegister]
    );

    let registry = Arc::new(PatchRegistry::new());
    let manager = ModManager::new(&mods_dir, dir.path().join("state.toon"), Arc::clone(&registry));
    manager.initialize().unwrap();
    let snap = manager.snapshot();
    assert_eq!(snap.len(), 1);
    assert_eq!(snap[0].id, "dawid.raw-mod");
    assert_eq!(snap[0].state, ModState::Failed);
    assert!(!snap[0].can_enable);
    assert!(
        snap[0]
            .diagnostic
            .as_deref()
            .unwrap()
            .contains("resource.register")
    );
    assert!(manager.set_enabled("dawid.raw-mod", true).is_err());
    assert!(!registry.is_active("dawid.raw-mod"));

    let missing_cap = valid_raw.replace("operation = \"add\"", "operation = \"replace\"");
    fs::write(raw_mod_dir.join("mod.toml"), &missing_cap).unwrap();
    assert!(matches!(
        load_mod_definition(&raw_mod_dir),
        Err(ModError::ManifestValidation(_))
    ));

    let bad_tgi = valid_raw.replace("00b1b104:40404000:000000001234abcd", "00b1b104:40404000:1234");
    fs::write(raw_mod_dir.join("mod.toml"), &bad_tgi).unwrap();
    assert!(matches!(
        load_mod_definition(&raw_mod_dir),
        Err(ModError::ManifestValidation(_))
    ));

    let bad_size = valid_raw.replace(&format!("size = {}", payload.len()), "size = 999");
    fs::write(raw_mod_dir.join("mod.toml"), &bad_size).unwrap();
    assert!(matches!(
        load_mod_definition(&raw_mod_dir),
        Err(ModError::AssetValidation(_))
    ));
    let bad_sha = valid_raw.replace(&sha, &"a".repeat(64));
    fs::write(raw_mod_dir.join("mod.toml"), &bad_sha).unwrap();
    assert!(matches!(
        load_mod_definition(&raw_mod_dir),
        Err(ModError::AssetValidation(_))
    ));

    // Reject non-regular manifest paths (e.g. a directory named `mod.toml`).
    let dir_manifest_mod = dir.path().join("dir_manifest");
    fs::create_dir_all(dir_manifest_mod.join("mod.toml")).unwrap();
    assert!(matches!(
        load_stable_candidate(&dir_manifest_mod),
        Err(ModError::ManifestValidation(_))
    ));
}
