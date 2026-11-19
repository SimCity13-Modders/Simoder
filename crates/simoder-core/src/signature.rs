//! Validated IDA-style byte signature parser and exact/unique memory scanner.

use crate::CoreError;

/// One byte constraint inside a signature pattern: concrete `Some(byte)` or wildcard `None`.
pub type SignatureByte = Option<u8>;

/// Parsed IDA-style hexadecimal byte signature supporting `?` and `??` wildcards.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Signature {
    bytes: Vec<SignatureByte>,
}

/// Outcome classification when resolving a unique pattern match inside a binary image.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UniqueSignatureStatus {
    /// Exactly one match was found at `offset`.
    Matched,
    /// Pattern did not appear in the scanned byte range.
    NotFound,
    /// Two or more matches were found (`match_count > 1`).
    Ambiguous,
}

/// Result of resolving a unique byte pattern match, including total observed `match_count`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct UniqueSignatureResult {
    /// Match classification.
    pub status: UniqueSignatureStatus,
    /// Zero-based byte offset when `status` is [`UniqueSignatureStatus::Matched`], otherwise `0`.
    pub offset: usize,
    /// Total number of matching positions discovered.
    pub match_count: usize,
}

impl Signature {
    /// Parse a whitespace-delimited IDA-style signature string such as `"8B 45 ?? 89"`.
    ///
    /// Returns [`CoreError::InvalidSignature`] if empty or if any concrete token is not two hex digits.
    pub fn parse(text: &str) -> Result<Self, CoreError> {
        let mut bytes = Vec::new();
        for token in text.split_ascii_whitespace() {
            if token == "?" || token == "??" {
                bytes.push(None);
                continue;
            }
            if token.len() != 2 || !token.is_ascii() {
                return Err(CoreError::InvalidSignature(
                    "every concrete signature token must contain exactly two hex digits".to_string(),
                ));
            }
            let value = u8::from_str_radix(token, 16).map_err(|_| {
                CoreError::InvalidSignature(format!("invalid hexadecimal byte '{token}'"))
            })?;
            bytes.push(Some(value));
        }
        if bytes.is_empty() {
            return Err(CoreError::InvalidSignature(
                "signature must not be empty".to_string(),
            ));
        }
        Ok(Self { bytes })
    }

    /// Borrow the parsed slice of concrete byte values and wildcard positions.
    pub fn bytes(&self) -> &[SignatureByte] {
        &self.bytes
    }

    /// Return all zero-based starting offsets where this signature matches `haystack`.
    pub fn find_all(&self, haystack: &[u8]) -> Vec<usize> {
        let len = self.bytes.len();
        if len == 0 || len > haystack.len() {
            return Vec::new();
        }
        haystack
            .windows(len)
            .enumerate()
            .filter_map(|(offset, window)| self.matches_window(window).then_some(offset))
            .collect()
    }

    /// Resolve this signature only when it appears at a single unambiguous offset in `haystack`.
    pub fn find_unique(&self, haystack: &[u8]) -> UniqueSignatureResult {
        let matches = self.find_all(haystack);
        match matches.as_slice() {
            [] => UniqueSignatureResult {
                status: UniqueSignatureStatus::NotFound,
                offset: 0,
                match_count: 0,
            },
            [only_offset] => UniqueSignatureResult {
                status: UniqueSignatureStatus::Matched,
                offset: *only_offset,
                match_count: 1,
            },
            many => UniqueSignatureResult {
                status: UniqueSignatureStatus::Ambiguous,
                offset: 0,
                match_count: many.len(),
            },
        }
    }

    fn matches_window(&self, window: &[u8]) -> bool {
        self.bytes
            .iter()
            .zip(window.iter())
            .all(|(expected, &actual)| match expected {
                Some(byte) => *byte == actual,
                None => true,
            })
    }
}
