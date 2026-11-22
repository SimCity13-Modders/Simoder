//! Integration tests for [`UiPatchRegistry`] multi-mod view composition, conflict detection,
//! generation revocation (HotReload UI cleanup), and node bounds.

use simoder_mods::{
    ModError, UiNode, UiNodeKind, UiPatchDescriptor, UiPatchOp, UiPatchRegistry, UiSelectOption,
};

fn button_node(label: &str, action: &str) -> UiNode {
    UiNode {
        kind: UiNodeKind::Button,
        id: Some(format!("btn_{action}")),
        label: Some(label.to_string()),
        action: Some(action.to_string()),
        ..Default::default()
    }
}

#[test]
fn ui_patch_registry_composes_multiple_mods_on_same_view_detects_conflicts_and_revokes_generations() {
    let mut registry = UiPatchRegistry::new();

    // Two different mods append buttons into the exact same `(view, slot)`.
    let rev1 = registry
        .register_patch(UiPatchDescriptor {
            patch_id: "mods-btn".into(),
            owner_mod_id: "simoder.base".into(),
            owner_generation: 1,
            view: "main.menu".into(),
            slot: "menu.actions".into(),
            op: UiPatchOp::Append,
            target: None,
            priority: 100,
            node: button_node("Mods", "open_mods"),
        })
        .unwrap();
    assert_eq!(rev1, 1);

    let rev2 = registry
        .register_patch(UiPatchDescriptor {
            patch_id: "road-presets".into(),
            owner_mod_id: "dawid.road-builder".into(),
            owner_generation: 2,
            view: "main.menu".into(),
            slot: "menu.actions".into(),
            op: UiPatchOp::Append,
            target: None,
            priority: 50,
            node: button_node("Road Presets", "open_presets"),
        })
        .unwrap();
    assert_eq!(rev2, 2);

    let (_rev, patches, conflicts) = registry.composed_patches_for_view("main.menu");
    assert!(conflicts.is_empty());
    assert_eq!(patches.len(), 2);
    // Lower priority integer (`50`) sorts deterministically ahead of `100`.
    assert_eq!(patches[0].patch_id, "dawid.road-builder.road-presets");
    assert_eq!(patches[1].patch_id, "simoder.base.mods-btn");

    // Two mods issuing `replace_slot` on the same `(view, slot, target)` produce an explicit conflict.
    registry
        .register_patch(UiPatchDescriptor {
            patch_id: "banner-a".into(),
            owner_mod_id: "mod.alpha".into(),
            owner_generation: 1,
            view: "main.menu".into(),
            slot: "header.banner".into(),
            op: UiPatchOp::ReplaceSlot,
            target: Some("motd".into()),
            priority: 10,
            node: button_node("Alpha", "a"),
        })
        .unwrap();
    registry
        .register_patch(UiPatchDescriptor {
            patch_id: "banner-b".into(),
            owner_mod_id: "mod.beta".into(),
            owner_generation: 1,
            view: "main.menu".into(),
            slot: "header.banner".into(),
            op: UiPatchOp::ReplaceSlot,
            target: Some("motd".into()),
            priority: 20,
            node: button_node("Beta", "b"),
        })
        .unwrap();

    let (_rev, composed, conflicts) = registry.composed_patches_for_view("main.menu");
    assert_eq!(composed.len(), 3);
    assert_eq!(conflicts.len(), 1);
    assert_eq!(conflicts[0].winner_patch_id, "mod.alpha.banner-a");
    assert_eq!(conflicts[0].loser_patch_id, "mod.beta.banner-b");

    // Cross-mod collision attempting to claim another mod's already-namespaced ID fails.
    assert!(matches!(
        registry.register_patch(UiPatchDescriptor {
            patch_id: "simoder.base.mods-btn".into(),
            owner_mod_id: "dawid.evil".into(),
            owner_generation: 1,
            view: "main.menu".into(),
            slot: "menu.actions".into(),
            op: UiPatchOp::Append,
            target: None,
            priority: 1,
            node: button_node("Spoof", "spoof"),
        }),
        Err(ModError::Lifecycle(_))
    ));

    // Unloading/reloading mod generation automatically removes only its patches and advances revision.
    assert_eq!(registry.revoke_generation("dawid.road-builder", 2), 1);
    let (rev_after, remaining, _) = registry.composed_patches_for_view("main.menu");
    assert!(rev_after > rev2);
    assert!(!remaining.iter().any(|p| p.owner_mod_id == "dawid.road-builder"));
}

#[test]
fn ui_patch_registry_validates_extended_input_select_slider_tabs_and_progress_primitives() {
    let mut registry = UiPatchRegistry::new();

    // Reject inverted slider range and non-positive step.
    assert!(matches!(
        registry.register_patch(UiPatchDescriptor {
            patch_id: "bad-slider".into(),
            owner_mod_id: "dawid.tools".into(),
            owner_generation: 1,
            view: "main.menu".into(),
            slot: "menu.actions".into(),
            op: UiPatchOp::Append,
            target: None,
            priority: 1,
            node: UiNode {
                kind: UiNodeKind::Slider,
                min: Some(100),
                max: Some(10),
                step: Some(1),
                ..Default::default()
            },
        }),
        Err(ModError::ManifestValidation(_))
    ));
    assert!(matches!(
        registry.register_patch(UiPatchDescriptor {
            patch_id: "bad-step".into(),
            owner_mod_id: "dawid.tools".into(),
            owner_generation: 1,
            view: "main.menu".into(),
            slot: "menu.actions".into(),
            op: UiPatchOp::Append,
            target: None,
            priority: 1,
            node: UiNode {
                kind: UiNodeKind::Slider,
                min: Some(0),
                max: Some(10),
                step: Some(0),
                ..Default::default()
            },
        }),
        Err(ModError::ManifestValidation(_))
    ));

    // Reject more than 128 options in Select.
    let too_many = (0..129)
        .map(|idx| UiSelectOption {
            value: format!("v{idx}"),
            label: format!("Label {idx}"),
        })
        .collect();
    assert!(matches!(
        registry.register_patch(UiPatchDescriptor {
            patch_id: "bad-select".into(),
            owner_mod_id: "dawid.tools".into(),
            owner_generation: 1,
            view: "main.menu".into(),
            slot: "menu.actions".into(),
            op: UiPatchOp::Append,
            target: None,
            priority: 1,
            node: UiNode {
                kind: UiNodeKind::Select,
                options: too_many,
                ..Default::default()
            },
        }),
        Err(ModError::ManifestValidation(_))
    ));

    // Valid Tabs containing Input, Select, Slider, Checkbox, and Progress.
    let rev = registry
        .register_patch(UiPatchDescriptor {
            patch_id: "settings-tabs".into(),
            owner_mod_id: "dawid.tools".into(),
            owner_generation: 1,
            view: "ingame.options".into(),
            slot: "modal.overlay".into(),
            op: UiPatchOp::ReplaceSlot,
            target: Some("tools".into()),
            priority: 0,
            node: UiNode {
                kind: UiNodeKind::Tabs,
                id: Some("root_tabs".into()),
                value: Some("general".into()),
                action: Some("switch_tab".into()),
                children: vec![UiNode {
                    kind: UiNodeKind::Tab,
                    id: Some("general".into()),
                    label: Some("General".into()),
                    children: vec![
                        UiNode {
                            kind: UiNodeKind::Input,
                            id: Some("search".into()),
                            placeholder: Some("Filter...".into()),
                            action: Some("filter".into()),
                            ..Default::default()
                        },
                        UiNode {
                            kind: UiNodeKind::Select,
                            id: Some("quality".into()),
                            value: Some("high".into()),
                            options: vec![UiSelectOption {
                                value: "high".into(),
                                label: "High".into(),
                            }],
                            ..Default::default()
                        },
                        UiNode {
                            kind: UiNodeKind::Slider,
                            id: Some("fov".into()),
                            value: Some("60".into()),
                            min: Some(45),
                            max: Some(90),
                            step: Some(5),
                            ..Default::default()
                        },
                        UiNode {
                            kind: UiNodeKind::Checkbox,
                            id: Some("grid".into()),
                            label: Some("Show Grid".into()),
                            checked: Some(true),
                            ..Default::default()
                        },
                        UiNode {
                            kind: UiNodeKind::Progress,
                            id: Some("sync_prog".into()),
                            value: Some("75".into()),
                            min: Some(0),
                            max: Some(100),
                            ..Default::default()
                        },
                    ],
                    ..Default::default()
                }],
                ..Default::default()
            },
        })
        .unwrap();
    assert_eq!(rev, 1);
}
