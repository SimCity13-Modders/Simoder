//! Isolated Luau (`mlua`) VM lifecycle, `require`, `simoder.props`, OOM, and timeout integration tests.

mod common;

use simoder_scripting::{
    AggregateMemoryPool, FloatObservation, PropCommandAction, PropHostAccess, ScriptError,
    ScriptModuleSource, ScriptRuntimeState, ScriptVm,
};

use common::{MockPropHost, luau_def, mod_id, sample_tgi};

#[test]
fn luau_vm_lifecycle_require_logs_and_sandbox() {
    let pool = AggregateMemoryPool::new();
    let host = MockPropHost::new();
    let def = luau_def(
        "author.luau-demo",
        r#"
            local helper = require("./helper")
            assert(simoder.apiVersion == 1)
            assert(simoder.modId == "author.luau-demo")
            assert(simoder.hasCapability("script.luau"))
            assert(not simoder.hasCapability("script.javascript"))
            assert(simoder.hasCapability("game.props.float"))
            assert(debug == nil and getfenv == nil and setfenv == nil and loadstring == nil and io == nil)
            local ok_tamper = pcall(function() simoder.apiVersion = 99 end)
            assert(not ok_tamper)
            return {
                onEnable = function()
                    simoder.log("enabled:" .. helper.greet())
                end,
                onTick = function(dt)
                    simoder.log("tick:" .. tostring(dt >= 0))
                end,
                onDisable = function()
                    simoder.log("disabled")
                end,
            }
        "#,
        vec![
            ScriptModuleSource {
                path: "scripts/helper.luau".into(),
                code: r#"
                    local sub = require("./sub/nested")
                    return { greet = function() return "hi-" .. sub.name end }
                "#
                .into(),
            },
            ScriptModuleSource {
                path: "scripts/sub/nested.luau".into(),
                code: r#"return { name = "glassbox" }"#.into(),
            },
        ],
    );

    let mut vm = ScriptVm::spawn(
        mod_id("author.luau-demo", 1),
        &def,
        PropHostAccess::Attached(host),
        &pool,
    )
    .unwrap();
    // Staged VM buffers startup logs until commit().
    assert!(vm.drain_logs().is_empty());
    vm.commit().unwrap();
    assert_eq!(vm.drain_logs(), vec!["enabled:hi-glassbox"]);

    vm.tick(0.05).unwrap();
    assert_eq!(vm.drain_logs(), vec!["tick:true"]);
    assert!(vm.memory_bytes() > 0);

    assert_eq!(vm.stop(), None);
    assert_eq!(vm.drain_logs(), vec!["disabled"]);
    assert!(matches!(
        vm.status().state,
        ScriptRuntimeState::Stopped { diagnostic: None }
    ));
}

#[test]
fn luau_vm_require_rejects_cycles_and_traversal() {
    let pool = AggregateMemoryPool::new();
    let cyclic = luau_def(
        "author.cyclic",
        r#"local a = require("./a"); return {}"#,
        vec![
            ScriptModuleSource {
                path: "scripts/a.luau".into(),
                code: r#"return require("./b")"#.into(),
            },
            ScriptModuleSource {
                path: "scripts/b.luau".into(),
                code: r#"return require("./a")"#.into(),
            },
        ],
    );
    let err = match ScriptVm::spawn(mod_id("author.cyclic", 1), &cyclic, PropHostAccess::Detached, &pool) {
        Ok(_) => panic!("cyclic require must fail"),
        Err(err) => err,
    };
    assert!(matches!(err, ScriptError::ModuleError(_)), "got {err:?}");

    let traversal = luau_def(
        "author.escape",
        r#"local x = require("../../secret"); return {}"#,
        vec![],
    );
    let err = match ScriptVm::spawn(mod_id("author.escape", 2), &traversal, PropHostAccess::Detached, &pool) {
        Ok(_) => panic!("path traversal must fail"),
        Err(err) => err,
    };
    assert!(matches!(err, ScriptError::ModuleError(_)), "got {err:?}");
}

#[test]
fn luau_vm_props_observations_rollback_and_on_disable_guard() {
    let pool = AggregateMemoryPool::new();
    let host = MockPropHost::new();
    host.insert(
        sample_tgi(),
        0x09AE_19D7,
        FloatObservation {
            value: 345.0,
            vanilla: 200.0,
            observed_at_unix_ms: 1_700_000_000_000,
            generation: 7,
            instances: 2,
            status: "active".into(),
        },
    );

    let def = luau_def(
        "author.luau-props",
        r#"
            return {
                onEnable = function()
                    local obs = simoder.props.getFloat("sanitizer", "operatingCost")
                    assert(obs.value == 345 and obs.vanilla == 200 and obs.historical == true)
                    simoder.props.setFloat("sanitizer", "operatingCost", 450)
                end,
                onTick = function(_)
                    simoder.props.setFloat("00B1B104:61EFC000:00000000719436BD", "0x09AE19D7", 500)
                    error("rollback tick batch")
                end,
                onDisable = function()
                    simoder.props.clearFloat("sanitizer", "operatingCost")
                end,
            }
        "#,
        vec![],
    );

    let mut vm = ScriptVm::spawn(
        mod_id("author.luau-props", 1),
        &def,
        PropHostAccess::Attached(host),
        &pool,
    )
    .unwrap();
    vm.commit().unwrap();
    let enable_cmds = vm.take_patch_commands();
    assert_eq!(enable_cmds.len(), 1);
    assert_eq!(enable_cmds[0].action, PropCommandAction::SetFloat(450.0));

    let tick_err = vm.tick(0.05).unwrap_err();
    assert!(matches!(tick_err, ScriptError::InvocationFailed(_)));
    assert!(vm.take_patch_commands().is_empty(), "failed tick must roll back commands");

    let disable_err = vm.stop();
    assert!(
        disable_err
            .as_deref()
            .is_some_and(|msg| msg.contains("onDisable")),
        "expected onDisable write rejection, got {disable_err:?}"
    );
}

#[test]
fn luau_vm_enforces_memory_cap_and_timeout_budget() {
    let pool = AggregateMemoryPool::new();
    let mut mem_def = luau_def(
        "author.luau-oom",
        r#"
            local t = {}
            local chunk = string.rep("x", 16384)
            for i = 1, 256 do
                t[i] = chunk .. tostring(i)
            end
            return {}
        "#,
        vec![],
    );
    mem_def.limits.memory_bytes = 2 * 1024 * 1024;
    mem_def.limits.budget_ms = 20;
    let err = match ScriptVm::spawn(mod_id("author.luau-oom", 1), &mem_def, PropHostAccess::Detached, &pool) {
        Ok(_) => panic!("2 MiB allocation bomb must hit OOM"),
        Err(err) => err,
    };
    assert!(
        matches!(err, ScriptError::MemoryLimitExceeded(_)),
        "expected MemoryLimitExceeded, got {err:?}"
    );

    let mut spin_def = luau_def(
        "author.luau-spin",
        r#"while true do end; return {}"#,
        vec![],
    );
    spin_def.limits.budget_ms = 1;
    let err = match ScriptVm::spawn(mod_id("author.luau-spin", 2), &spin_def, PropHostAccess::Detached, &pool) {
        Ok(_) => panic!("infinite loop must exceed 1 ms budget"),
        Err(err) => err,
    };
    assert!(
        matches!(err, ScriptError::BudgetExceeded { budget_ms: 1, .. }),
        "expected BudgetExceeded, got {err:?}"
    );
}
