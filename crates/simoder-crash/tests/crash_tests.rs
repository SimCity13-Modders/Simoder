//! Integration tests for `simoder-crash` sensitive-data redaction.

use simoder_crash::redact_sensitive_text;

#[test]
fn redacts_user_profiles_and_tokens_while_preserving_game_paths() {
    let raw = r#"Crash at C:\Users\dawid\Documents\SimCity\dump.dmp Authorization: Bearer abc.123.xyz GITEA_ACCESS_TOKEN=secret123 path=C:\Games\SimCity\SimCity.exe"#;
    let clean = redact_sensitive_text(raw);
    assert!(!clean.contains("dawid"));
    assert!(!clean.contains("abc.123.xyz"));
    assert!(!clean.contains("secret123"));
    assert!(clean.contains(r"<USERPROFILE>\Documents\SimCity\dump.dmp"));
    assert!(clean.contains("Bearer <REDACTED>"));
    assert!(clean.contains("GITEA_ACCESS_TOKEN=<REDACTED>"));
    assert!(clean.contains(r"C:\Games\SimCity\SimCity.exe"));
}

#[test]
fn redacts_forward_slash_user_paths() {
    let raw = "Saved to c:/Users/alice/AppData/Local/Simoder/log.txt";
    assert_eq!(
        redact_sensitive_text(raw),
        "Saved to <USERPROFILE>/AppData/Local/Simoder/log.txt"
    );
}
