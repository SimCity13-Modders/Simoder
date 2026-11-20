//! Comprehensive integration tests for `simoder-mods`: patch registry & arithmetic,
//! persistent `state.toon`, Schema 2/3 manifests, PNG/GLB/material/vanilla-reference validation,
//! DBPF overlay policy & menu-clone rewriting, and full transactional `ModManager` lifecycle.

use std::borrow::Cow;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use simoder_core::{RuntimePropertyRecord, SCALAR_FLOAT_METADATA, Tgi, hex};
use simoder_formats::sc_properties::PropertyEncoding;
use simoder_formats::{
    Key, Kind, Package, PropType, Property, PropertyFile, Value, build_resource_overlay,
};
use simoder_mods::{
    GLB_TYPE_ID, MENU_ICON_PROP_HASH, MENU_MARQUEE_IMAGE_PROP_HASH, MENU_PARENT_PROP_HASH,
    MENU_UNIT_PROP_HASH, MOD_ASSET_GROUP_ID, MenuCloneOverrides, ModError, ModManager, ModState,
    PACKAGE_INFO_GROUP_ID, PNG_TYPE_ID, PROP_TYPE_ID, PackageMetadataVerdict,
    PackageMountLocation, PatchOperation, PatchRegistry, PropertyPatch, PropertyValueType,
    RegisteredPatch, ResourcePatchDefinition, ResourceSymbolOrigin, RuntimeCapability,
    allocate_mod_instance_id, apply_float_operation, apply_patch_sequence,
    audit_mod_directory_no_vanilla_copies, build_mod_overlay_package,
    inspect_package_bytes_metadata_policy, load_enabled_state, load_mod_definition,
    mount_overrides_base_game, rewrite_menu_clone_references, rewrite_tgi_references,
    save_enabled_state, sha256_bytes, validate_no_vanilla_copies,
};

fn target_tgi(instance: u32) -> Tgi {
    Tgi {
        type_id: 0x00B1_B104,
        group: 0x61EF_C000,
        instance,
    }
}

fn minimal_valid_png(width: u32, height: u32) -> Vec<u8> {
    let mut buf = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut buf, width, height);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().unwrap();
        let pixels = vec![255u8; (width * height * 4) as usize];
        writer.write_image_data(&pixels).unwrap();
    }
    buf
}

fn minimal_valid_glb(include_bin_chunk: bool) -> Vec<u8> {
    let json: &[u8] = if include_bin_chunk {
        br#"{"asset":{"version":"2.0","generator":"simoder-test"},"buffers":[{"byteLength":4}]}"#
    } else {
        br#"{"asset":{"version":"2.0","generator":"simoder-test"}}"#
    };
    gltf::binary::Glb {
        header: gltf::binary::Header {
            magic: *b"glTF",
            version: 2,
            length: 0,
        },
        json: Cow::Borrowed(json),
        bin: include_bin_chunk.then_some(Cow::Borrowed(&[0xAA, 0xBB, 0xCC, 0xDD])),
    }
    .to_vec()
    .unwrap()
}

fn write_authoring_patch_mod(dir: &Path, id: &str, name: &str, cost_value: f32) {
    fs::create_dir_all(dir).unwrap();
    let content = format!(
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"{id}\"\nname = \"{name}\"\nversion = \"1.0.0\"\nauthor = \"tester\"\n\n\
         [[patches]]\n\
         target = {{ type = \"0x00B1B104\", group = \"0x61EFC000\", instance = \"0x719436BD\" }}\n\
         [[patches.properties]]\n\
         id = \"0x09AE19D7\"\ntype = \"float\"\noperation = \"set\"\nvalue = {cost_value}\n"
    );
    fs::write(dir.join("mod.toml"), content).unwrap();
}

#[test]
fn patch_registry_tracks_activation_sequence_conflicts_replace_and_runtime_arithmetic() {
    let target = target_tgi(0x7194_36BD);
    let registry = PatchRegistry::new();
    assert_eq!(registry.generation(), 0);

    let patch_a = ResourcePatchDefinition {
        target,
        symbol: Some(ResourceSymbolOrigin {
            name: "sanitizer".into(),
            source: "EcoGame".into(),
        }),
        properties: vec![
            PropertyPatch {
                property_id: 0x09AE_19D7,
                value_type: PropertyValueType::Float,
                operation: PatchOperation::Add,
                value: 45.0,
                symbol: None,
            },
            PropertyPatch {
                property_id: 0x0B00_0001,
                value_type: PropertyValueType::Float,
                operation: PatchOperation::Multiply,
                value: 2.0,
                symbol: None,
            },
        ],
    };
    let patch_b = ResourcePatchDefinition {
        target,
        symbol: None,
        properties: vec![PropertyPatch {
            property_id: 0x09AE_19D7,
            value_type: PropertyValueType::Float,
            operation: PatchOperation::Divide,
            value: 3.0,
            symbol: None,
        }],
    };

    let affected_a = registry.activate("mod-z", &[patch_a]).unwrap();
    assert_eq!(affected_a, vec![target]);
    registry.activate("mod-a", &[patch_b]).unwrap();
    assert_eq!(registry.generation(), 2);

    let snap = registry.current();
    assert_eq!(snap.active_targets(), vec![target]);
    let registered = snap.find(target).unwrap();
    // Sequence 0 (`mod-z`) precedes sequence 1 (`mod-a`) regardless of alphabetical name.
    assert_eq!(registered[0].owner, "mod-z");
    assert_eq!(registered[0].sequence, 0);
    assert_eq!(registered[1].owner, "mod-a");
    assert_eq!(registered[1].sequence, 1);

    let conflicts = snap.conflicts();
    assert_eq!(conflicts.len(), 1);
    assert_eq!(conflicts[0].previous_owner, "mod-z");
    assert_eq!(conflicts[0].winning_owner, "mod-a");
    assert_eq!(conflicts[0].property_id, 0x09AE_19D7);

    // Runtime table arithmetic evaluation across active patch chain.
    let vanilla = [
        RuntimePropertyRecord {
            id: 0x09AE_19D7,
            value_bits: 300.0_f32.to_bits(),
            auxiliary: [0; 3],
            metadata: SCALAR_FLOAT_METADATA,
        },
        RuntimePropertyRecord {
            id: 0x0B00_0001,
            value_bits: 10.0_f32.to_bits(),
            auxiliary: [0; 3],
            metadata: SCALAR_FLOAT_METADATA,
        },
    ];
    let out = apply_patch_sequence(target, &vanilla, registered).unwrap();
    // (300.0 + 45.0) / 3.0 = 115.0; 10.0 * 2.0 = 20.0
    assert_eq!(f32::from_bits(out[0].value_bits), 115.0);
    assert_eq!(f32::from_bits(out[1].value_bits), 20.0);

    // Replace preserves sequence 0 for `mod-z`.
    let replacement = ResourcePatchDefinition {
        target,
        symbol: None,
        properties: vec![PropertyPatch {
            property_id: 0x09AE_19D7,
            value_type: PropertyValueType::Float,
            operation: PatchOperation::Subtract,
            value: 30.0,
            symbol: None,
        }],
    };
    registry.replace("mod-z", &[replacement]).unwrap();
    let replaced_reg = registry.current().find(target).unwrap().to_vec();
    assert_eq!(replaced_reg[0].owner, "mod-z");
    assert_eq!(replaced_reg[0].sequence, 0);
    assert_eq!(replaced_reg[1].owner, "mod-a");
    assert_eq!(replaced_reg[1].sequence, 1);

    let out_after_replace = apply_patch_sequence(target, &vanilla, &replaced_reg).unwrap();
    // (300.0 - 30.0) / 3.0 = 90.0
    assert_eq!(f32::from_bits(out_after_replace[0].value_bits), 90.0);

    // Arithmetic / table error guards.
    assert!(matches!(
        apply_float_operation(10.0, PatchOperation::Divide, 0.0),
        Err(ModError::PatchValidation(_))
    ));
    assert!(matches!(
        apply_float_operation(f32::MAX, PatchOperation::Multiply, 10.0),
        Err(ModError::PatchValidation(_))
    ));
    let unsorted = [vanilla[1], vanilla[0]];
    assert!(matches!(
        apply_patch_sequence(target, &unsorted, &replaced_reg),
        Err(ModError::PatchValidation(_))
    ));
    let wrong_meta = [
        RuntimePropertyRecord {
            id: 0x09AE_19D7,
            value_bits: 1,
            auxiliary: [0; 3],
            metadata: 0x0009_0000, // Int32
        },
        vanilla[1],
    ];
    assert!(matches!(
        apply_patch_sequence(target, &wrong_meta, &replaced_reg),
        Err(ModError::PatchValidation(_))
    ));
    let missing_prop = vec![RegisteredPatch {
        owner: "mod-x".into(),
        patch: ResourcePatchDefinition {
            target,
            symbol: None,
            properties: vec![PropertyPatch {
                property_id: 0xDEAD_BEEF,
                value_type: PropertyValueType::Float,
                operation: PatchOperation::Set,
                value: 1.0,
                symbol: None,
            }],
        },
        sequence: 0,
    }];
    assert!(matches!(
        apply_patch_sequence(target, &vanilla, &missing_prop),
        Err(ModError::PatchValidation(_))
    ));
}

#[test]
fn state_store_round_trips_empty_and_ordered_lists_and_rejects_invalid_inputs() {
    let dir = tempfile::tempdir().unwrap();
    let state_path = dir.path().join("simoder").join("state.toon");

    // Missing file returns empty order cleanly.
    assert!(load_enabled_state(&state_path).unwrap().is_empty());

    // Empty list round-trip.
    save_enabled_state(&state_path, &[]).unwrap();
    assert!(load_enabled_state(&state_path).unwrap().is_empty());

    // Multi-mod ordered round-trip.
    let order = vec![
        "dawid.first-mod".to_string(),
        "second_mod".to_string(),
        "mod-3".to_string(),
    ];
    save_enabled_state(&state_path, &order).unwrap();
    assert_eq!(load_enabled_state(&state_path).unwrap(), order);

    // Invalid IDs & duplicates rejected on save.
    assert!(matches!(
        save_enabled_state(&state_path, &["BadUpperCase".to_string()]),
        Err(ModError::InvalidModId(_))
    ));
    assert!(matches!(
        save_enabled_state(&state_path, &[".leading-dot".to_string()]),
        Err(ModError::InvalidModId(_))
    ));
    assert!(matches!(
        save_enabled_state(&state_path, &["double..sep".to_string()]),
        Err(ModError::InvalidModId(_))
    ));
    assert!(matches!(
        save_enabled_state(&state_path, &["dup.id".to_string(), "dup.id".to_string()]),
        Err(ModError::ManifestValidation(_))
    ));

    // Reject duplicate IDs, invalid IDs, and corrupt TOON on disk.
    fs::write(&state_path, "enabled[2]: dup.mod,dup.mod\n").unwrap();
    assert!(matches!(
        load_enabled_state(&state_path),
        Err(ModError::ManifestValidation(_))
    ));
    fs::write(&state_path, "enabled[1]: Invalid_ID\n").unwrap();
    assert!(matches!(
        load_enabled_state(&state_path),
        Err(ModError::InvalidModId(_))
    ));
    fs::write(&state_path, "unexpected_key: 123\n").unwrap();
    assert!(matches!(
        load_enabled_state(&state_path),
        Err(ModError::Format(_))
    ));
}

#[test]
fn manifest_validates_symbol_aliases_hex_forms_and_rejects_invalid_authoring() {
    let dir = tempfile::tempdir().unwrap();
    let mod_dir = dir.path().join("alias_mod");
    fs::create_dir_all(&mod_dir).unwrap();

    // Valid alias usage mixing integer and hex strings (including integer property ID).
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
    let def = load_mod_definition(&mod_dir).unwrap();
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

    // Name / ID mismatch rejected.
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

    // Unknown alias rejected.
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

    // Duplicate (TGI, property) rejected across two patch blocks.
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

    // Divide by zero rejected.
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

    // Reserved `simoder.base` ID rejected.
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

    // Empty authoring manifest rejected.
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

    // Coexistence with retired manifests rejected.
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

    // Known vanilla digest set matching our fake vanilla blob detects nothing in clean mod dir.
    let known_vanilla: HashSet<String> = [vanilla_sha.clone()].into_iter().collect();
    assert!(validate_no_vanilla_copies(def.new_assets(), &known_vanilla).is_ok());
    assert!(
        audit_mod_directory_no_vanilla_copies(&mod_dir, &known_vanilla)
            .unwrap()
            .is_empty()
    );

    // Reject missing asset file.
    let bad_missing = valid_toml.replace("assets/normal.png", "assets/missing.png");
    fs::write(mod_dir.join("mod.toml"), &bad_missing).unwrap();
    assert!(load_mod_definition(&mod_dir).is_err());

    // Reject SHA-256 mismatch.
    let bad_sha = valid_toml.replace(&sha_diff, &"0".repeat(64));
    fs::write(mod_dir.join("mod.toml"), &bad_sha).unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::AssetValidation(_))
    ));

    // Reject size mismatch.
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

    // Reject corrupt PNG (bad signature or corrupt IHDR CRC).
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

    // Reject corrupt GLB (wrong version, bad magic, plain JSON, truncated chunk, invalid JSON chunk).
    for corrupt_glb in [
        b"bad!bad!bad!bad!bad!".to_vec(),
        br#"{"asset":{"version":"2.0"}}"#.to_vec(),
        {
            let mut g = minimal_valid_glb(false);
            g[4..8].copy_from_slice(&1_u32.to_le_bytes()); // GLB v1
            g
        },
        {
            let mut g = minimal_valid_glb(true);
            g.pop(); // declared header length no longer matches disk length
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

    // Reject unsafe paths: traversal `..`, backslash `\`, colon `:`, non-`assets/` prefix.
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

    // Reject `path` key inside `[[assets.references]]`.
    let ref_with_path = valid_toml.replace(
        "source = \"SimCity_Graphics.package\"",
        "source = \"SimCity_Graphics.package\"\npath = \"assets/diffuse.png\"",
    );
    fs::write(mod_dir.join("mod.toml"), &ref_with_path).unwrap();
    assert!(matches!(
        load_mod_definition(&mod_dir),
        Err(ModError::VanillaCopyViolation(_))
    ));

    // Reject a vanilla file disguised as `[[assets.new]]` matching `expected_sha256` & directory audit.
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

    // Audit scans disk directly and flags copied vanilla file even outside `mod.toml`.
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

    // Manager blocks Schema 3 mod with `resource.register` diagnostic and keeps registry clean.
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

    // Reject undeclared capability (`replace` without `resource.replace`).
    let missing_cap = valid_raw.replace("operation = \"add\"", "operation = \"replace\"");
    fs::write(raw_mod_dir.join("mod.toml"), &missing_cap).unwrap();
    assert!(matches!(
        load_mod_definition(&raw_mod_dir),
        Err(ModError::ManifestValidation(_))
    ));

    // Reject malformed 64-bit resource ID.
    let bad_tgi = valid_raw.replace("00b1b104:40404000:000000001234abcd", "00b1b104:40404000:1234");
    fs::write(raw_mod_dir.join("mod.toml"), &bad_tgi).unwrap();
    assert!(matches!(
        load_mod_definition(&raw_mod_dir),
        Err(ModError::ManifestValidation(_))
    ));

    // Reject size & SHA mismatch.
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
}

#[test]
fn overlay_packaging_metadata_policy_mount_location_and_menu_rewrites() {
    let dir = tempfile::tempdir().unwrap();
    let mod_dir = dir.path().join("overlay_mod");
    let assets_dir = mod_dir.join("assets");
    fs::create_dir_all(&assets_dir).unwrap();

    let png = minimal_valid_png(32, 32);
    let glb = minimal_valid_glb(false);
    fs::write(assets_dir.join("icon.png"), &png).unwrap();
    fs::write(assets_dir.join("model.glb"), &glb).unwrap();

    let sha_png = hex::encode(sha256_bytes(&png));
    let sha_glb = hex::encode(sha256_bytes(&glb));
    let manifest_toml = format!(
        "schemaVersion = 2\nformat = \"simoder.authoring\"\n\n\
         [metadata]\nid = \"dawid.overlay-mod\"\nname = \"Overlay Mod\"\nversion = \"1.0.0\"\n\n\
         [[assets.references]]\n\
         alias = \"vanilla_parent_menu\"\ntarget = \"00B1B104:09878A01:00000010\"\nsource = \"SimCity_App.package\"\n\n\
         [[assets.new]]\n\
         id = \"custom_icon\"\nkind = \"png\"\npath = \"assets/icon.png\"\nsha256 = \"{sha_png}\"\nsize = {}\n\n\
         [[assets.new]]\n\
         id = \"custom_mesh\"\nkind = \"glb\"\npath = \"assets/model.glb\"\nsha256 = \"{sha_glb}\"\nsize = {}\n",
        png.len(),
        glb.len(),
    );
    fs::write(mod_dir.join("mod.toml"), manifest_toml).unwrap();

    let def = load_mod_definition(&mod_dir).unwrap();
    let extra_prop_tgi = Tgi {
        type_id: PROP_TYPE_ID,
        group: 0x1122_3344,
        instance: 0x0000_9999,
    };
    let overlay_bytes =
        build_mod_overlay_package(&def, &[(extra_prop_tgi, vec![0, 0, 0, 0])], &HashSet::new())
            .unwrap();
    assert_eq!(
        inspect_package_bytes_metadata_policy(&overlay_bytes).unwrap(),
        PackageMetadataVerdict::BypassSignature
    );

    // Open overlay via `Package::open` and verify only new assets + extra entries are packed.
    let pkg_path = dir.path().join("built.package");
    fs::write(&pkg_path, &overlay_bytes).unwrap();
    let expected_icon_tgi = Tgi {
        type_id: PNG_TYPE_ID,
        group: MOD_ASSET_GROUP_ID,
        instance: allocate_mod_instance_id("dawid.overlay-mod", "custom_icon"),
    };
    let expected_mesh_tgi = Tgi {
        type_id: GLB_TYPE_ID,
        group: MOD_ASSET_GROUP_ID,
        instance: allocate_mod_instance_id("dawid.overlay-mod", "custom_mesh"),
    };
    {
        let pkg = Package::open(&pkg_path).unwrap();
        let entry_tgis: HashSet<Tgi> = pkg.entries().iter().map(|e| e.id).collect();
        assert_eq!(entry_tgis.len(), 3);
        assert!(entry_tgis.contains(&expected_icon_tgi));
        assert!(entry_tgis.contains(&expected_mesh_tgi));
        assert!(entry_tgis.contains(&extra_prop_tgi));
        assert!(!entry_tgis.contains(&def.vanilla_references()[0].target));
    }

    // Reject package-info metadata TGI inside `build_mod_overlay_package`.
    let forbidden_meta = Tgi {
        type_id: PROP_TYPE_ID,
        group: PACKAGE_INFO_GROUP_ID,
        instance: 1,
    };
    assert!(matches!(
        build_mod_overlay_package(&def, &[(forbidden_meta, vec![1, 2, 3, 4])], &HashSet::new()),
        Err(ModError::PackageValidation(_))
    ));

    // Reject vanilla copy during overlay packaging.
    let blocked_set: HashSet<String> = [sha_png].into_iter().collect();
    assert!(matches!(
        build_mod_overlay_package(&def, &[], &blocked_set),
        Err(ModError::VanillaCopyViolation(_))
    ));

    // Verify metadata policy verdicts for 0, 1, and 2 `0x00B1B104` / `0x40404000` records.
    let pkg_zero = build_resource_overlay(&[(extra_prop_tgi, vec![1, 2])]).unwrap();
    assert_eq!(
        inspect_package_bytes_metadata_policy(&pkg_zero).unwrap(),
        PackageMetadataVerdict::BypassSignature
    );
    let pkg_one = build_resource_overlay(&[(forbidden_meta, vec![1, 2])]).unwrap();
    assert_eq!(
        inspect_package_bytes_metadata_policy(&pkg_one).unwrap(),
        PackageMetadataVerdict::RequiresRsaSignature
    );
    let forbidden_meta_2 = Tgi {
        type_id: PROP_TYPE_ID,
        group: PACKAGE_INFO_GROUP_ID,
        instance: 2,
    };
    let pkg_two =
        build_resource_overlay(&[(forbidden_meta, vec![1]), (forbidden_meta_2, vec![2])]).unwrap();
    assert_eq!(
        inspect_package_bytes_metadata_policy(&pkg_two).unwrap(),
        PackageMetadataVerdict::RejectedDuplicateMetadata(2)
    );

    // Mount location precedence check.
    assert!(mount_overrides_base_game(PackageMountLocation::SimCityData));
    assert!(!mount_overrides_base_game(PackageMountLocation::UserPackages));

    // Menu clone reference rewriting + canonical PROP encode/parse round-trip.
    let make_key_prop = |hash: u32, tgi: Tgi| Property {
        hash,
        prop_type: PropType::Key,
        kind: Kind::Scalar(Value::Key(Key {
            type_id: tgi.type_id,
            group: tgi.group,
            instance: tgi.instance,
        })),
        encoding: PropertyEncoding::default(),
    };
    let vanilla_parent = Tgi {
        type_id: PROP_TYPE_ID,
        group: 0x0987_8A01,
        instance: 0x0000_0001,
    };
    let vanilla_icon = Tgi {
        type_id: PNG_TYPE_ID,
        group: 0x40E0_0000,
        instance: 0x0000_0002,
    };
    let vanilla_marquee = Tgi {
        type_id: PNG_TYPE_ID,
        group: 0x40E0_0000,
        instance: 0x0000_0003,
    };
    let vanilla_unit = Tgi {
        type_id: PROP_TYPE_ID,
        group: 0x0987_8A01,
        instance: 0x0000_0004,
    };
    let unrelated_tgi = Tgi {
        type_id: PROP_TYPE_ID,
        group: 0x0000_1111,
        instance: 0x0000_2222,
    };

    let mut prop = PropertyFile {
        values: vec![
            make_key_prop(MENU_PARENT_PROP_HASH, vanilla_parent),
            make_key_prop(MENU_ICON_PROP_HASH, vanilla_icon),
            make_key_prop(MENU_MARQUEE_IMAGE_PROP_HASH, vanilla_marquee),
            make_key_prop(MENU_UNIT_PROP_HASH, vanilla_unit),
            make_key_prop(0x1234_5678, unrelated_tgi),
        ],
        claimed_count: 5,
    };

    let new_parent = target_tgi(0xAA01);
    let new_marquee = target_tgi(0xAA02);
    let new_unit = target_tgi(0xAA03);
    let menu_rewrites = rewrite_menu_clone_references(
        &mut prop,
        MenuCloneOverrides {
            parent_menu: Some(new_parent),
            menu_icon: Some(expected_icon_tgi),
            menu_marquee_image: Some(new_marquee),
            menu_unit: Some(new_unit),
        },
    );
    assert_eq!(menu_rewrites, 4);

    let encoded = prop.encode_canonical().unwrap();
    let reparsed = PropertyFile::parse(&encoded).unwrap();
    let read_key = |hash: u32| -> Tgi {
        let Some(Kind::Scalar(Value::Key(k))) = reparsed.get(hash).map(|p| &p.kind) else {
            panic!("missing scalar Key for 0x{hash:08X}");
        };
        Tgi {
            type_id: k.type_id,
            group: k.group,
            instance: k.instance,
        }
    };
    assert_eq!(read_key(MENU_PARENT_PROP_HASH), new_parent);
    assert_eq!(read_key(MENU_ICON_PROP_HASH), expected_icon_tgi);
    assert_eq!(read_key(MENU_MARQUEE_IMAGE_PROP_HASH), new_marquee);
    assert_eq!(read_key(MENU_UNIT_PROP_HASH), new_unit);
    assert_eq!(read_key(0x1234_5678), unrelated_tgi);

    let mut generic_prop = reparsed;
    let generic_target = target_tgi(0xBB01);
    let mut map = HashMap::new();
    map.insert(unrelated_tgi, generic_target);
    assert_eq!(rewrite_tgi_references(&mut generic_prop, &map), 1);
    let Some(Kind::Scalar(Value::Key(updated))) =
        generic_prop.get(0x1234_5678).map(|p| &p.kind)
    else {
        panic!("missing rewritten scalar key");
    };
    assert_eq!(updated.instance, generic_target.instance);
}

#[test]
fn mod_manager_full_lifecycle_cache_reload_duplicates_missing_and_stop_gate() {
    let dir = tempfile::tempdir().unwrap();
    let mods_dir = dir.path().join("mods");
    let state_path = dir.path().join("simoder").join("state.toon");
    let sanitizer_dir = mods_dir.join("BetterSanitizer");
    write_authoring_patch_mod(
        &sanitizer_dir,
        "dawid.better-sanitizer",
        "Better Sanitizer",
        345.0,
    );

    // Mixed-case `CaChE` directory (even with invalid/duplicate mods) must be skipped cleanly.
    let cache_dir = mods_dir.join("CaChE");
    write_authoring_patch_mod(
        &cache_dir,
        "dawid.better-sanitizer",
        "Cached Sanitizer",
        999.0,
    );
    fs::write(cache_dir.join("BrokenNested").join("mod.toml"), "invalid").ok();

    let refresh_calls = Arc::new(AtomicUsize::new(0));
    let refresh_counter = Arc::clone(&refresh_calls);
    let registry = Arc::new(PatchRegistry::new());
    let manager = ModManager::new(&mods_dir, &state_path, Arc::clone(&registry))
        .with_refresh_callback(Box::new(move |_affected| {
            refresh_counter.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }));

    manager.initialize().unwrap();
    let snap = manager.snapshot();
    assert_eq!(snap.len(), 1);
    assert_eq!(snap[0].id, "dawid.better-sanitizer");
    assert_eq!(snap[0].state, ModState::Inactive);
    assert!(snap[0].can_enable);
    assert!(!snap[0].can_disable);

    // Invalid content inside `CaChE` stays invisible on refresh.
    fs::write(cache_dir.join("mod.toml"), "invalid = [").unwrap();
    manager.refresh().unwrap();
    assert_eq!(manager.snapshot().len(), 1);

    // Enable first mod.
    manager.set_enabled("dawid.better-sanitizer", true).unwrap();
    assert!(registry.is_active("dawid.better-sanitizer"));
    assert_eq!(manager.enabled_order(), vec!["dawid.better-sanitizer"]);
    assert_eq!(refresh_calls.load(Ordering::SeqCst), 1);

    // Add second mod and enable it so we can assert activation order survives reload of first mod.
    let second_dir = mods_dir.join("SecondMod");
    write_authoring_patch_mod(&second_dir, "dawid.second-mod", "Second Mod", 500.0);
    manager.set_enabled("dawid.second-mod", true).unwrap();
    assert_eq!(
        manager.enabled_order(),
        vec!["dawid.better-sanitizer", "dawid.second-mod"]
    );

    // Restart persistence check with fresh `PatchRegistry` + `ModManager`.
    {
        let restarted_reg = Arc::new(PatchRegistry::new());
        let restarted =
            ModManager::new(&mods_dir, &state_path, Arc::clone(&restarted_reg));
        restarted.initialize().unwrap();
        assert!(restarted_reg.is_active("dawid.better-sanitizer"));
        assert!(restarted_reg.is_active("dawid.second-mod"));
        assert_eq!(
            restarted.enabled_order(),
            vec!["dawid.better-sanitizer", "dawid.second-mod"]
        );
    }

    // Valid edit while active transitions to Changed, keeping old patch active; revert restores Active.
    write_authoring_patch_mod(
        &sanitizer_dir,
        "dawid.better-sanitizer",
        "Better Sanitizer",
        360.0,
    );
    manager.refresh().unwrap();
    let entry_changed = manager
        .snapshot()
        .into_iter()
        .find(|e| e.id == "dawid.better-sanitizer")
        .unwrap();
    assert_eq!(entry_changed.state, ModState::Changed);
    assert!(entry_changed.has_pending_changes);
    assert!(entry_changed.can_reload);
    let rev_360 = entry_changed.pending_revision.clone().unwrap();
    let target = target_tgi(0x7194_36BD);
    assert_eq!(
        registry.current().find(target).unwrap()[0].patch.properties[0].value,
        345.0
    );

    write_authoring_patch_mod(
        &sanitizer_dir,
        "dawid.better-sanitizer",
        "Better Sanitizer",
        345.0,
    );
    manager.refresh().unwrap();
    let entry_reverted = manager
        .snapshot()
        .into_iter()
        .find(|e| e.id == "dawid.better-sanitizer")
        .unwrap();
    assert_eq!(entry_reverted.state, ModState::Active);
    assert!(!entry_reverted.can_reload);

    // Edit again and test stale vs current `reload` preserving original sequence 0!
    write_authoring_patch_mod(
        &sanitizer_dir,
        "dawid.better-sanitizer",
        "Better Sanitizer",
        360.0,
    );
    manager.refresh().unwrap();
    assert!(matches!(
        manager.reload("dawid.better-sanitizer", Some("deadbeef")),
        Err(ModError::Lifecycle(_))
    ));
    assert_eq!(
        manager
            .snapshot()
            .into_iter()
            .find(|e| e.id == "dawid.better-sanitizer")
            .unwrap()
            .pending_revision
            .as_deref(),
        Some(rev_360.as_str())
    );
    manager
        .reload("dawid.better-sanitizer", Some(&rev_360))
        .unwrap();
    assert_eq!(
        manager.enabled_order(),
        vec!["dawid.better-sanitizer", "dawid.second-mod"]
    );
    let registered_after_reload = registry.current().find(target).unwrap().to_vec();
    assert_eq!(registered_after_reload[0].owner, "dawid.better-sanitizer");
    assert_eq!(registered_after_reload[0].patch.properties[0].value, 360.0);
    assert_eq!(registered_after_reload[1].owner, "dawid.second-mod");

    // Disable second mod so we isolate sanitizer tests.
    manager.set_enabled("dawid.second-mod", false).unwrap();

    // Malformed edit while active -> Changed retaining active patches; disable -> Failed; fix + refresh -> Inactive -> enable works.
    fs::write(sanitizer_dir.join("mod.toml"), "patches = malformed\n").unwrap();
    manager.refresh().unwrap();
    let malformed_changed = manager
        .snapshot()
        .into_iter()
        .find(|e| e.id == "dawid.better-sanitizer")
        .unwrap();
    assert_eq!(malformed_changed.state, ModState::Changed);
    assert!(!malformed_changed.can_reload);
    assert!(registry.is_active("dawid.better-sanitizer"));
    assert_eq!(
        registry.current().find(target).unwrap()[0].patch.properties[0].value,
        360.0
    );

    manager.set_enabled("dawid.better-sanitizer", false).unwrap();
    let failed_after_disable = manager
        .snapshot()
        .into_iter()
        .find(|e| e.id == "dawid.better-sanitizer")
        .unwrap();
    assert_eq!(failed_after_disable.state, ModState::Failed);
    assert!(manager.set_enabled("dawid.better-sanitizer", true).is_err());

    write_authoring_patch_mod(
        &sanitizer_dir,
        "dawid.better-sanitizer",
        "Better Sanitizer",
        400.0,
    );
    manager.refresh().unwrap();
    manager.set_enabled("dawid.better-sanitizer", true).unwrap();
    assert_eq!(
        registry.current().find(target).unwrap()[0].patch.properties[0].value,
        400.0
    );

    // Broken sibling mod fails independently without harming active mods.
    let broken_dir = mods_dir.join("BrokenMod");
    fs::create_dir_all(&broken_dir).unwrap();
    fs::write(broken_dir.join("mod.toml"), "broken = [").unwrap();
    manager.refresh().unwrap();
    assert!(registry.is_active("dawid.better-sanitizer"));
    assert!(manager.snapshot().iter().any(|e| e.state == ModState::Failed));

    // Removing active mod directory unloads registry, removes from `state.toon`, marks Missing.
    fs::remove_dir_all(&sanitizer_dir).unwrap();
    manager.refresh().unwrap();
    assert!(!registry.is_active("dawid.better-sanitizer"));
    assert!(manager.enabled_order().is_empty());
    assert!(load_enabled_state(&state_path).unwrap().is_empty());
    assert!(manager.snapshot().iter().any(|e| {
        e.id == "dawid.better-sanitizer" && e.state == ModState::Missing
    }));

    // Duplicate IDs across two folders produce two Failed entries listing both paths.
    write_authoring_patch_mod(
        &mods_dir.join("DuplicateOne"),
        "test.duplicate",
        "Duplicate One",
        410.0,
    );
    write_authoring_patch_mod(
        &mods_dir.join("DuplicateTwo"),
        "test.duplicate",
        "Duplicate Two",
        420.0,
    );
    manager.refresh().unwrap();
    let dup_failures: Vec<_> = manager
        .snapshot()
        .into_iter()
        .filter(|e| {
            e.id == "test.duplicate"
                && e.state == ModState::Failed
                && e.diagnostic
                    .as_deref()
                    .is_some_and(|d| d.contains("DuplicateOne") && d.contains("DuplicateTwo"))
        })
        .collect();
    assert_eq!(dup_failures.len(), 2);

    // Sticky stop gate blocks initialize, refresh, set_enabled, and reload without mutating active state.
    let generation_before_stop = manager.generation();
    manager.set_enabled("dawid.second-mod", true).unwrap();
    let gen_active = manager.generation();
    assert!(gen_active > generation_before_stop);
    manager.request_stop();
    manager.request_stop();
    assert!(manager.is_stopping());
    assert!(
        manager
            .refresh()
            .unwrap_err()
            .to_string()
            .contains("stopping")
    );
    assert!(
        manager
            .set_enabled("dawid.second-mod", false)
            .unwrap_err()
            .to_string()
            .contains("stopping")
    );
    assert!(
        manager
            .reload("dawid.second-mod", None)
            .unwrap_err()
            .to_string()
            .contains("stopping")
    );
    assert_eq!(manager.generation(), gen_active);
    assert!(registry.is_active("dawid.second-mod"));

    let never_started = ModManager::new(&mods_dir, &state_path, Arc::new(PatchRegistry::new()));
    never_started.request_stop();
    assert!(
        never_started
            .initialize()
            .unwrap_err()
            .to_string()
            .contains("stopping")
    );
}
