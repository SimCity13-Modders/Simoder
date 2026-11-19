#![forbid(unsafe_code)]

//! Crash report representation and sensitive-data redaction for Beta diagnostics.

use serde::{Deserialize, Serialize};

/// Structured Beta crash report payload with sensitive user paths and credentials redacted.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CrashReport {
    /// Human-readable fault summary.
    pub summary: String,
    /// Captured stack frames with sensitive user profile segments redacted.
    pub frames: Vec<String>,
    /// Additional redacted engine/loader notes.
    pub redacted_notes: String,
}

/// Redact Windows user profile paths (`<drive>:\Users\<name>`) and token credentials from text.
pub fn redact_sensitive_text(input: &str) -> String {
    let stage1 = redact_windows_user_paths(input);
    let stage2 = redact_prefixed_secret(&stage1, "bearer ", true);
    redact_prefixed_secret(&stage2, "token=", false)
}

/// Replace Windows user home segments (`X:\Users\<name>` or `X:/Users/<name>`) with `<USERPROFILE>`.
fn redact_windows_user_paths(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = String::with_capacity(input.len());
    let mut i = 0;

    while i < bytes.len() {
        if i + 9 <= bytes.len()
            && bytes[i].is_ascii_alphabetic()
            && bytes[i + 1] == b':'
            && (bytes[i + 2] == b'\\' || bytes[i + 2] == b'/')
            && input[i + 3..].len() >= 6
            && (input[i + 3..i + 9].eq_ignore_ascii_case("users\\")
                || input[i + 3..i + 9].eq_ignore_ascii_case("users/"))
        {
            out.push_str("<USERPROFILE>");
            i += 9;
            while i < bytes.len()
                && bytes[i] != b'\\'
                && bytes[i] != b'/'
                && !bytes[i].is_ascii_whitespace()
            {
                i += 1;
            }
            continue;
        }

        let ch = input[i..].chars().next().unwrap_or('\0');
        out.push(ch);
        i += ch.len_utf8();
    }

    out
}

/// Mask secret values following an ASCII case-insensitive prefix (`needle`) up to whitespace, quote, or delimiter.
fn redact_prefixed_secret(input: &str, needle: &str, require_boundary: bool) -> String {
    let mut out = String::with_capacity(input.len());
    let mut i = 0;
    let needle_len = needle.len();
    let bytes = input.as_bytes();

    while i < input.len() {
        let rest = &input[i..];
        let boundary_ok = !require_boundary
            || i == 0
            || (!bytes[i - 1].is_ascii_alphanumeric() && bytes[i - 1] != b'_');
        let matched = boundary_ok
            && rest.len() >= needle_len
            && rest.is_char_boundary(needle_len)
            && rest[..needle_len].eq_ignore_ascii_case(needle);

        if matched {
            out.push_str(&rest[..needle_len]);
            out.push_str("<REDACTED>");
            i += needle_len;
            while i < input.len() {
                let ch = input[i..].chars().next().unwrap_or(' ');
                if ch.is_whitespace() || matches!(ch, '"' | '\'' | ';' | ',' | '&' | ')') {
                    break;
                }
                i += ch.len_utf8();
            }
            continue;
        }

        let ch = rest.chars().next().unwrap_or('\0');
        out.push(ch);
        i += ch.len_utf8();
    }

    out
}
