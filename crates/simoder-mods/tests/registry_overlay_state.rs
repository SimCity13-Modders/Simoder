//! Patch registry ordering, arithmetic evaluation, `state.toon` persistence,
//! DBPF overlay packaging, metadata policies, and menu-clone rewriting integration tests.

mod common;

use std::collections::{HashMap, HashSet};
use std::fs;

use simoder_core::{RuntimePropertyRecord, SCALAR_FLOAT_METADATA, Tgi, hex};
use simoder_formats::sc_properties::PropertyEncoding;
use simoder_formats::{
    Key, Kind, Package, PropType, Property, PropertyFile, Value, build_resource_overlay,
};
use simoder_mods::{
    GLB_TYPE_ID, MENU_ICON_PROP_HASH, MENU_MARQUEE_IMAGE_PROP_HASH, MENU_PARENT_PROP_HASH,
    MENU_UNIT_PROP_HASH, MOD_ASSET_GROUP_ID, MenuCloneOverrides, ModError, PACKAGE_INFO_GROUP_ID,
    PNG_TYPE_ID, PROP_TYPE_ID, PackageMetadataVerdict, PackageMountLocation, PatchOperation,
    PatchRegistry, PropertyPatch, PropertyValueType, RegisteredPatch, ResourcePatchDefinition,
    ResourceSymbolOrigin, allocate_mod_instance_id, apply_float_operation, apply_patch_sequence,
    build_mod_overlay_package, inspect_package_bytes_metadata_policy, load_enabled_state,
    load_mod_definition, mount_overrides_base_game, rewrite_menu_clone_references,
    rewrite_tgi_references, save_enabled_state, sha256_bytes,
};

use common::{minimal_valid_glb, minimal_valid_png, target_tgi};

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
    assert_eq!(registered[0].owner, "mod-z");
    assert_eq!(registered[0].sequence, 0);
    assert_eq!(registered[1].owner, "mod-a");
    assert_eq!(registered[1].sequence, 1);

    let conflicts = snap.conflicts();
    assert_eq!(conflicts.len(), 1);
    assert_eq!(conflicts[0].previous_owner, "mod-z");
    assert_eq!(conflicts[0].winning_owner, "mod-a");
    assert_eq!(conflicts[0].property_id, 0x09AE_19D7);

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
    assert_eq!(f32::from_bits(out[0].value_bits), 115.0);
    assert_eq!(f32::from_bits(out[1].value_bits), 20.0);

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
    assert_eq!(f32::from_bits(out_after_replace[0].value_bits), 90.0);

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
            metadata: 0x0009_0000,
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

    assert!(load_enabled_state(&state_path).unwrap().is_empty());

    save_enabled_state(&state_path, &[]).unwrap();
    assert!(load_enabled_state(&state_path).unwrap().is_empty());

    let order = vec![
        "dawid.first-mod".to_string(),
        "second_mod".to_string(),
        "mod-3".to_string(),
    ];
    save_enabled_state(&state_path, &order).unwrap();
    assert_eq!(load_enabled_state(&state_path).unwrap(), order);

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

    let forbidden_meta = Tgi {
        type_id: PROP_TYPE_ID,
        group: PACKAGE_INFO_GROUP_ID,
        instance: 1,
    };
    assert!(matches!(
        build_mod_overlay_package(&def, &[(forbidden_meta, vec![1, 2, 3, 4])], &HashSet::new()),
        Err(ModError::PackageValidation(_))
    ));

    let blocked_set: HashSet<String> = [sha_png].into_iter().collect();
    assert!(matches!(
        build_mod_overlay_package(&def, &[], &blocked_set),
        Err(ModError::VanillaCopyViolation(_))
    ));

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

    assert!(mount_overrides_base_game(PackageMountLocation::SimCityData));
    assert!(!mount_overrides_base_game(PackageMountLocation::UserPackages));

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
