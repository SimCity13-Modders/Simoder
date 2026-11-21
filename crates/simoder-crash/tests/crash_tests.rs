//! Integration tests for `simoder-crash` sensitive-data redaction.

use simoder_crash::{CrashReport, redact_sensitive_text};

#[test]
fn redacts_user_profiles_and_tokens_while_preserving_game_paths() {
    let raw = r#"Crash at C:\Users\dawid\Documents\SimCity\dump.dmp Authorization: Bearer "abc.123.xyz" GITEA_ACCESS_TOKEN=secret123 json={"home":"C:\Users\bob"} path=C:\Games\SimCity\SimCity.exe"#;
    let clean = redact_sensitive_text(raw);
    assert!(!clean.contains("dawid"));
    assert!(!clean.contains("bob"));
    assert!(!clean.contains("abc.123.xyz"));
    assert!(!clean.contains("secret123"));
    assert!(clean.contains(r"<USERPROFILE>\Documents\SimCity\dump.dmp"));
    assert!(clean.contains(r#"json={"home":"<USERPROFILE>"}"#));
    assert!(clean.contains(r#"Bearer "<REDACTED>""#));
    assert!(clean.contains("GITEA_ACCESS_TOKEN=<REDACTED>"));
    assert!(clean.contains(r"C:\Games\SimCity\SimCity.exe"));
}

#[test]
fn redacts_forward_slash_and_handles_multibyte_utf8_paths() {
    let raw = r#"Saved to c:/Users/alice/AppData/Local/Simoder/log.txt alongside C:\temp\zażółć/file.txt"#;
    assert_eq!(
        redact_sensitive_text(raw),
        r#"Saved to <USERPROFILE>/AppData/Local/Simoder/log.txt alongside C:\temp\zażółć/file.txt"#
    );

    let report = CrashReport {
        summary: redact_sensitive_text(r#"Fault at C:\Users\dawid\bin"#),
        frames: vec!["simoder_d3d9!init".to_string()],
        redacted_notes: redact_sensitive_text("Bearer token_xyz"),
    };
    let json = serde_json::to_string(&report).unwrap();
    let decoded: CrashReport = serde_json::from_str(&json).unwrap();
    assert_eq!(decoded, report);
}
