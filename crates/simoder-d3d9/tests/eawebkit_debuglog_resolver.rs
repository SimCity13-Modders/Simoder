use simoder_d3d9::{ResolvedEaWebKitClientGlobal, resolve_eawebkit_client_global};

fn build_synthetic_sections(
    image_base: u32,
    text_rva: u32,
    rdata_rva: u32,
    client_global_ptr_va: u32,
) -> (Vec<u8>, Vec<u8>, u32, u32, u32) {
    let mut rdata = vec![0u8; 0x200];
    let str_off = 0x40usize;
    let anchor = b"%s %s (%d): %s\0";
    rdata[str_off..str_off + anchor.len()].copy_from_slice(anchor);
    let console_format_va = image_base + rdata_rva + str_off as u32;

    let mut text = vec![0x90u8; 0x300];
    // Place `GetEAWebKitClient` at +0x020: `mov eax, [client_global_ptr_va]; ret`
    let get_client_off = 0x020usize;
    let get_client_va = image_base + text_rva + get_client_off as u32;
    text[get_client_off] = 0xA1;
    text[get_client_off + 1..get_client_off + 5]
        .copy_from_slice(&client_global_ptr_va.to_le_bytes());
    text[get_client_off + 5] = 0xC3;

    // Place `call GetEAWebKitClient` at +0x120 inside `addMessageToConsole`
    let call_off = 0x120usize;
    let call_va = image_base + text_rva + call_off as u32;
    let rel32 = (get_client_va as i64 - (call_va as i64 + 5)) as i32;
    text[call_off] = 0xE8;
    text[call_off + 1..call_off + 5].copy_from_slice(&rel32.to_le_bytes());

    // Place `push console_format_va` at +0x1E0 (< 0x100 bytes after call_off)
    let xref_off = 0x1E0usize;
    let console_xref_va = image_base + text_rva + xref_off as u32;
    text[xref_off] = 0x68;
    text[xref_off + 1..xref_off + 5].copy_from_slice(&console_format_va.to_le_bytes());

    (
        text,
        rdata,
        console_format_va,
        console_xref_va,
        get_client_va,
    )
}

#[test]
fn resolves_unique_string_xref_and_client_global_across_aslr_bases() {
    let image_base = 0x57FA_0000;
    let text_rva = 0x0000_1000;
    let rdata_rva = 0x0077_5000;
    let client_global = image_base + 0x008B_2F34;

    let (text, rdata, fmt_va, xref_va, get_client_va) =
        build_synthetic_sections(image_base, text_rva, rdata_rva, client_global);

    let resolved = resolve_eawebkit_client_global(image_base, text_rva, &text, rdata_rva, &rdata)
        .expect("unique String-XREF anchor must resolve");

    assert_eq!(
        resolved,
        ResolvedEaWebKitClientGlobal {
            console_format_va: fmt_va,
            console_xref_va: xref_va,
            get_client_va,
            client_global_ptr_va: client_global,
        }
    );
}

#[test]
fn fails_closed_when_anchor_string_is_missing_or_duplicated() {
    let image_base = 0x1000_0000;
    let text_rva = 0x1000;
    let rdata_rva = 0x2000;
    let client_global = 0x1000_9000;

    let (text, mut rdata, _, _, _) =
        build_synthetic_sections(image_base, text_rva, rdata_rva, client_global);

    // Duplicate string in .rdata -> None
    let anchor = b"%s %s (%d): %s\0";
    rdata[0x100..0x100 + anchor.len()].copy_from_slice(anchor);
    assert_eq!(
        resolve_eawebkit_client_global(image_base, text_rva, &text, rdata_rva, &rdata),
        None
    );

    // Missing string -> None
    let empty_rdata = vec![0u8; 0x200];
    assert_eq!(
        resolve_eawebkit_client_global(image_base, text_rva, &text, rdata_rva, &empty_rdata),
        None
    );
}

#[test]
fn fails_closed_when_push_xref_is_duplicated_or_callee_ambiguous() {
    let image_base = 0x1000_0000;
    let text_rva = 0x1000;
    let rdata_rva = 0x2000;
    let client_global = 0x1000_9000;

    let (mut text, rdata, fmt_va, _, _) =
        build_synthetic_sections(image_base, text_rva, rdata_rva, client_global);

    // Duplicate `push fmt_va` -> None
    let mut dup_xref_text = text.clone();
    dup_xref_text[0x240] = 0x68;
    dup_xref_text[0x241..0x245].copy_from_slice(&fmt_va.to_le_bytes());
    assert_eq!(
        resolve_eawebkit_client_global(image_base, text_rva, &dup_xref_text, rdata_rva, &rdata),
        None
    );

    // Second distinct `A1 ?? ?? ?? ?? C3` callee in the lookback window -> None
    let second_getter_off = 0x050usize;
    let second_getter_va = image_base + text_rva + second_getter_off as u32;
    text[second_getter_off] = 0xA1;
    text[second_getter_off + 1..second_getter_off + 5]
        .copy_from_slice(&0x1000_9999u32.to_le_bytes());
    text[second_getter_off + 5] = 0xC3;

    let call2_off = 0x140usize;
    let call2_va = image_base + text_rva + call2_off as u32;
    let rel2 = (second_getter_va as i64 - (call2_va as i64 + 5)) as i32;
    text[call2_off] = 0xE8;
    text[call2_off + 1..call2_off + 5].copy_from_slice(&rel2.to_le_bytes());
    assert_eq!(
        resolve_eawebkit_client_global(image_base, text_rva, &text, rdata_rva, &rdata),
        None
    );

    // Zero matching callees -> None
    let mut no_callee_text = vec![0x90u8; 0x300];
    no_callee_text[0x1E0] = 0x68;
    no_callee_text[0x1E1..0x1E5].copy_from_slice(&fmt_va.to_le_bytes());
    assert_eq!(
        resolve_eawebkit_client_global(image_base, text_rva, &no_callee_text, rdata_rva, &rdata),
        None
    );
}
