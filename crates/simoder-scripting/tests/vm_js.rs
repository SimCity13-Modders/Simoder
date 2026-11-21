//! QuickJS-NG (`rquickjs`) proof-of-concept and runtime safety qualification tests (issue `#2`).

mod common;

use simoder_scripting::{
    AggregateMemoryPool, FloatObservation, PropCommandAction, PropHostAccess, ScriptError,
    ScriptModuleSource, ScriptVm,
};

use common::{MockPropHost, js_def, mod_id, sample_tgi};

#[test]
fn js_vm_lifecycle_isolation_and_frozen_host_api() {
    let pool = AggregateMemoryPool::new();
    let def_a = js_def(
        "author.js-a",
        r#"
            const helper = require("./helper");
            if (simoder.apiVersion !== 1) throw new Error("bad apiVersion");
            if (simoder.modId !== "author.js-a") throw new Error("bad modId");
            if (!simoder.hasCapability("script.javascript")) throw new Error("missing js cap");
            if (simoder.hasCapability("script.luau")) throw new Error("unexpected luau cap");
            if (!Object.isFrozen(simoder)) throw new Error("simoder must be frozen");
            globalThis.secretLeak = "mod-a-only";
            module.exports = {
                onEnable() {
                    simoder.log("js-enabled:" + helper.tag);
                },
                onTick(dt) {
                    if (typeof dt !== "number") throw new Error("bad dt");
                    simoder.log("js-tick");
                },
                onDisable() {
                    simoder.log("js-disabled");
                }
            };
        "#,
        vec![
            ScriptModuleSource {
                path: "scripts/helper.js".into(),
                code: r#"
                    const nested = require("./sub/leaf");
                    exports.tag = "cjs-" + nested.val;
                "#
                .into(),
            },
            ScriptModuleSource {
                path: "scripts/sub/leaf.js".into(),
                code: r#"module.exports = { val: 42 };"#.into(),
            },
        ],
    );

    let def_b = js_def(
        "author.js-b",
        r#"
            if (typeof globalThis.secretLeak !== "undefined") {
                throw new Error("VM isolation breached");
            }
            if (typeof process !== "undefined" || typeof Bun !== "undefined") {
                throw new Error("unexpected runtime globals");
            }
            let threw = false;
            try {
                simoder.apiVersion = 999;
            } catch (e) {
                threw = true;
            }
            if (!threw) throw new Error("mutating frozen simoder must throw in strict mode");
            module.exports = { onEnable() { simoder.log("isolated-ok"); } };
        "#,
        vec![],
    );

    let mut vm_a = ScriptVm::spawn(mod_id("author.js-a", 1), &def_a, PropHostAccess::Detached, &pool).unwrap();
    let vm_b = ScriptVm::spawn(mod_id("author.js-b", 2), &def_b, PropHostAccess::Detached, &pool).unwrap();
    vm_a.commit().unwrap();
    vm_b.commit().unwrap();
    assert_eq!(vm_a.drain_logs(), vec!["js-enabled:cjs-42"]);
    assert_eq!(vm_b.drain_logs(), vec!["isolated-ok"]);

    vm_a.tick(0.05).unwrap();
    assert_eq!(vm_a.drain_logs(), vec!["js-tick"]);
    assert_eq!(vm_a.stop(), None);
    assert_eq!(vm_a.drain_logs(), vec!["js-disabled"]);

    let cyclic = js_def(
        "author.js-cycle",
        r#"require("./a"); module.exports = {};"#,
        vec![
            ScriptModuleSource {
                path: "scripts/a.js".into(),
                code: r#"require("./b");"#.into(),
            },
            ScriptModuleSource {
                path: "scripts/b.js".into(),
                code: r#"require("./a");"#.into(),
            },
        ],
    );
    let err = match ScriptVm::spawn(mod_id("author.js-cycle", 3), &cyclic, PropHostAccess::Detached, &pool) {
        Ok(_) => panic!("cyclic JS require must fail"),
        Err(err) => err,
    };
    assert!(matches!(err, ScriptError::ModuleError(_)), "got {err:?}");
}

#[test]
fn js_vm_props_observations_rollback_and_on_disable_guard() {
    let pool = AggregateMemoryPool::new();
    let host = MockPropHost::new();
    host.insert(
        sample_tgi(),
        0x09AE_19D7,
        FloatObservation {
            value: 512.0,
            vanilla: 200.0,
            observed_at_unix_ms: 1_700_000_000_000,
            generation: 3,
            instances: 1,
            status: "active".into(),
        },
    );

    let def = js_def(
        "author.js-props",
        r#"
            module.exports = {
                onEnable() {
                    const obs = simoder.props.getFloat("sanitizer", "operatingCost");
                    if (!obs || obs.value !== 512 || !Object.isFrozen(obs)) {
                        throw new Error("unexpected JS PROP observation");
                    }
                    simoder.props.setFloat("sanitizer", "operatingCost", 600);
                },
                onTick() {
                    simoder.props.clearFloat("sanitizer", "operatingCost");
                    throw new Error("abort tick cleanly");
                },
                onDisable() {
                    simoder.props.setFloat("sanitizer", "operatingCost", 111);
                }
            };
        "#,
        vec![],
    );

    let mut vm = ScriptVm::spawn(
        mod_id("author.js-props", 1),
        &def,
        PropHostAccess::Attached(host),
        &pool,
    )
    .unwrap();
    vm.commit().unwrap();
    let cmds = vm.take_patch_commands();
    assert_eq!(cmds.len(), 1);
    assert_eq!(cmds[0].action, PropCommandAction::SetFloat(600.0));

    let tick_err = vm.tick(0.05).unwrap_err();
    assert!(matches!(tick_err, ScriptError::InvocationFailed(_)));
    assert!(vm.take_patch_commands().is_empty(), "JS failed tick must roll back PROP writes");

    let disable_err = vm.stop();
    assert!(
        disable_err
            .as_deref()
            .is_some_and(|msg| msg.contains("onDisable")),
        "expected JS onDisable write rejection, got {disable_err:?}"
    );
}

#[test]
fn js_vm_enforces_memory_cap_stack_limit_and_interrupt_budget() {
    let pool = AggregateMemoryPool::new();

    let mut oom_def = js_def(
        "author.js-oom",
        r#"
            const sink = [];
            for (let i = 0; i < 256; i += 1) {
                sink.push(new Uint8Array(16384));
            }
        "#,
        vec![],
    );
    oom_def.limits.memory_bytes = 2 * 1024 * 1024;
    oom_def.limits.budget_ms = 20;
    let err = match ScriptVm::spawn(mod_id("author.js-oom", 1), &oom_def, PropHostAccess::Detached, &pool) {
        Ok(_) => panic!("JS allocation bomb must trigger MemoryLimitExceeded"),
        Err(err) => err,
    };
    assert!(
        matches!(err, ScriptError::MemoryLimitExceeded(_)),
        "expected MemoryLimitExceeded, got {err:?}"
    );

    let stack_def = js_def(
        "author.js-stack",
        r#"function dive(n) { return dive(n + 1); } dive(0);"#,
        vec![],
    );
    let err = match ScriptVm::spawn(mod_id("author.js-stack", 2), &stack_def, PropHostAccess::Detached, &pool) {
        Ok(_) => panic!("deep recursion must fail cleanly"),
        Err(err) => err,
    };
    assert!(
        matches!(err, ScriptError::InvocationFailed(_)),
        "expected stack overflow InvocationFailed, got {err:?}"
    );

    let mut spin_def = js_def("author.js-spin", r#"while (true) {}"#, vec![]);
    spin_def.limits.budget_ms = 1;
    let err = match ScriptVm::spawn(mod_id("author.js-spin", 3), &spin_def, PropHostAccess::Detached, &pool) {
        Ok(_) => panic!("JS infinite loop must trigger BudgetExceeded"),
        Err(err) => err,
    };
    assert!(
        matches!(err, ScriptError::BudgetExceeded { budget_ms: 1, .. }),
        "expected BudgetExceeded, got {err:?}"
    );
}
