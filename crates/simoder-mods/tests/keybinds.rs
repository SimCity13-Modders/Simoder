use simoder_mods::{
    KeyChord, KeybindConflictKind, KeybindContext, KeybindDescriptor, KeybindRegistry,
};
use tempfile::tempdir;

fn make_desc(
    mod_id: &str,
    action_id: &str,
    title: &str,
    context: &str,
    default_key: Option<&str>,
) -> KeybindDescriptor {
    KeybindDescriptor {
        owner_mod_id: mod_id.to_string(),
        action_id: action_id.to_string(),
        title: title.to_string(),
        description: format!("{title} description"),
        context: KeybindContext::parse(context).unwrap(),
        default_chord: default_key.map(|k| KeyChord::parse(k).unwrap()),
    }
}

#[test]
fn chord_normalization_and_validation() {
    let a = KeyChord::parse("ctrl + alt + c").unwrap();
    let b = KeyChord::parse("ALT+Ctrl+C").unwrap();
    assert_eq!(a, b);
    assert_eq!(a.canonical(), "Ctrl+Alt+C");

    assert_eq!(KeyChord::parse("escape").unwrap().canonical(), "Esc");
    assert_eq!(KeyChord::parse("~").unwrap().canonical(), "Backquote");
    assert_eq!(KeyChord::parse("f9").unwrap().canonical(), "F9");

    assert!(KeyChord::parse("").is_err());
    assert!(KeyChord::parse("Ctrl+Alt").is_err());
    assert!(KeyChord::parse("Ctrl+A+B").is_err());
    assert!(KeyChord::parse("UnknownKey").is_err());
}

#[test]
fn context_overlap_detects_mod_conflicts_accurately() {
    let mut reg = KeybindRegistry::new();
    let id_menu = reg
        .register(make_desc("mod.a", "open_a", "Open A", "main_menu", Some("E")))
        .unwrap();
    let id_build = reg
        .register(make_desc("mod.b", "build_b", "Build B", "city.build", Some("E")))
        .unwrap();

    // Disjoint contexts (`main_menu` vs `city.build`) must NOT conflict.
    let snap1 = reg.snapshot();
    assert!(!snap1.iter().find(|e| e.id == id_menu).unwrap().has_conflict);
    assert!(!snap1.iter().find(|e| e.id == id_build).unwrap().has_conflict);

    // Parent context (`city`) overlaps child (`city.build`) -> MUST conflict.
    let id_city = reg
        .register(make_desc("mod.c", "city_c", "City C", "city", Some("E")))
        .unwrap();
    let snap2 = reg.snapshot();
    assert!(snap2.iter().find(|e| e.id == id_build).unwrap().has_conflict);
    assert!(snap2.iter().find(|e| e.id == id_city).unwrap().has_conflict);
    assert!(!snap2.iter().find(|e| e.id == id_menu).unwrap().has_conflict);

    // Global context overlaps everything.
    let id_global = reg
        .register(make_desc("mod.d", "global_d", "Global D", "global", Some("E")))
        .unwrap();
    let snap3 = reg.snapshot();
    assert!(snap3.iter().find(|e| e.id == id_menu).unwrap().has_conflict);
    assert!(snap3.iter().find(|e| e.id == id_global).unwrap().has_conflict);
}

#[test]
fn detects_vanilla_cheat_console_conflict_and_supports_rebind_reset_persistence() {
    let dir = tempdir().unwrap();
    let toon_path = dir.path().join("keybinds.toon");

    let mut reg = KeybindRegistry::new();
    let id = reg
        .register(make_desc(
            "simoder.ui_inspector",
            "toggle_picker",
            "Toggle Element Picker",
            "global",
            Some("Ctrl+Shift+C"),
        ))
        .unwrap();

    let snap = reg.snapshot();
    assert_eq!(snap.len(), 1);
    assert!(snap[0].has_conflict);
    assert!(snap[0].conflicts.iter().any(|c| {
        c.kind == KeybindConflictKind::Vanilla && c.with_title.contains("Cheat Console")
    }));

    // Rebind to Ctrl+Alt+C -> conflict clears.
    let rebound = reg.rebind(&id, Some("alt+ctrl+c")).unwrap();
    assert_eq!(rebound.as_deref(), Some("Ctrl+Alt+C"));
    let snap_rebound = reg.snapshot();
    assert!(!snap_rebound[0].has_conflict);
    assert!(snap_rebound[0].is_custom);

    // Save and reload in a fresh registry.
    reg.save_overrides(&toon_path).unwrap();
    let mut reg2 = KeybindRegistry::new();
    reg2.load_overrides(&toon_path).unwrap();
    reg2.register(make_desc(
        "simoder.ui_inspector",
        "toggle_picker",
        "Toggle Element Picker",
        "global",
        Some("Ctrl+Shift+C"),
    ))
    .unwrap();
    let map = reg2.effective_map_for_mod("simoder.ui_inspector");
    assert_eq!(map.get("toggle_picker").map(String::as_str), Some("Ctrl+Alt+C"));

    // Unbind -> None.
    assert_eq!(reg2.rebind(&id, Some("")).unwrap(), None);
    assert!(reg2.effective_chord(&id).is_none());

    // Reset -> back to default & revoke_mod cleans up descriptor.
    reg2.reset(&id).unwrap();
    assert_eq!(
        reg2.effective_chord(&id).map(|c| c.canonical()),
        Some("Ctrl+Shift+C".to_string())
    );
    assert_eq!(reg2.revoke_mod("simoder.ui_inspector"), 1);
    assert!(reg2.snapshot().is_empty());
}
