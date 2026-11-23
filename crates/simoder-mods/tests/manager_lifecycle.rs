//! Full transactional `ModManager` lifecycle, discovery, `CaChE` filtering, hot-reload revision
//! verification, duplicate/missing recovery, and stop-gate integration tests.

mod common;

use std::fs;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use simoder_mods::{ModError, ModManager, ModState, PatchRegistry, load_enabled_state};

use common::{target_tgi, write_authoring_patch_mod};

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

    fs::write(cache_dir.join("mod.toml"), "invalid = [").unwrap();
    manager.refresh().unwrap();
    assert_eq!(manager.snapshot().len(), 1);

    manager.set_enabled("dawid.better-sanitizer", true).unwrap();
    assert!(registry.is_active("dawid.better-sanitizer"));
    assert_eq!(manager.enabled_order(), vec!["dawid.better-sanitizer"]);
    assert_eq!(refresh_calls.load(Ordering::SeqCst), 1);

    let second_dir = mods_dir.join("SecondMod");
    write_authoring_patch_mod(&second_dir, "dawid.second-mod", "Second Mod", 500.0);
    manager.set_enabled("dawid.second-mod", true).unwrap();
    assert_eq!(
        manager.enabled_order(),
        vec!["dawid.better-sanitizer", "dawid.second-mod"]
    );

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

    manager.set_enabled("dawid.second-mod", false).unwrap();

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

    let broken_dir = mods_dir.join("BrokenMod");
    fs::create_dir_all(&broken_dir).unwrap();
    fs::write(broken_dir.join("mod.toml"), "broken = [").unwrap();
    manager.refresh().unwrap();
    assert!(registry.is_active("dawid.better-sanitizer"));
    assert!(manager.snapshot().iter().any(|e| e.state == ModState::Failed));

    fs::remove_dir_all(&sanitizer_dir).unwrap();
    manager.refresh().unwrap();
    assert!(!registry.is_active("dawid.better-sanitizer"));
    assert!(manager.enabled_order().is_empty());
    assert!(load_enabled_state(&state_path).unwrap().is_empty());
    assert!(manager.snapshot().iter().any(|e| {
        e.id == "dawid.better-sanitizer" && e.state == ModState::Missing
    }));

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
