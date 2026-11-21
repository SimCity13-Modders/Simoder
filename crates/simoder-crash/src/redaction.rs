//! Crash report pre-clipboard redaction validation (`CRASH-004`), reusing producer redaction from [`simoder_core`].

pub use simoder_core::{ENV_USER_PREFIXES, redact_sensitive_text};

use crate::types::CrashError;

/// Validate that a shareable report string contains no residual user profile paths or unredacted bearer/token secrets.
pub fn validate_shareable_redaction(text: &str) -> Result<(), CrashError> {
    let lower = text.to_ascii_lowercase();
    if lower.contains(":\\users\\") || lower.contains(":/users/") {
        return Err(CrashError::RedactionViolation(
            "detected unredacted Windows user profile path".into(),
        ));
    }
    if lower.contains("/home/") {
        return Err(CrashError::RedactionViolation(
            "detected unredacted POSIX home directory path".into(),
        ));
    }
    for env_prefix in ENV_USER_PREFIXES {
        if lower.contains(env_prefix) {
            return Err(CrashError::RedactionViolation(format!(
                "detected unredacted environment profile variable '{env_prefix}'"
            )));
        }
    }
    let re_redacted = redact_sensitive_text(text);
    if re_redacted != text {
        return Err(CrashError::RedactionViolation(
            "shareable text still changes under producer redaction pass".into(),
        ));
    }
    Ok(())
}
