//! Integration tests for `simoder-d3d9` host validation, D3D9 exports, PE32 parsing, COM vftable/shader checks, and bootstrap coordination.

use std::fs;
use std::path::Path;
use std::sync::Arc;

use simoder_d3d9::{
    BootstrapCoordinator, BootstrapOutcome, COFF_MACHINE_X86, D3D9_EXPORT_COUNT, D3d9Error,
    D3d9Export, D3d9ForwardingTable, IMAGE_SCN_MEM_EXECUTE, IntegratedSimoderRuntime, ModuleRange,
    PROXY_DISABLED_MARKER_FILE, Pe32Image, SIMODER_RUST_ABI_VERSION, ShaderKind, all_d3d9_exports,
    assess_pe32_bytes, inspect_shader_blob, is_simcity_executable, lookup_export_by_name,
    lookup_export_by_ordinal, render_module_def, runtime_abi_version, validate_com_vftable,
};
use simoder_formats::DeveloperConsoleConfig;
use simoder_mods::{
    ModToolAnnotations, ModToolDescriptor, NativeModuleCallbacks, NativeModuleRegistration,
    NativeModuleScopeState, SdHandle, SdLifecycleState, SdModuleDescriptorV2, SdResult,
};

fn build_synthetic_pe32() -> Vec<u8> {
    let mut img = vec![0_u8; 0x220];
    img[0] = b'M';
    img[1] = b'Z';
    let nt: usize = 0x80;
    img[0x3C..0x40].copy_from_slice(&(nt as u32).to_le_bytes());
    img[nt..nt + 4].copy_from_slice(b"PE\0\0");
    // COFF FileHeader
    img[nt + 4..nt + 6].copy_from_slice(&COFF_MACHINE_X86.to_le_bytes());
    img[nt + 6..nt + 8].copy_from_slice(&1_u16.to_le_bytes());
    img[nt + 20..nt + 22].copy_from_slice(&96_u16.to_le_bytes());
    // PE32 OptionalHeader (StandardFields32 + WindowsFields32 = 96 bytes)
    img[nt + 24..nt + 26].copy_from_slice(&0x010B_u16.to_le_bytes());
    img[nt + 24 + 16..nt + 24 + 20].copy_from_slice(&0x1000_u32.to_le_bytes());
    img[nt + 24 + 28..nt + 24 + 32].copy_from_slice(&0x0040_0000_u32.to_le_bytes());
    img[nt + 24 + 56..nt + 24 + 60].copy_from_slice(&0x2000_u32.to_le_bytes());
    // Section header .text at nt + 24 + 96 = 0xF8
    let sec = nt + 24 + 96;
    img[sec..sec + 8].copy_from_slice(b".text\0\0\0");
    img[sec + 8..sec + 12].copy_from_slice(&32_u32.to_le_bytes());
    img[sec + 12..sec + 16].copy_from_slice(&0x1000_u32.to_le_bytes());
    img[sec + 16..sec + 20].copy_from_slice(&32_u32.to_le_bytes());
    img[sec + 20..sec + 24].copy_from_slice(&0x200_u32.to_le_bytes());
    img[sec + 36..sec + 40].copy_from_slice(&(IMAGE_SCN_MEM_EXECUTE | 0x20).to_le_bytes());
    // Code at 0x200 (RVA 0x1000, VA 0x0040_1000): E8 rel32 -> 0x0040_2000 (disp = 0x0FFB)
    img[0x200] = 0xE8;
    img[0x201..0x205].copy_from_slice(&0x0FFB_i32.to_le_bytes());
    img[0x205] = 0xC3;
    // Constant 0x1234_5678 at file offset 0x208 (RVA 0x1008)
    img[0x208..0x20C].copy_from_slice(&0x1234_5678_u32.to_le_bytes());
    img
}

#[test]
fn matches_simcity_executable_case_insensitively() {
    assert!(is_simcity_executable(Path::new(
        r"C:\Games\SimCity\SimCity\SimCity.exe"
    )));
    assert!(is_simcity_executable(Path::new("simcity.EXE")));
    assert!(!is_simcity_executable(Path::new(
        r"C:\Games\SimCity\EADesktop.exe"
    )));
    assert!(!is_simcity_executable(Path::new("")));
}

#[test]
fn returns_configured_abi_version() {
    assert_eq!(runtime_abi_version(), SIMODER_RUST_ABI_VERSION);
}

#[test]
fn d3d9_export_table_covers_all_23_ordinals_and_renders_def() {
    let table = all_d3d9_exports();
    assert_eq!(table.len(), D3D9_EXPORT_COUNT);
    assert_eq!(table.first(), Some(&D3d9Export::Ordinal16));
    assert_eq!(table.last(), Some(&D3d9Export::Direct3DCreate9Ex));

    let create = lookup_export_by_name("Direct3DCreate9").unwrap();
    assert_eq!(create, D3d9Export::Direct3DCreate9);
    assert_eq!(create.ordinal(), 37);
    assert_eq!(create.index(), 21);
    assert!(create.triggers_bootstrap());

    let create_ex = lookup_export_by_ordinal(38).unwrap();
    assert_eq!(create_ex, D3d9Export::Direct3DCreate9Ex);
    assert_eq!(create_ex.symbol_name(), Some("Direct3DCreate9Ex"));
    assert!(create_ex.triggers_bootstrap());

    let perf = lookup_export_by_name("D3DPERF_GetStatus").unwrap();
    assert_eq!(perf, D3d9Export::D3DPERF_GetStatus);
    assert_eq!(perf.ordinal(), 29);
    assert!(!perf.triggers_bootstrap());

    let ordinal_only = lookup_export_by_ordinal(16).unwrap();
    assert_eq!(ordinal_only, D3d9Export::Ordinal16);
    assert_eq!(ordinal_only.symbol_name(), None);
    assert_eq!(ordinal_only.index(), 0);

    assert!(lookup_export_by_ordinal(15).is_none());
    assert!(lookup_export_by_ordinal(39).is_none());

    let def = render_module_def();
    assert!(def.contains("Direct3DCreate9=_SimoderDirect3DCreate9@4 @37"));
    assert!(def.contains("Direct3DCreate9Ex=_SimoderDirect3DCreate9Ex@8 @38"));
    assert!(def.contains("SimoderOrdinal16=SimoderProxyOrdinal16 @16 NONAME"));
}

#[test]
fn pe32_parser_validates_sections_rva_mapping_and_e8_relative_calls() {
    let bytes = build_synthetic_pe32();
    let pe = Pe32Image::parse(&bytes).unwrap();
    assert_eq!(pe.machine(), COFF_MACHINE_X86);
    assert_eq!(pe.image_base(), 0x0040_0000);
    assert_eq!(pe.entry_point_rva(), 0x1000);
    assert_eq!(pe.file_offset_to_rva(0x208), Some(0x1008));
    assert_eq!(pe.find_section(".text").map(|s| s.is_executable()), Some(true));
    assert_eq!(
        pe.find_value_in_executable_sections(0x1234_5678, 8),
        vec![0x1008]
    );
    assert_eq!(
        pe.find_relative_call_references(0x0040_2000, 8),
        vec![0x1000]
    );

    let assessment = assess_pe32_bytes(&bytes).unwrap();
    assert_eq!(assessment.machine, COFF_MACHINE_X86);
    assert!(assessment.text_sha256.is_some());
    assert!(!assessment.verified_retail_file);
    assert!(!assessment.verified_retail_text);

    assert!(matches!(Pe32Image::parse(b"NOT_PE"), Err(D3d9Error::InvalidPe(_))));

    let mut overflow_pe = build_synthetic_pe32();
    let sec = 0x80 + 24 + 96;
    overflow_pe[sec + 12..sec + 16].copy_from_slice(&0xFFFF_FFF0_u32.to_le_bytes());
    assert!(matches!(
        Pe32Image::parse(&overflow_pe),
        Err(D3d9Error::InvalidPe(_))
    ));
}

#[test]
fn com_vftable_validator_accepts_rdata_vftables_and_rejects_non_exec_methods() {
    let d3d9_mod = ModuleRange::new(0x6000_0000, 0x0020_0000);
    let d3d9_exec = ModuleRange::new(0x6001_0000, 0x0010_0000);
    let game_mod = ModuleRange::new(0x0040_0000, 0x0100_0000);

    // Valid COM vftable sitting in d3d9 .rdata (outside d3d9_exec) with 5 executable methods.
    let vtable_rdata = 0x6015_0000;
    let exec_methods = [
        0x6001_1000,
        0x6001_1200,
        0x6001_1400,
        0x6001_1600,
        0x6001_1800,
    ];
    assert!(validate_com_vftable(
        vtable_rdata,
        &exec_methods,
        d3d9_mod,
        d3d9_exec,
        game_mod
    ));

    // Reject small integer pointers and methods outside executable section.
    assert!(!validate_com_vftable(
        0x0000_0400,
        &exec_methods,
        d3d9_mod,
        d3d9_exec,
        game_mod
    ));
    let mut bad_methods = exec_methods;
    bad_methods[3] = 0x6015_2000; // points to .rdata instead of executable code
    assert!(!validate_com_vftable(
        vtable_rdata,
        &bad_methods,
        d3d9_mod,
        d3d9_exec,
        game_mod
    ));
}

#[test]
fn shader_blob_inspector_identifies_ps_and_vs_version_tokens() {
    let mut ps_blob = Vec::new();
    ps_blob.extend_from_slice(&64_u32.to_le_bytes());
    ps_blob.extend_from_slice(&0xFFFF_0300_u32.to_le_bytes()); // ps_3_0
    let ps = inspect_shader_blob(&ps_blob).unwrap();
    assert_eq!(ps.kind, ShaderKind::Pixel);
    assert_eq!((ps.total_byte_len, ps.major_version, ps.minor_version), (64, 3, 0));

    let mut vs_blob = Vec::new();
    vs_blob.extend_from_slice(&32_u32.to_le_bytes());
    vs_blob.extend_from_slice(&0xFFFE_0200_u32.to_le_bytes()); // vs_2_0
    let vs = inspect_shader_blob(&vs_blob).unwrap();
    assert_eq!(vs.kind, ShaderKind::Vertex);
    assert_eq!((vs.total_byte_len, vs.major_version, vs.minor_version), (32, 2, 0));

    // Reject vertex declaration / unaligned or unknown prefix blobs
    let mut bad_prefix = Vec::new();
    bad_prefix.extend_from_slice(&32_u32.to_le_bytes());
    bad_prefix.extend_from_slice(&0x1234_0000_u32.to_le_bytes());
    assert!(inspect_shader_blob(&bad_prefix).is_none());
}

#[test]
fn bootstrap_coordinator_enforces_single_owner_marker_and_config_seeding() {
    let dir = tempfile::tempdir().unwrap();
    let game_dir = dir.path().join("SimCity");
    let sys_dir = dir.path().join("System32");
    fs::create_dir_all(&game_dir).unwrap();
    fs::create_dir_all(&sys_dir).unwrap();

    let proxy_dll = game_dir.join("d3d9.dll");
    let simcity_exe = game_dir.join("SimCity.exe");
    fs::write(&simcity_exe, build_synthetic_pe32()).unwrap();

    // Non-SimCity caller stays disabled without creating simoder/config.toon
    let non_game = BootstrapCoordinator::new();
    let other_exe = game_dir.join("EADesktop.exe");
    let non_game_res = non_game
        .try_begin_bootstrap(&proxy_dll, &other_exe, &sys_dir)
        .unwrap();
    assert!(matches!(
        non_game_res,
        BootstrapOutcome::DisabledNonSimCityProcess { executable_name } if executable_name == "EADesktop.exe"
    ));
    assert!(!non_game.is_module_pinned());
    assert!(!dir.path().join("simoder").join("config.toon").exists());

    // Valid SimCity.exe initializes once, pins module, seeds config.toon, assesses PE32, then rejects re-entry
    let coordinator = BootstrapCoordinator::new();
    let first = coordinator
        .try_begin_bootstrap(&proxy_dll, &simcity_exe, &sys_dir)
        .unwrap();
    match first {
        BootstrapOutcome::Initialized {
            paths,
            config,
            build,
        } => {
            assert_eq!(paths.game_root, dir.path());
            assert_eq!(paths.data_root, dir.path().join("simoder"));
            assert_eq!(paths.system_d3d9_path, sys_dir.join("d3d9.dll"));
            assert!(!config.developer_console.enabled);
            assert!(paths.data_root.join("config.toon").is_file());
            assert_eq!(build.unwrap().machine, COFF_MACHINE_X86);
        }
        other => panic!("expected Initialized, got {other:?}"),
    }
    assert!(coordinator.is_requested());
    assert!(coordinator.is_module_pinned());

    assert_eq!(
        coordinator
            .try_begin_bootstrap(&proxy_dll, &simcity_exe, &sys_dir)
            .unwrap(),
        BootstrapOutcome::SkippedAlreadyRequested
    );

    // proxy.disabled marker suppresses initialization cleanly
    fs::write(dir.path().join("simoder").join(PROXY_DISABLED_MARKER_FILE), b"").unwrap();
    let disabled_coord = BootstrapCoordinator::new();
    let disabled_outcome = disabled_coord
        .try_begin_bootstrap(&proxy_dll, &simcity_exe, &sys_dir)
        .unwrap();
    assert!(matches!(
        disabled_outcome,
        BootstrapOutcome::DisabledByMarkerFile { .. }
    ));

    // Self-forwarding loop protection rejects proxy_dll == system_dir/d3d9.dll (including verbatim \\?\)
    let self_loop = BootstrapCoordinator::new();
    assert!(matches!(
        self_loop.try_begin_bootstrap(&sys_dir.join("d3d9.dll"), &simcity_exe, &sys_dir),
        Err(D3d9Error::InvalidInstallPath(_))
    ));
    let verbatim_sys_dll = Path::new(&format!(r"\\?\{}", sys_dir.join("d3d9.dll").display())).to_path_buf();
    assert!(matches!(
        BootstrapCoordinator::new().try_begin_bootstrap(&verbatim_sys_dll, &simcity_exe, &sys_dir),
        Err(D3d9Error::InvalidInstallPath(_))
    ));

    // Relative paths and '.' / '..' traversal segments are rejected cleanly.
    assert!(matches!(
        BootstrapCoordinator::new().try_begin_bootstrap(Path::new(r"SimCity\d3d9.dll"), &simcity_exe, &sys_dir),
        Err(D3d9Error::InvalidInstallPath(_))
    ));
    let dotted = sys_dir.join(".").join("d3d9.dll");
    assert!(matches!(
        BootstrapCoordinator::new().try_begin_bootstrap(&dotted, &simcity_exe, &sys_dir),
        Err(D3d9Error::InvalidInstallPath(_))
    ));
    let parent_trav = sys_dir.join("..").join("System32").join("d3d9.dll");
    assert!(matches!(
        BootstrapCoordinator::new().try_begin_bootstrap(&parent_trav, &simcity_exe, &sys_dir),
        Err(D3d9Error::InvalidInstallPath(_))
    ));
}

struct TestNativeAddon;

impl NativeModuleCallbacks for TestNativeAddon {
    fn query_v2(&self, host_version: u32, descriptor: &mut SdModuleDescriptorV2) -> SdResult {
        if host_version != 2 {
            return SdResult::Version;
        }
        descriptor.fill_canonical()
    }

    fn prepare_v2(&self, _owner: SdHandle) -> SdResult {
        SdResult::Ok
    }

    fn admit_v2(&self, _owner: SdHandle) -> SdResult {
        SdResult::Ok
    }

    fn quiesce_v2(&self, _owner: SdHandle) -> SdResult {
        SdResult::Ok
    }

    fn drain_v2(&self, _owner: SdHandle) -> SdResult {
        SdResult::Ok
    }
}

extern "system" fn mock_d3d9_create(sdk_version: u32) -> usize {
    (sdk_version as usize) + 0x1000
}

#[test]
fn integrated_runtime_unites_all_subsystems_and_forwarding_table_dispatches_d3d9_factory() {
    let dir = tempfile::tempdir().unwrap();
    let game_dir = dir.path().join("SimCity");
    let sys_dir = dir.path().join("System32");
    fs::create_dir_all(&game_dir).unwrap();
    fs::create_dir_all(&sys_dir).unwrap();

    let proxy_dll = game_dir.join("d3d9.dll");
    let simcity_exe = game_dir.join("SimCity.exe");
    fs::write(&simcity_exe, build_synthetic_pe32()).unwrap();

    let runtime = IntegratedSimoderRuntime::new();
    let outcome = runtime
        .boot(&proxy_dll, &simcity_exe, &sys_dir, 1000)
        .unwrap();
    assert!(matches!(outcome, BootstrapOutcome::Initialized { .. }));

    // Verify DiagnosticLogPipeline captured the startup event.
    let batch = runtime
        .logs()
        .drain_batch(1010, DeveloperConsoleConfig::default());
    assert_eq!(batch.file_records.len(), 1);
    assert!(
        batch.file_records[0]
            .message
            .contains("Simoder runtime initialized")
    );

    // Verify CrashHandlerCoordinator and ModManager were initialized.
    runtime.with_crash_handler_mut(|ch| assert!(ch.is_some()));
    runtime.with_mod_manager_mut(|mm| assert!(mm.is_some()));

    // Verify decoupled HostToolRegistry works inside the integrated runtime.
    runtime.with_tool_registry_mut(|reg| {
        reg.register_tool(
            ModToolDescriptor {
                owner_mod_id: "dawid.road-helper".into(),
                owner_generation: 1,
                name: "status".into(),
                description: "Read road helper status.".into(),
                input_schema: serde_json::json!({"type": "object"}),
                output_schema: None,
                required_grants: vec![],
                annotations: ModToolAnnotations::read_only(true),
            },
            Arc::new(|_| Ok(serde_json::json!({"ok": true}))),
        )
        .unwrap();
        assert_eq!(reg.list_tools().len(), 1);
    });

    // Verify ABI v2 native module attachment and shutdown/drain.
    let sha = "b".repeat(64);
    let reg = NativeModuleRegistration {
        module_id: "simoder.devtools.mcp".into(),
        expected_sha256: sha.clone(),
        observed_sha256: sha,
        source_size_bytes: 128 * 1024,
        mapped_image_bytes: 256 * 1024,
        native_trust_approved: true,
    };
    let owner = runtime
        .load_and_admit_native_module(&reg, Arc::new(TestNativeAddon))
        .unwrap();
    runtime.with_native_host_mut(|host| {
        assert_eq!(
            host.generation_state(owner),
            Some(NativeModuleScopeState::Active)
        );
    });
    assert_eq!(runtime.shutdown_native_module(owner, true), SdResult::Ok);
    runtime.with_native_host_mut(|host| {
        assert_eq!(host.lifecycle_state(), SdLifecycleState::Stopped);
    });

    // Verify D3d9ForwardingTable dispatches Direct3DCreate9 cleanly.
    let forwarder = D3d9ForwardingTable::new();
    assert_eq!(forwarder.invoke_create9(32), 0);
    forwarder.set_create9_target(Some(mock_d3d9_create));
    assert_eq!(forwarder.invoke_create9(32), 0x1020);
    assert_eq!(forwarder.factory_call_count(), 2);
    assert_eq!(forwarder.last_sdk_version(), 32);
}
