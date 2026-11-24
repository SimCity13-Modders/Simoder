use simoder_core::{
    LoadedModuleView, ResolutionMethod, ResolutionStrategy, ResolvedStringXrefGetter,
    ResolvedSymbol, resolve_symbol,
};

#[test]
fn hint_then_fallback_uses_fast_path_when_matched_and_falls_back_when_shifted() {
    let image_base = 0x0123_0000;
    let text_rva = 0x1000;
    let mut text = vec![0x90u8; 0x200];
    let fn_bytes = [0x53, 0x56, 0x32, 0xDB, 0x57, 0x8B, 0xF9, 0x38];

    // Case 1: Function at expected hint RVA 0x1020
    text[0x20..0x20 + fn_bytes.len()].copy_from_slice(&fn_bytes);
    let rdata = [];
    let view = LoadedModuleView {
        image_base,
        text_rva,
        text: &text,
        rdata_rva: 0x2000,
        rdata: &rdata,
    };
    let fallback = ResolutionStrategy::UniqueSignature {
        pattern: "53 56 32 DB 57 8B F9 38",
        result_offset: 0,
    };
    let strategy = ResolutionStrategy::HintThenFallback {
        hint_rva: 0x1020,
        hint_prefix: &fn_bytes[..4],
        fallback: &fallback,
    };

    assert_eq!(
        resolve_symbol(&view, &strategy),
        Some(ResolvedSymbol {
            va: image_base + 0x1020,
            method: ResolutionMethod::VerifiedHint,
        })
    );

    // Case 2: Simulating a game update shifting the function to RVA 0x1090
    let mut shifted_text = vec![0x90u8; 0x200];
    shifted_text[0x90..0x90 + fn_bytes.len()].copy_from_slice(&fn_bytes);
    let shifted_view = LoadedModuleView {
        image_base,
        text_rva,
        text: &shifted_text,
        rdata_rva: 0x2000,
        rdata: &rdata,
    };
    assert_eq!(
        resolve_symbol(&shifted_view, &strategy),
        Some(ResolvedSymbol {
            va: image_base + 0x1090,
            method: ResolutionMethod::UniqueSignature,
        })
    );
}

#[test]
fn unique_signature_fails_closed_on_missing_or_duplicate_matches() {
    let mut text = vec![0xCCu8; 0x100];
    let pat = [0x8B, 0xF1, 0x83, 0xBE, 0xB0, 0x00, 0x00, 0x00];
    let rdata = [];

    // 0 matches -> None
    let empty_view = LoadedModuleView {
        image_base: 0x0040_0000,
        text_rva: 0x1000,
        text: &text,
        rdata_rva: 0x2000,
        rdata: &rdata,
    };
    let strategy = ResolutionStrategy::UniqueSignature {
        pattern: "8B F1 83 BE B0 00 00 00",
        result_offset: 0,
    };
    assert_eq!(resolve_symbol(&empty_view, &strategy), None);

    // 2 matches -> None
    text[0x10..0x18].copy_from_slice(&pat);
    text[0x80..0x88].copy_from_slice(&pat);
    let dup_view = LoadedModuleView {
        image_base: 0x0040_0000,
        text_rva: 0x1000,
        text: &text,
        rdata_rva: 0x2000,
        rdata: &rdata,
    };
    assert_eq!(resolve_symbol(&dup_view, &strategy), None);
}

#[test]
fn global_ptr_from_signature_extracts_runtime_pointer_at_aslr_base() {
    let image_base: u32 = 0x0090_0000;
    let expected_slot: u32 = image_base + 0x00E5_96F8;
    let mut text = vec![0x90u8; 0x80];
    // 8B 42 54 FF D0 39 05 <imm32> 75 10
    text[0x10..0x17].copy_from_slice(&[0x8B, 0x42, 0x54, 0xFF, 0xD0, 0x39, 0x05]);
    text[0x17..0x1B].copy_from_slice(&expected_slot.to_le_bytes());
    text[0x1B..0x1D].copy_from_slice(&[0x75, 0x10]);

    let rdata = [];
    let view = LoadedModuleView {
        image_base,
        text_rva: 0x1000,
        text: &text,
        rdata_rva: 0x2000,
        rdata: &rdata,
    };
    let strategy = ResolutionStrategy::GlobalPtrFromSignature {
        pattern: "8B 42 54 FF D0 39 05 ?? ?? ?? ?? 75 10",
        imm32_offset: 7,
    };
    assert_eq!(
        resolve_symbol(&view, &strategy),
        Some(ResolvedSymbol {
            va: expected_slot,
            method: ResolutionMethod::GlobalPtrSignature,
        })
    );
}

#[test]
fn string_xref_getter_resolves_unique_anchor_and_rejects_duplicates() {
    let image_base: u32 = 0x57FA_0000;
    let text_rva: u32 = 0x1000;
    let rdata_rva: u32 = 0x9000;
    let global_ptr_va = image_base + 0x008B_2F34;

    let mut rdata = vec![0u8; 0x100];
    let anchor = b"%s %s (%d): %s\0";
    rdata[0x20..0x20 + anchor.len()].copy_from_slice(anchor);
    let string_va = image_base + rdata_rva + 0x20;

    let mut text = vec![0x90u8; 0x200];
    let getter_va = image_base + text_rva + 0x10;
    text[0x10] = 0xA1;
    text[0x11..0x15].copy_from_slice(&global_ptr_va.to_le_bytes());
    text[0x15] = 0xC3;

    let call_va = image_base + text_rva + 0x80;
    let rel32 = (getter_va as i64 - (call_va as i64 + 5)) as i32;
    text[0x80] = 0xE8;
    text[0x81..0x85].copy_from_slice(&rel32.to_le_bytes());

    let xref_va = image_base + text_rva + 0xC0;
    text[0xC0] = 0x68;
    text[0xC1..0xC5].copy_from_slice(&string_va.to_le_bytes());

    let view = LoadedModuleView {
        image_base,
        text_rva,
        text: &text,
        rdata_rva,
        rdata: &rdata,
    };

    assert_eq!(
        simoder_core::resolve_string_xref_getter(&view, anchor, 0x100),
        Some(ResolvedStringXrefGetter {
            string_va,
            xref_va,
            getter_va,
            global_ptr_va,
        })
    );
    assert_eq!(
        resolve_symbol(
            &view,
            &ResolutionStrategy::StringXrefGetter {
                anchor,
                lookback_bytes: 0x100,
            }
        ),
        Some(ResolvedSymbol {
            va: global_ptr_va,
            method: ResolutionMethod::StringXrefGetter,
        })
    );

    // Duplicate anchor -> None
    rdata[0x80..0x80 + anchor.len()].copy_from_slice(anchor);
    let dup_view = LoadedModuleView {
        image_base,
        text_rva,
        text: &text,
        rdata_rva,
        rdata: &rdata,
    };
    assert_eq!(
        simoder_core::resolve_string_xref_getter(&dup_view, anchor, 0x100),
        None
    );
}
