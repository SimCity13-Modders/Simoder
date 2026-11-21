//! Integration tests for `simoder-mods` Native Module Host Boundary (`ABI v2`),
//! decoupled `HostToolRegistry` (zero hard dependency on MCP), and `Simoder-OpenSCP`
//! `build_retail_debug_tools_overlay_package`.

use std::collections::BTreeSet;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use serde_json::json;
use simoder_mods::{
    DEBUG_TOOLS, DEFAULT_CATEGORY, DEFAULT_UI_CATEGORY, HostToolRegistry, ModToolAnnotations,
    ModToolDescriptor, NativeModuleCallbacks, NativeModuleHost, NativeModuleRegistration,
    NativeModuleScopeState, SD_CONTRACT_SHA256_HEX, SD_READY_MENU, SD_READY_RUNTIME,
    SdHandle, SdLifecycleState, SdModuleDescriptorV2, SdResult,
    build_retail_debug_tools_overlay_package,
};

struct MockNativeMcpModule {
    busy_on_drain: AtomicBool,
}

impl MockNativeMcpModule {
    fn new(busy_on_drain: bool) -> Self {
        Self {
            busy_on_drain: AtomicBool::new(busy_on_drain),
        }
    }
}

impl NativeModuleCallbacks for MockNativeMcpModule {
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
        if self.busy_on_drain.load(Ordering::SeqCst) {
            SdResult::Busy
        } else {
            SdResult::Ok
        }
    }
}

#[test]
fn native_module_host_enforces_abi_v2_trust_base_readiness_single_owner_and_image_retention() {
    let mut host = NativeModuleHost::with_limits(2, 4);
    let sha_a = "a".repeat(64);
    let reg = NativeModuleRegistration {
        module_id: "simoder.devtools.mcp".into(),
        expected_sha256: sha_a.clone(),
        observed_sha256: sha_a.clone(),
        source_size_bytes: 256 * 1024,
        mapped_image_bytes: 512 * 1024,
        native_trust_approved: false,
    };

    // Unapproved native trust is rejected before Query/Prepare.
    let module = Arc::new(MockNativeMcpModule::new(true));
    assert_eq!(
        host.prepare_module(&reg, Arc::clone(&module) as Arc<dyn NativeModuleCallbacks>),
        Err(SdResult::Denied)
    );

    let mut trusted_reg = reg;
    trusted_reg.native_trust_approved = true;
    let owner1 = host
        .prepare_module(
            &trusted_reg,
            Arc::clone(&module) as Arc<dyn NativeModuleCallbacks>,
        )
        .expect("Trusted ABI v2 module should stage into Prepared");
    assert_eq!(
        host.generation_state(owner1),
        Some(NativeModuleScopeState::Prepared)
    );

    // Admit before base is ready must return NotReady.
    assert_eq!(host.admit_module(owner1), SdResult::NotReady);

    // Enable base readiness and admit.
    host.set_runtime_readiness(
        true,
        SdLifecycleState::Ready,
        SD_READY_RUNTIME | SD_READY_MENU,
    );
    assert_eq!(host.admit_module(owner1), SdResult::Ok);
    assert_eq!(
        host.generation_state(owner1),
        Some(NativeModuleScopeState::Active)
    );

    // Duplicate active owner for the same module_id is rejected with Conflict.
    assert_eq!(
        host.prepare_module(
            &trusted_reg,
            Arc::clone(&module) as Arc<dyn NativeModuleCallbacks>
        ),
        Err(SdResult::Conflict)
    );

    // Quiesce transitions to Stopping; Drain while BUSY retains Stopping state and never unloads.
    assert_eq!(host.quiesce_module(owner1), SdResult::Ok);
    assert_eq!(
        host.generation_state(owner1),
        Some(NativeModuleScopeState::Stopping)
    );
    assert_eq!(host.drain_module(owner1), SdResult::Busy);
    assert_eq!(
        host.generation_state(owner1),
        Some(NativeModuleScopeState::Stopping)
    );

    // Once in-flight work finishes, Drain transitions to Drained while retaining the image slot.
    module.busy_on_drain.store(false, Ordering::SeqCst);
    assert_eq!(host.drain_module(owner1), SdResult::Ok);
    assert_eq!(
        host.generation_state(owner1),
        Some(NativeModuleScopeState::Drained)
    );
    assert_eq!(host.retained_image_count(), 1);
    assert_eq!(SD_CONTRACT_SHA256_HEX.len(), 64);
}

#[test]
fn decoupled_host_tool_registry_allows_mods_to_register_tools_without_mcp_installed_and_revokes_on_unload() {
    let mut registry = HostToolRegistry::new();

    // Mod registers a custom MCP tool while no MCP DevTool mod is installed -> succeeds cleanly!
    registry
        .register_tool(
            ModToolDescriptor {
                owner_mod_id: "dawid.traffic-analytics".into(),
                owner_generation: 3,
                name: "inspect_congestion".into(),
                description: "Inspect road congestion metrics.".into(),
                input_schema: json!({"type": "object", "properties": {"avenueOnly": {"type": "boolean"}}}),
                output_schema: None,
                required_grants: vec!["mod.traffic.read".into()],
                annotations: ModToolAnnotations::read_only(true),
            },
            Arc::new(|args| {
                let avenue_only = args
                    .get("avenueOnly")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                Ok(json!({"congestionIndex": 0.42, "avenueOnly": avenue_only}))
            }),
        )
        .expect("Mod tool registration must work without MCP installed");

    let listed = registry.list_tools();
    assert_eq!(listed.len(), 1);
    assert_eq!(listed[0].name, "dawid.traffic-analytics.inspect_congestion");

    // Invocation without required grant is denied.
    let empty_grants = BTreeSet::new();
    assert_eq!(
        registry.invoke_tool(
            "dawid.traffic-analytics.inspect_congestion",
            &json!({"avenueOnly": true}),
            &empty_grants
        ),
        Err(SdResult::Denied)
    );

    // Invocation with grant succeeds.
    let mut grants = BTreeSet::new();
    grants.insert("mod.traffic.read".into());
    let res = registry
        .invoke_tool(
            "dawid.traffic-analytics.inspect_congestion",
            &json!({"avenueOnly": true}),
            &grants,
        )
        .unwrap();
    assert_eq!(res["congestionIndex"], json!(0.42));
    assert_eq!(res["avenueOnly"], json!(true));

    // When the mod is disabled or reloaded, its generation is revoked and tools become unreachable.
    assert_eq!(
        registry.revoke_mod_generation("dawid.traffic-analytics", 3),
        1
    );
    assert!(registry.list_tools().is_empty());
    assert_eq!(
        registry.invoke_tool(
            "dawid.traffic-analytics.inspect_congestion",
            &json!({}),
            &grants
        ),
        Err(SdResult::Invalid)
    );
}

#[test]
fn openscp_debug_tools_overlay_builder_rejects_empty_packages_deterministically() {
    let dir = tempfile::tempdir().unwrap();
    let empty_pkg_path = dir.path().join("empty.package");
    let empty_bytes = simoder_formats::write_uncompressed_overlay(&[]).unwrap();
    std::fs::write(&empty_pkg_path, empty_bytes).unwrap();

    let err = build_retail_debug_tools_overlay_package(
        &empty_pkg_path,
        Some(DEFAULT_CATEGORY),
        Some(DEFAULT_UI_CATEGORY),
    )
    .expect_err("Missing debug tool definitions must fail deterministically");
    assert!(err.to_string().contains(&format!("0x{:08X}", DEBUG_TOOLS[0])));
}
