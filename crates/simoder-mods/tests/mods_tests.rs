//! Integration tests for `simoder-mods` TGI conflict resolution.

use simoder_core::Tgi;
use simoder_mods::{PatchEntry, PatchRegistry};

#[test]
fn resolves_last_wins_and_reports_overridden_contenders() {
    let tgi = Tgi {
        type_id: 0x00B1_B104,
        group: 0x0987_8A01,
        instance: 0x0000_0042,
    };
    let mut reg = PatchRegistry::new();
    reg.register(PatchEntry {
        owner: "mod-a".into(),
        target: tgi,
        payload_sha256: [1; 32],
    });
    reg.register(PatchEntry {
        owner: "mod-b".into(),
        target: tgi,
        payload_sha256: [2; 32],
    });

    assert_eq!(reg.active_for_target(tgi).unwrap().owner, "mod-b");
    let conflicts = reg.conflicts();
    assert_eq!(conflicts.len(), 1);
    assert_eq!(conflicts[0].winner, "mod-b");
    assert_eq!(conflicts[0].overridden, vec!["mod-a".to_string()]);

    reg.remove_owner("mod-b");
    assert_eq!(reg.active_for_target(tgi).unwrap().owner, "mod-a");
    assert!(reg.conflicts().is_empty());
}
