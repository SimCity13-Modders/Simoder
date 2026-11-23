//! Producer-side sensitive data redaction shared across central logging and crash diagnostics (`CRASH-004`).

/// Secret key prefixes redacted case-insensitively across log streams, bridge traces, and crash reports.
const SECRET_PREFIXES: &[(&str, bool)] = &[
    ("bearer ", true),
    ("basic ", true),
    ("token=", false),
    ("access_token=", false),
    ("api_key=", false),
    ("apikey=", false),
    ("secret=", false),
    ("password=", false),
    ("passwd=", false),
];

/// Environment variable style user profile markers replaced with `<USERPROFILE>`.
pub const ENV_USER_PREFIXES: &[&str] = &["%userprofile%", "%localappdata%", "%appdata%"];

/// Redact Windows/POSIX user profile paths (`<drive>:\Users\<name>`, `\\?\C:\Users\<name>`, `/home/<name>`)
/// and secret tokens/keys/passwords from diagnostic text.
pub fn redact_sensitive_text(input: &str) -> String {
    let mut stage = redact_windows_user_paths(input);
    stage = redact_posix_home_paths(&stage);
    for env_prefix in ENV_USER_PREFIXES {
        stage = redact_case_insensitive_literal(&stage, env_prefix, "<USERPROFILE>");
    }
    for &(prefix, require_boundary) in SECRET_PREFIXES {
        stage = redact_prefixed_secret(&stage, prefix, require_boundary);
    }
    stage
}

/// Replace Windows user home segments (`X:\Users\<name>`, `X:/Users/<name>`, or `\\?\X:\Users\<name>`) with `<USERPROFILE>`.
fn redact_windows_user_paths(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = String::with_capacity(input.len());
    let mut i = 0;

    while i < bytes.len() {
        let verbatim_len = if i + 13 <= bytes.len()
            && (bytes[i..i + 4] == *b"\\\\?\\" || bytes[i..i + 4] == *b"\\\\.\\")
            && bytes[i + 4].is_ascii_alphabetic()
            && bytes[i + 5] == b':'
            && (bytes[i + 6] == b'\\' || bytes[i + 6] == b'/')
            && (bytes[i + 7..i + 13].eq_ignore_ascii_case(b"users\\")
                || bytes[i + 7..i + 13].eq_ignore_ascii_case(b"users/"))
        {
            4
        } else {
            0
        };

        let drive_idx = i + verbatim_len;
        if drive_idx + 9 <= bytes.len()
            && bytes[drive_idx].is_ascii_alphabetic()
            && bytes[drive_idx + 1] == b':'
            && (bytes[drive_idx + 2] == b'\\' || bytes[drive_idx + 2] == b'/')
            && (bytes[drive_idx + 3..drive_idx + 9].eq_ignore_ascii_case(b"users\\")
                || bytes[drive_idx + 3..drive_idx + 9].eq_ignore_ascii_case(b"users/"))
        {
            out.push_str("<USERPROFILE>");
            i = drive_idx + 9;
            while i < bytes.len()
                && !matches!(bytes[i], b'\\' | b'/' | b'"' | b'\'' | b',' | b')' | b';' | b'}')
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

/// Replace `/home/<username>` segments with `<USERPROFILE>`.
fn redact_posix_home_paths(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = String::with_capacity(input.len());
    let mut i = 0;

    while i < bytes.len() {
        let boundary_ok = i == 0
            || matches!(
                bytes[i - 1],
                b' ' | b'\t' | b'\n' | b'\r' | b'"' | b'\'' | b'=' | b'('
            );
        if boundary_ok && i + 6 < bytes.len() && bytes[i..i + 6].eq_ignore_ascii_case(b"/home/") {
            out.push_str("<USERPROFILE>");
            i += 6;
            while i < bytes.len()
                && !matches!(bytes[i], b'/' | b'\\' | b'"' | b'\'' | b',' | b')' | b';' | b'}')
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

fn redact_case_insensitive_literal(input: &str, needle: &str, replacement: &str) -> String {
    let mut out = String::with_capacity(input.len());
    let mut i = 0;
    let needle_len = needle.len();
    let bytes = input.as_bytes();

    while i < bytes.len() {
        let rest = &input[i..];
        if rest.len() >= needle_len
            && rest.is_char_boundary(needle_len)
            && rest[..needle_len].eq_ignore_ascii_case(needle)
        {
            out.push_str(replacement);
            i += needle_len;
            continue;
        }
        let ch = rest.chars().next().unwrap_or('\0');
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
            i += needle_len;
            while i < bytes.len() && bytes[i] == b' ' {
                out.push(' ');
                i += 1;
            }
            if matches!(bytes.get(i), Some(b'"' | b'\'')) {
                out.push(bytes[i] as char);
                i += 1;
            }
            if input[i..].starts_with("<REDACTED>") {
                out.push_str("<REDACTED>");
                i += "<REDACTED>".len();
                continue;
            }
            out.push_str("<REDACTED>");
            while i < input.len() {
                let ch = input[i..].chars().next().unwrap_or(' ');
                if ch.is_whitespace() || matches!(ch, '"' | '\'' | ';' | ',' | '&' | ')' | '}') {
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
