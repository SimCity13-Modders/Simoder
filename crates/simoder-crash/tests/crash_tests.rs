//! Integration tests for `simoder-crash` redaction, symbolication, fault isolation,
//! full-dump consent gating, window actions, fallback directories, and retention rotation.

use std::fs;

use simoder_crash::{
    CrashActionResponse, CrashError, CrashHandlerCoordinator, CrashReport, CrashWindowAction,
    DumpCollectionConsent, ExceptionCode, ExceptionContext, FaultCaptureRequest,
    FaultClassification, LoadedModule, MAX_RETAINED_CRASH_REPORTS, MemoryAccessKind,
    RETAIL_MINIDUMP_HANDLER_VA, RETAIL_MINIDUMP_RETURN_VA, RETAIL_SIMCITY_EXE_SHA256,
    SymbolCatalog, SymbolStatus, UnwindCompleteness, redact_sensitive_text,
    validate_shareable_redaction,
};

fn sample_exception_context(code: ExceptionCode, eip: u32) -> ExceptionContext {
    ExceptionContext {
        raw_exception_code: code.code(),
        known_code: Some(code),
        faulting_thread_id: 16476,
        exception_address: eip,
        access_kind: Some(MemoryAccessKind::Read),
        access_target_address: Some(0x41D8_0604),
        eip,
        esp: 0x740D_FF50,
        ebp: 0x740D_FF80,
        gpr: [0, 1, 2, 3, 4, 5],
    }
}

#[test]
fn redacts_user_profiles_verbatim_posix_env_and_tokens_while_preserving_game_paths() {
    let raw = r#"Crash at C:\Users\dawid\Documents\SimCity\dump.dmp verbatim=\\?\C:\Users\anna\Desktop\mod.toml posix=/home/marcin/simcity env=%LOCALAPPDATA%\Simoder Authorization: Bearer "abc.123.xyz" GITEA_ACCESS_TOKEN=secret123 api_key=mykey99 json={"home":"C:\Users\bob"} path=C:\Games\SimCity\SimCity.exe"#;
    let clean = redact_sensitive_text(raw);
    for forbidden in ["dawid", "anna", "marcin", "bob", "abc.123.xyz", "secret123", "mykey99"] {
        assert!(!clean.contains(forbidden), "leaked '{forbidden}' in: {clean}");
    }
    assert!(clean.contains(r"<USERPROFILE>\Documents\SimCity\dump.dmp"));
    assert!(clean.contains(r"<USERPROFILE>\Desktop\mod.toml"));
    assert!(clean.contains("<USERPROFILE>/simcity"));
    assert!(clean.contains(r"<USERPROFILE>\Simoder"));
    assert!(clean.contains(r#"json={"home":"<USERPROFILE>"}"#));
    assert!(clean.contains(r#"Bearer "<REDACTED>""#));
    assert!(clean.contains("GITEA_ACCESS_TOKEN=<REDACTED>"));
    assert!(clean.contains("api_key=<REDACTED>"));
    assert!(clean.contains(r"C:\Games\SimCity\SimCity.exe"));
    validate_shareable_redaction(&clean).unwrap();
}

#[test]
fn redacts_forward_slash_handles_multibyte_utf8_and_validates_shareable_schema() {
    let raw = r#"Saved to c:/Users/alice/AppData/Local/Simoder/log.txt alongside C:\temp\zażółć/file.txt"#;
    let redacted = redact_sensitive_text(raw);
    assert_eq!(
        redacted,
        r#"Saved to <USERPROFILE>/AppData/Local/Simoder/log.txt alongside C:\temp\zażółć/file.txt"#
    );
    validate_shareable_redaction(&redacted).unwrap();
    assert!(matches!(
        validate_shareable_redaction(r"Leaked C:\Users\dawid\secret.txt"),
        Err(CrashError::RedactionViolation(_))
    ));

    let report = CrashReport {
        summary: redact_sensitive_text(r#"Fault at C:\Users\dawid\bin"#),
        frames: vec!["simoder_d3d9!init".to_string()],
        redacted_notes: redact_sensitive_text("Bearer token_xyz"),
        build_sha256: Some(RETAIL_SIMCITY_EXE_SHA256.to_string()),
        exception_context: None,
        structured_frames: Vec::new(),
        unwind_completeness: Some(UnwindCompleteness::Complete),
        ownership_disclaimer: String::new(),
        log_tail: Vec::new(),
    };
    let json = report.to_shareable_json().unwrap();
    let decoded: CrashReport = serde_json::from_str(&json).unwrap();
    assert_eq!(decoded, report);
}

#[test]
fn coordinator_rejects_isolated_script_errors_and_symbolizes_exact_vs_mismatched_builds() {
    let dir = tempfile::tempdir().unwrap();
    let coordinator = CrashHandlerCoordinator::new(
        dir.path().join("crashes"),
        dir.path().join("fallback_crashes"),
        dir.path().join("logs"),
        SymbolCatalog::with_retail_defaults(),
    );
    assert!(!coordinator.network_upload_authorized());

    // Isolated script faults must never trigger the native fatal crash handler (CRASH-002).
    let script_req = FaultCaptureRequest {
        classification: FaultClassification::IsolatedScriptError {
            mod_id: "author.budget-ui".into(),
            generation: 2,
            diagnostic: "runtime error in onTick".into(),
        },
        context: sample_exception_context(ExceptionCode::AccessViolation, 0x0040_1000),
        raw_return_addresses: vec![],
        unwind: UnwindCompleteness::Complete,
        loaded_modules: vec![],
        notes: String::new(),
        log_tail: vec![],
    };
    assert!(matches!(
        coordinator.capture_fault(&script_req),
        Err(CrashError::IsolatedScriptFaultNotFatal { .. })
    ));

    // Exact build SHA-256 resolves symbol; mismatched build suppresses symbol names; unmapped address stays explicit.
    let modules = vec![
        LoadedModule {
            name: "SimCity.exe".into(),
            path: redact_sensitive_text(r"C:\Games\SimCity\SimCity.exe"),
            base_address: 0x0040_0000,
            size_bytes: 0x0100_0000,
            sha256: Some(RETAIL_SIMCITY_EXE_SHA256.to_string()),
        },
        LoadedModule {
            name: "EAWebKit.dll".into(),
            path: redact_sensitive_text(r"C:\Games\SimCity\EAWebKit.dll"),
            base_address: 0x38FE_0000,
            size_bytes: 0x00A0_0000,
            sha256: Some("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff".into()),
        },
        LoadedModule {
            name: "d3d9.dll".into(),
            path: redact_sensitive_text(r"C:\Users\dawid\Games\SimCity\d3d9.dll"),
            base_address: 0x1000_0000,
            size_bytes: 0x0008_0000,
            sha256: None,
        },
    ];

    let fatal_req = FaultCaptureRequest {
        classification: FaultClassification::NativeFatal {
            code: ExceptionCode::AccessViolation,
        },
        context: sample_exception_context(
            ExceptionCode::AccessViolation,
            RETAIL_MINIDUMP_HANDLER_VA + 7,
        ),
        raw_return_addresses: vec![
            0x38FE_0000 + 0x005A_C498, // EAWebKit with mismatched build SHA-256
            0x1000_1234,               // d3d9.dll without catalog symbols (module+offset only)
            0x41D8_0604,               // unmapped address (e.g. unloaded agent DLL)
        ],
        unwind: UnwindCompleteness::Complete,
        loaded_modules: modules,
        notes: r"Loaded config from C:\Users\dawid\AppData\Local\Simoder with token=topsecret99"
            .into(),
        log_tail: vec![r"200 | LOADER | bootstrap | INFO | 1 | Profile C:\Users\dawid\SimCity".into()],
    };

    let report = coordinator.capture_fault(&fatal_req).unwrap();
    assert_eq!(report.structured_frames.len(), 4);
    assert_eq!(report.structured_frames[0].status, SymbolStatus::ExactBuildSymbol);
    assert_eq!(
        report.structured_frames[0].symbol_name.as_deref(),
        Some("SimCity::WriteCrashMiniDump")
    );
    assert_eq!(report.structured_frames[0].symbol_displacement, Some(7));

    assert_eq!(report.structured_frames[1].status, SymbolStatus::BuildIdMismatch);
    assert!(report.structured_frames[1].symbol_name.is_none());

    assert_eq!(report.structured_frames[2].status, SymbolStatus::ModuleOffsetOnly);
    assert!(report.structured_frames[2].rendered.contains("d3d9.dll+0x00001234"));

    assert_eq!(report.structured_frames[3].status, SymbolStatus::UnmappedAddress);
    assert!(report.structured_frames[3].rendered.contains("<unmapped>"));

    let shareable = report.to_shareable_text().unwrap();
    assert!(!shareable.contains("dawid"));
    assert!(!shareable.contains("topsecret99"));
    assert!(shareable.contains("SimCity.exe!SimCity::WriteCrashMiniDump+0x7"));
    assert!(shareable.contains("does not by itself prove mod or engine fault ownership"));
}

#[test]
fn coordinator_enforces_stack_overflow_fallback_full_dump_consent_window_actions_and_rotation() {
    let dir = tempfile::tempdir().unwrap();
    // Create a regular file at `primary_blocker` so creating a directory inside it fails and triggers fallback_crash_dir!
    let primary_blocker = dir.path().join("readonly_file_blocker");
    fs::write(&primary_blocker, b"not a directory").unwrap();
    let blocked_primary_dir = primary_blocker.join("crashes");
    let fallback_dir = dir.path().join("appdata_fallback_crashes");
    let log_dir = dir.path().join("logs");

    let coordinator = CrashHandlerCoordinator::new(
        &blocked_primary_dir,
        &fallback_dir,
        &log_dir,
        SymbolCatalog::with_retail_defaults(),
    );

    let so_req = FaultCaptureRequest {
        classification: FaultClassification::NativeFatal {
            code: ExceptionCode::StackOverflow,
        },
        context: sample_exception_context(ExceptionCode::StackOverflow, 0x005C_FB10),
        raw_return_addresses: vec![0x005C_FE30, 0x005D_4800, 0x0061_8AD0],
        unwind: UnwindCompleteness::Complete,
        loaded_modules: vec![],
        notes: "Stack overflow emergency capture".into(),
        log_tail: vec![],
    };
    let report = coordinator.capture_fault(&so_req).unwrap();
    assert_eq!(
        report.unwind_completeness,
        Some(UnwindCompleteness::StackExhaustedFallback)
    );
    // Stack overflow limits deep walking to EIP + at most 1 immediate return address.
    assert_eq!(report.structured_frames.len(), 2);

    // Full memory dump consent enforcement (CRASH-004).
    assert!(matches!(
        coordinator.write_full_memory_dump_if_consented(
            DumpCollectionConsent::DefaultRedactedReportOnly,
            "001",
            b"MDMP"
        ),
        Err(CrashError::FullDumpConsentRequired)
    ));
    assert!(matches!(
        coordinator.write_full_memory_dump_if_consented(
            DumpCollectionConsent::FullDumpDeclined,
            "001",
            b"MDMP"
        ),
        Err(CrashError::FullDumpConsentRequired)
    ));
    assert!(matches!(
        coordinator.write_full_memory_dump_if_consented(
            DumpCollectionConsent::FullMemoryDumpConsented {
                warning_acknowledged: false
            },
            "001",
            b"MDMP"
        ),
        Err(CrashError::FullDumpConsentRequired)
    ));

    let dump_desc = coordinator
        .write_full_memory_dump_if_consented(
            DumpCollectionConsent::FullMemoryDumpConsented {
                warning_acknowledged: true,
            },
            "001",
            b"MDMP_PAYLOAD",
        )
        .unwrap();
    assert!(dump_desc.dump_path.starts_with(&fallback_dir));
    assert_eq!(dump_desc.retail_handler_va, RETAIL_MINIDUMP_HANDLER_VA);
    assert_eq!(dump_desc.retail_return_va, RETAIL_MINIDUMP_RETURN_VA);
    assert!(!dump_desc.uploaded_over_network);

    // All three crash window actions (CRASH-004).
    match coordinator
        .execute_window_action(CrashWindowAction::CopyShareableReportToClipboard, &report)
        .unwrap()
    {
        CrashActionResponse::ClipboardPayload(text) => {
            assert!(text.contains("STACK_OVERFLOW"));
            validate_shareable_redaction(&text).unwrap();
        }
        other => panic!("expected ClipboardPayload, got {other:?}"),
    }

    match coordinator
        .execute_window_action(CrashWindowAction::OpenCrashReportFolder, &report)
        .unwrap()
    {
        CrashActionResponse::OpenedLocalFolder {
            path,
            transmitted_over_network,
        } => {
            assert_eq!(path, fallback_dir);
            assert!(!transmitted_over_network);
        }
        other => panic!("expected OpenedLocalFolder, got {other:?}"),
    }

    match coordinator
        .execute_window_action(CrashWindowAction::OpenLogFolder, &report)
        .unwrap()
    {
        CrashActionResponse::OpenedLocalFolder {
            path,
            transmitted_over_network,
        } => {
            assert_eq!(path, log_dir);
            assert!(!transmitted_over_network);
        }
        other => panic!("expected OpenedLocalFolder, got {other:?}"),
    }

    // Persist 20 reports into fallback_dir and verify rotation retains at most MAX_RETAINED_CRASH_REPORTS (16).
    for idx in 0..20 {
        let id = format!("{idx:04}");
        let written = coordinator.persist_shareable_report(&report, &id).unwrap();
        assert!(written.starts_with(&fallback_dir));
    }
    let retained_json_count = fs::read_dir(&fallback_dir)
        .unwrap()
        .filter_map(Result::ok)
        .filter(|e| {
            e.file_name()
                .to_str()
                .is_some_and(|n| n.starts_with("crash-") && n.ends_with(".json"))
        })
        .count();
    assert_eq!(retained_json_count, MAX_RETAINED_CRASH_REPORTS);
}
