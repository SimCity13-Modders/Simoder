#![forbid(unsafe_code)]

//! Universal future-proof binary symbol & global-pointer resolution for Simoder.
//!
//! Centralizes fail-closed resolution strategies across all native bridges and hooks:
//! - [`ResolutionStrategy::VerifiedHint`]: O(1) prefix check at a known RVA.
//! - [`ResolutionStrategy::UniqueSignature`]: Masked IDA-style byte scan requiring exactly 1 match.
//! - [`ResolutionStrategy::GlobalPtrFromSignature`]: Unique masked scan extracting an `imm32` global pointer.
//! - [`ResolutionStrategy::StringXrefGetter`]: `.rdata` string anchor -> unique `push imm32` -> unique `A1 [ptr] C3` getter.
//! - [`ResolutionStrategy::HintThenFallback`]: Verified RVA fast-path with automatic fallback on shifted builds.

use crate::{Signature, UniqueSignatureStatus};

/// Borrowed view of a loaded 32-bit PE module's `.text` and `.rdata` sections.
#[derive(Debug, Clone, Copy)]
pub struct LoadedModuleView<'a> {
    /// Runtime base address where the module is mapped (`HMODULE`).
    pub image_base: u32,
    /// Relative virtual address of the `.text` section.
    pub text_rva: u32,
    /// Raw bytes of the `.text` section.
    pub text: &'a [u8],
    /// Relative virtual address of the `.rdata` section.
    pub rdata_rva: u32,
    /// Raw bytes of the `.rdata` section.
    pub rdata: &'a [u8],
}

/// Resolution mechanism that succeeded for a [`ResolvedSymbol`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResolutionMethod {
    /// Fast-path RVA matched its expected byte prefix.
    VerifiedHint,
    /// Unambiguous masked signature match in `.text`.
    UniqueSignature,
    /// Global `imm32` pointer extracted from a unique masked signature in `.text`.
    GlobalPtrSignature,
    /// Global `imm32` pointer extracted via `.rdata` string -> `push imm32` -> `A1 [ptr] C3` getter.
    StringXrefGetter,
}

/// Resolved runtime virtual address and the strategy that verified it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ResolvedSymbol {
    /// Resolved runtime virtual address.
    pub va: u32,
    /// Strategy that produced this address.
    pub method: ResolutionMethod,
}

/// Detailed addresses produced by [`resolve_string_xref_getter`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ResolvedStringXrefGetter {
    /// Virtual address of the unique NUL-terminated anchor string in `.rdata`.
    pub string_va: u32,
    /// Virtual address of the unique `push imm32` (`0x68`) referencing [`Self::string_va`].
    pub xref_va: u32,
    /// Virtual address of the `mov eax, [global]; ret` (`A1 .. C3`) getter.
    pub getter_va: u32,
    /// Virtual address of the global pointer read by [`Self::getter_va`].
    pub global_ptr_va: u32,
}

/// Declarative symbol resolution strategy evaluated against a [`LoadedModuleView`].
#[derive(Debug, Clone, Copy)]
pub enum ResolutionStrategy<'a> {
    /// Verify `expected_prefix` at `image_base + rva` inside `.text`.
    VerifiedHint {
        /// Expected RVA relative to `image_base`.
        rva: u32,
        /// Exact prefix bytes expected at `rva`.
        expected_prefix: &'a [u8],
    },
    /// Scan `.text` for a unique IDA-style masked byte pattern and add `result_offset`.
    UniqueSignature {
        /// Whitespace-separated IDA pattern (supports `?` and `??`).
        pattern: &'a str,
        /// Signed byte offset added to the matched offset (usually `0`).
        result_offset: isize,
    },
    /// Scan `.text` for a unique IDA-style masked byte pattern and read a 32-bit LE pointer at `imm32_offset`.
    GlobalPtrFromSignature {
        /// Whitespace-separated IDA pattern (supports `?` and `??`).
        pattern: &'a str,
        /// Byte offset within the matched pattern where the 4-byte `imm32` pointer begins.
        imm32_offset: usize,
    },
    /// Resolve a global pointer via a unique `.rdata` string anchor -> `push imm32` -> `call (A1 [ptr] C3)`.
    StringXrefGetter {
        /// Unique byte anchor in `.rdata` (typically NUL-terminated ASCII literal).
        anchor: &'a [u8],
        /// Backward byte window before `push imm32` to search for `call rel32`.
        lookback_bytes: usize,
    },
    /// Try `VerifiedHint` first; if the build shifted, automatically evaluate `fallback`.
    HintThenFallback {
        /// Fast-path RVA relative to `image_base`.
        hint_rva: u32,
        /// Expected prefix bytes at `hint_rva`.
        hint_prefix: &'a [u8],
        /// Fallback strategy executed when the hint prefix does not match.
        fallback: &'a ResolutionStrategy<'a>,
    },
}

/// Resolve a runtime virtual address using `strategy` against `view`.
///
/// Fails closed (`None`) if any pattern is invalid, absent, or matches more than once.
pub fn resolve_symbol(
    view: &LoadedModuleView<'_>,
    strategy: &ResolutionStrategy<'_>,
) -> Option<ResolvedSymbol> {
    match *strategy {
        ResolutionStrategy::VerifiedHint {
            rva,
            expected_prefix,
        } => {
            if expected_prefix.is_empty() || rva < view.text_rva {
                return None;
            }
            let off = usize::try_from(rva - view.text_rva).ok()?;
            let actual = view.text.get(off..off.checked_add(expected_prefix.len())?)?;
            if actual == expected_prefix {
                Some(ResolvedSymbol {
                    va: view.image_base.checked_add(rva)?,
                    method: ResolutionMethod::VerifiedHint,
                })
            } else {
                None
            }
        }
        ResolutionStrategy::UniqueSignature {
            pattern,
            result_offset,
        } => {
            let sig = Signature::parse(pattern).ok()?;
            let res = sig.find_unique(view.text);
            if res.status != UniqueSignatureStatus::Matched {
                return None;
            }
            let adjusted_off = res.offset.checked_add_signed(result_offset)?;
            if adjusted_off >= view.text.len() {
                return None;
            }
            let va = view
                .image_base
                .checked_add(view.text_rva)?
                .checked_add(u32::try_from(adjusted_off).ok()?)?;
            Some(ResolvedSymbol {
                va,
                method: ResolutionMethod::UniqueSignature,
            })
        }
        ResolutionStrategy::GlobalPtrFromSignature {
            pattern,
            imm32_offset,
        } => {
            let sig = Signature::parse(pattern).ok()?;
            let res = sig.find_unique(view.text);
            if res.status != UniqueSignatureStatus::Matched {
                return None;
            }
            let ptr_off = res.offset.checked_add(imm32_offset)?;
            let bytes: [u8; 4] = view.text.get(ptr_off..ptr_off.checked_add(4)?)?.try_into().ok()?;
            Some(ResolvedSymbol {
                va: u32::from_le_bytes(bytes),
                method: ResolutionMethod::GlobalPtrSignature,
            })
        }
        ResolutionStrategy::StringXrefGetter {
            anchor,
            lookback_bytes,
        } => resolve_string_xref_getter(view, anchor, lookback_bytes).map(|r| ResolvedSymbol {
            va: r.global_ptr_va,
            method: ResolutionMethod::StringXrefGetter,
        }),
        ResolutionStrategy::HintThenFallback {
            hint_rva,
            hint_prefix,
            fallback,
        } => resolve_symbol(
            view,
            &ResolutionStrategy::VerifiedHint {
                rva: hint_rva,
                expected_prefix: hint_prefix,
            },
        )
        .or_else(|| resolve_symbol(view, fallback)),
    }
}

/// Resolve a global pointer via unique `.rdata` `anchor` -> unique `push imm32` -> unique `call (A1 [ptr] C3)`.
///
/// Fails closed (`None`) if any step finds `0` or `> 1` distinct matches.
pub fn resolve_string_xref_getter(
    view: &LoadedModuleView<'_>,
    anchor: &[u8],
    lookback_bytes: usize,
) -> Option<ResolvedStringXrefGetter> {
    if anchor.is_empty() {
        return None;
    }
    let string_va = find_unique_bytes_va(view.image_base, view.rdata_rva, view.rdata, anchor)?;
    let imm = string_va.to_le_bytes();
    let push_needle = [0x68, imm[0], imm[1], imm[2], imm[3]];
    let (xref_off, xref_va) = find_unique_push_imm32(view, &push_needle)?;

    let text_va = view.image_base.checked_add(view.text_rva)?;
    let text_len = u32::try_from(view.text.len()).ok()?;
    let text_end_va = text_va.checked_add(text_len)?;

    let win_start = xref_off.saturating_sub(lookback_bytes);
    let win_end = xref_off.saturating_sub(4);
    if win_start >= win_end {
        return None;
    }

    let mut unique_match: Option<(u32, u32)> = None;
    for call_off in win_start..win_end {
        if view.text.get(call_off) != Some(&0xE8) {
            continue;
        }
        let rel_bytes: [u8; 4] = view.text.get(call_off + 1..call_off + 5)?.try_into().ok()?;
        let rel32 = i32::from_le_bytes(rel_bytes);
        let call_va = text_va.checked_add(u32::try_from(call_off).ok()?)?;
        let next_va = call_va.checked_add(5)?;
        let tgt_va = next_va.checked_add_signed(rel32)?;

        if tgt_va < text_va || tgt_va > text_end_va.checked_sub(6)? {
            continue;
        }
        let tgt_off = usize::try_from(tgt_va - text_va).ok()?;
        let stub = view.text.get(tgt_off..tgt_off + 6)?;
        if stub[0] != 0xA1 || stub[5] != 0xC3 {
            continue;
        }
        let global_ptr_va = u32::from_le_bytes(stub[1..5].try_into().ok()?);
        match unique_match {
            None => unique_match = Some((tgt_va, global_ptr_va)),
            Some(prev) if prev == (tgt_va, global_ptr_va) => {}
            Some(_) => return None,
        }
    }

    let (getter_va, global_ptr_va) = unique_match?;
    Some(ResolvedStringXrefGetter {
        string_va,
        xref_va,
        getter_va,
        global_ptr_va,
    })
}

fn find_unique_bytes_va(image_base: u32, sec_rva: u32, sec: &[u8], needle: &[u8]) -> Option<u32> {
    let mut found_off = None;
    for (idx, window) in sec.windows(needle.len()).enumerate() {
        if window == needle {
            if found_off.is_some() {
                return None;
            }
            found_off = Some(idx);
        }
    }
    let off = u32::try_from(found_off?).ok()?;
    image_base.checked_add(sec_rva)?.checked_add(off)
}

fn find_unique_push_imm32(view: &LoadedModuleView<'_>, needle: &[u8; 5]) -> Option<(usize, u32)> {
    let mut found_off = None;
    for (idx, window) in view.text.windows(needle.len()).enumerate() {
        if window == needle {
            if found_off.is_some() {
                return None;
            }
            found_off = Some(idx);
        }
    }
    let off = found_off?;
    let va = view
        .image_base
        .checked_add(view.text_rva)?
        .checked_add(u32::try_from(off).ok()?)?;
    Some((off, va))
}
