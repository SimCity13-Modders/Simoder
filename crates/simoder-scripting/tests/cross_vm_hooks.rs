//! Synchronous cross-VM hook chains (`HookChain`) and real multi-VM `CrossVmHookHub` tests.

mod common;

use simoder_scripting::{
    CrossVmCallArg, CrossVmHookHub, HookApproval, HookChain, HookPermissionScope, HookTargetId,
    HookValue, ScriptError, ScriptLimits,
};

use common::vm_gen;

#[test]
fn hook_chain_enforces_pre_last_override_and_post_without_fallback() {
    let mut chain = HookChain::new(Box::new(|x: &mut i32| {
        *x += 100;
        Ok(*x)
    }));
    chain.add_pre_hook("mod-a", Box::new(|x: &mut i32| { *x += 1; Ok(()) }));
    chain.add_override("mod-b", Box::new(|x: &mut i32| Ok(*x * 2)));
    chain.add_override("mod-c", Box::new(|x: &mut i32| Ok(*x * 10)));
    chain.add_post_hook("mod-d", Box::new(|_, ret: i32| Ok(ret + 5)));

    assert_eq!(chain.active_override_owner(), Some("mod-c"));
    let mut arg = 2;
    assert_eq!(chain.invoke(&mut arg).unwrap(), 35);

    let mut failing = HookChain::new(Box::new(|_: &mut i32| Ok(999)));
    failing.add_override(
        "bad-mod",
        Box::new(|_| Err(ScriptError::InvocationFailed("boom".into()))),
    );
    failing.add_post_hook("post", Box::new(|_, _| panic!("post hook must not run")));
    assert!(failing.invoke(&mut 0).is_err());
}

#[test]
fn cross_vm_synchronous_prehook_mutates_real_target_object_with_alias_identity() {
    let hub = CrossVmHookHub::new();
    hub.set_mod_load_order(vec!["mod-target".into(), "mod-hook".into()])
        .unwrap();

    let target_id = hub
        .spawn_vm(
            "mod-target",
            vm_gen(1),
            ScriptLimits::default(),
            r#"
                local privateSecret = function() return 999 end
                local settings = {
                    tax = 10,
                    nested = { multiplier = 2 }
                }
                return {
                    settings = settings,
                    applySettings = function(a, b)
                        if a ~= b then error("alias identity lost in owner") end
                        return a.tax + b.nested.multiplier
                    end
                }
            "#,
        )
        .unwrap();

    let caller_id = hub
        .spawn_vm(
            "mod-hook",
            vm_gen(2),
            ScriptLimits::default(),
            r#"return { noop = function() return 0 end }"#,
        )
        .unwrap();

    let hook_target = HookTargetId::new("mod-target", "applySettings").unwrap();
    hub.grant_approval(HookApproval {
        caller_mod_id: "mod-hook".into(),
        target: hook_target.clone(),
        scope: HookPermissionScope::MutateArgs,
    })
    .unwrap();

    hub.register_pre_hook(
        &caller_id,
        &hook_target,
        r#"
            return function(a, b)
                assert(a == b, "two arguments to same object must preserve proxy alias identity")
                a.tax = 25
                b.nested.multiplier = 5
            end
        "#,
    )
    .unwrap();

    let private_target = HookTargetId::new("mod-target", "privateSecret").unwrap();
    assert!(
        hub.invoke_target(&target_id, &private_target, &[], 5)
            .is_err()
    );

    let ret = hub
        .invoke_target(
            &target_id,
            &hook_target,
            &[
                CrossVmCallArg::TargetExportedTable("settings".into()),
                CrossVmCallArg::TargetExportedTable("settings".into()),
            ],
            10,
        )
        .unwrap();
    assert_eq!(ret.values(), &[HookValue::int(30)]);

    assert_eq!(
        hub.read_target_table_field("mod-target", "settings", "tax")
            .unwrap(),
        HookValue::int(25)
    );
}

#[test]
fn cross_vm_proxy_handles_cycles_rejects_frozen_and_unsupported_values() {
    let hub = CrossVmHookHub::new();
    let target_id = hub
        .spawn_vm(
            "mod-cycle",
            vm_gen(1),
            ScriptLimits::default(),
            r#"
                local cfg = { value = 7 }
                cfg.self = cfg
                local frozen = { locked = 1 }
                table.freeze(frozen)
                return {
                    cfg = cfg,
                    frozen = frozen,
                    touchCycle = function(c) return c.self.value end,
                    touchFrozen = function(f) return f.locked end,
                }
            "#,
        )
        .unwrap();
    let caller_id = hub
        .spawn_vm("mod-caller", vm_gen(2), ScriptLimits::default(), "return {}")
        .unwrap();

    let cycle_target = HookTargetId::new("mod-cycle", "touchCycle").unwrap();
    let frozen_target = HookTargetId::new("mod-cycle", "touchFrozen").unwrap();
    for t in [&cycle_target, &frozen_target] {
        hub.grant_approval(HookApproval {
            caller_mod_id: "mod-caller".into(),
            target: t.clone(),
            scope: HookPermissionScope::MutateArgs,
        })
        .unwrap();
    }

    hub.register_pre_hook(
        &caller_id,
        &cycle_target,
        r#"
            return function(c)
                assert(c == c.self)
                c.self.value = 19
            end
        "#,
    )
    .unwrap();
    let ret = hub
        .invoke_target(
            &target_id,
            &cycle_target,
            &[CrossVmCallArg::TargetExportedTable("cfg".into())],
            10,
        )
        .unwrap();
    assert_eq!(ret.values(), &[HookValue::int(19)]);

    hub.register_pre_hook(
        &caller_id,
        &cycle_target,
        r#"return function(c) c.fn = function() end end"#,
    )
    .unwrap();
    let closure_err = hub
        .invoke_target(
            &target_id,
            &cycle_target,
            &[CrossVmCallArg::TargetExportedTable("cfg".into())],
            10,
        )
        .unwrap_err();
    assert!(
        closure_err
            .to_string()
            .contains("closures cannot cross VM boundaries"),
        "expected unsupported closure error, got {closure_err}"
    );

    hub.register_pre_hook(
        &caller_id,
        &frozen_target,
        r#"return function(f) f.locked = 2 end"#,
    )
    .unwrap();
    let frozen_err = hub
        .invoke_target(
            &target_id,
            &frozen_target,
            &[CrossVmCallArg::TargetExportedTable("frozen".into())],
            10,
        )
        .unwrap_err();
    assert!(
        frozen_err.to_string().contains("frozen proxy"),
        "expected frozen rejection, got {frozen_err}"
    );
}

#[test]
fn cross_vm_enforces_approvals_live_revocation_load_order_and_failure_semantics() {
    let hub = CrossVmHookHub::new();
    hub.set_mod_load_order(vec![
        "mod-base".into(),
        "mod-first".into(),
        "mod-second".into(),
    ])
    .unwrap();

    let base_id = hub
        .spawn_vm(
            "mod-base",
            vm_gen(1),
            ScriptLimits::default(),
            r#"return { compute = function(x) return x + 1 end }"#,
        )
        .unwrap();
    let first_id = hub
        .spawn_vm("mod-first", vm_gen(2), ScriptLimits::default(), "return {}")
        .unwrap();
    let second_id = hub
        .spawn_vm("mod-second", vm_gen(3), ScriptLimits::default(), "return {}")
        .unwrap();

    let target = HookTargetId::new("mod-base", "compute").unwrap();
    assert!(matches!(
        HookTargetId::new("simoder.base", "internal"),
        Err(ScriptError::PermissionDenied(_))
    ));
    let unapproved = hub.register_override(&first_id, &target, "return function(x) return x end");
    assert!(matches!(unapproved, Err(ScriptError::PermissionDenied(_))));

    hub.grant_approval(HookApproval {
        caller_mod_id: "mod-first".into(),
        target: target.clone(),
        scope: HookPermissionScope::OverrideBody,
    })
    .unwrap();
    hub.grant_approval(HookApproval {
        caller_mod_id: "mod-second".into(),
        target: target.clone(),
        scope: HookPermissionScope::OverrideBody,
    })
    .unwrap();

    hub.register_override(&second_id, &target, "return function(x) return x * 100 end")
        .unwrap();
    hub.register_override(&first_id, &target, "return function(x) return x * 10 end")
        .unwrap();

    let conflict = hub.inspect_override_conflicts(&target).unwrap();
    assert_eq!(conflict.inactive_contenders, vec![first_id.clone()]);
    assert_eq!(
        hub.invoke_target(&base_id, &target, &[CrossVmCallArg::Value(HookValue::int(3))], 10)
            .unwrap()
            .values(),
        &[HookValue::int(300)]
    );

    hub.revoke_approval("mod-second", &target).unwrap();
    let revoked_err = hub
        .invoke_target(&base_id, &target, &[CrossVmCallArg::Value(HookValue::int(3))], 10)
        .unwrap_err();
    assert!(matches!(revoked_err, ScriptError::PermissionDenied(_)));

    hub.grant_approval(HookApproval {
        caller_mod_id: "mod-second".into(),
        target: target.clone(),
        scope: HookPermissionScope::OverrideBody,
    })
    .unwrap();
    hub.register_override(&second_id, &target, r#"return function(_) error("override crashed") end"#)
        .unwrap();
    hub.register_post_hook(
        &first_id,
        &target,
        r#"return function(_) error("posthook must NOT run after override failure") end"#,
    )
    .unwrap();
    let fail_err = hub
        .invoke_target(&base_id, &target, &[CrossVmCallArg::Value(HookValue::int(3))], 10)
        .unwrap_err();
    assert!(fail_err.to_string().contains("override crashed"), "got {fail_err}");
}

#[test]
fn cross_vm_get_return_multi_nil_and_get_function_stale_handles() {
    let hub = CrossVmHookHub::new();
    hub.set_mod_load_order(vec!["mod-src".into(), "mod-a".into(), "mod-b".into()])
        .unwrap();
    let src_id = hub
        .spawn_vm(
            "mod-src",
            vm_gen(1),
            ScriptLimits::default(),
            r#"return { multi = function(x) return x, nil, "end" end }"#,
        )
        .unwrap();
    let a_id = hub
        .spawn_vm("mod-a", vm_gen(2), ScriptLimits::default(), "return {}")
        .unwrap();
    let b_id = hub
        .spawn_vm("mod-b", vm_gen(3), ScriptLimits::default(), "return {}")
        .unwrap();

    let target = HookTargetId::new("mod-src", "multi").unwrap();
    hub.grant_approval(HookApproval {
        caller_mod_id: "mod-a".into(),
        target: target.clone(),
        scope: HookPermissionScope::ObserveOnly,
    })
    .unwrap();
    hub.grant_approval(HookApproval {
        caller_mod_id: "mod-b".into(),
        target: target.clone(),
        scope: HookPermissionScope::ObserveOnly,
    })
    .unwrap();

    // `GetFunction` and PostHook return-value replacement are denied under `ObserveOnly`.
    assert!(matches!(
        hub.get_function(&a_id, &target),
        Err(ScriptError::PermissionDenied(_))
    ));

    hub.register_post_hook(
        &a_id,
        &target,
        r#"
            return function()
                local a, b, c = simoder.hooks.getReturn()
                assert(a == 5 and b == nil and c == "end")
                return nil, "patched", nil
            end
        "#,
    )
    .unwrap();
    let observe_replace_err = hub
        .invoke_target(&src_id, &target, &[CrossVmCallArg::Value(HookValue::int(5))], 10)
        .unwrap_err();
    assert!(matches!(observe_replace_err, ScriptError::PermissionDenied(_)));

    hub.grant_approval(HookApproval {
        caller_mod_id: "mod-a".into(),
        target: target.clone(),
        scope: HookPermissionScope::MutateArgs,
    })
    .unwrap();
    hub.register_post_hook(
        &b_id,
        &target,
        r#"
            return function()
                local a, b, c = simoder.hooks.getReturn()
                assert(a == nil and b == "patched" and c == nil)
                return
            end
        "#,
    )
    .unwrap();

    let ret = hub
        .invoke_target(&src_id, &target, &[CrossVmCallArg::Value(HookValue::int(5))], 10)
        .unwrap();
    assert_eq!(ret.len(), 3);
    assert_eq!(
        ret.values(),
        &[HookValue::nil(), HookValue::text("patched"), HookValue::nil()]
    );

    assert!(matches!(
        hub.get_function(&a_id, &target),
        Err(ScriptError::PermissionDenied(_))
    ));
    hub.grant_approval(HookApproval {
        caller_mod_id: "mod-a".into(),
        target: target.clone(),
        scope: HookPermissionScope::OverrideBody,
    })
    .unwrap();
    let fn_handle = hub.get_function(&a_id, &target).unwrap();
    let direct = hub
        .invoke_get_function(&fn_handle, vec![HookValue::int(9)], 10)
        .unwrap();
    assert_eq!(
        direct.values(),
        &[HookValue::int(9), HookValue::nil(), HookValue::text("end")]
    );

    let _src_reloaded = hub
        .spawn_vm(
            "mod-src",
            vm_gen(4),
            ScriptLimits::default(),
            r#"return { multi = function(x) return x + 100 end }"#,
        )
        .unwrap();
    let stale_err = hub
        .invoke_get_function(&fn_handle, vec![HookValue::int(9)], 10)
        .unwrap_err();
    assert!(matches!(stale_err, ScriptError::StaleHandle(_)));
}

#[test]
fn cross_vm_enforces_reentry_cap_and_provenance_without_laundering() {
    let hub = CrossVmHookHub::new();
    let a_id = hub
        .spawn_vm(
            "mod-a",
            vm_gen(1),
            ScriptLimits::default(),
            r#"
                return {
                    ping = function(depth)
                        local orig, active = simoder.hooks.provenance()
                        assert(orig == "mod-a" and active == "mod-a")
                        return simoder.hooks.invoke("mod-b", "pong", depth + 1)
                    end
                }
            "#,
        )
        .unwrap();
    let _b_id = hub
        .spawn_vm(
            "mod-b",
            vm_gen(2),
            ScriptLimits::default(),
            r#"
                return {
                    pong = function(depth)
                        local orig, active = simoder.hooks.provenance()
                        assert(orig == "mod-a" and active == "mod-b", "must track initiator vs active owner")
                        return simoder.hooks.invoke("mod-a", "ping", depth + 1)
                    end
                }
            "#,
        )
        .unwrap();

    let ping_target = HookTargetId::new("mod-a", "ping").unwrap();
    let err = hub
        .invoke_target(&a_id, &ping_target, &[CrossVmCallArg::Value(HookValue::int(0))], 10)
        .unwrap_err();
    assert!(
        err.to_string().contains("re-entry depth exceeded max 8"),
        "expected ReentryLimitExceeded, got {err}"
    );
}

#[test]
fn cross_vm_syncs_override_and_posthook_proxy_writes_rejects_raw_tables_and_preserves_outer_budget() {
    let hub = CrossVmHookHub::new();
    let a_id = hub
        .spawn_vm(
            "mod-a",
            vm_gen(1),
            ScriptLimits::default(),
            r#"
                local tbl = { count = 1 }
                return {
                    state = tbl,
                    outer = function()
                        local mid = simoder.hooks.invoke("mod-b", "bounce")
                        local spins = 0
                        while spins < 100000000 do
                            spins = spins + 1
                        end
                        return mid
                    end,
                    inner = function()
                        return 7
                    end,
                    spin_inner = function()
                        return 7
                    end
                }
            "#,
        )
        .unwrap();
    let b_id = hub
        .spawn_vm(
            "mod-b",
            vm_gen(2),
            ScriptLimits::default(),
            r#"
                return {
                    bounce = function()
                        return simoder.hooks.invoke("mod-a", "spin_inner")
                    end
                }
            "#,
        )
        .unwrap();

    let inner_target = HookTargetId::new("mod-a", "inner").unwrap();
    hub.grant_approval(HookApproval {
        caller_mod_id: "mod-b".into(),
        target: inner_target.clone(),
        scope: HookPermissionScope::OverrideBody,
    })
    .unwrap();
    hub.register_override(
        &b_id,
        &inner_target,
        "return function(obj) obj.count = obj.count + 10 return obj.count end",
    )
    .unwrap();
    hub.register_post_hook(
        &b_id,
        &inner_target,
        "return function(obj) obj.count = obj.count + 5 end",
    )
    .unwrap();

    let updated = hub
        .invoke_target(
            &a_id,
            &inner_target,
            &[CrossVmCallArg::TargetExportedTable("state".into())],
            20,
        )
        .unwrap();
    assert_eq!(updated.values(), &[HookValue::int(11)]);
    assert_eq!(
        hub.read_target_table_field("mod-a", "state", "count").unwrap(),
        HookValue::int(16)
    );

    // Reject raw foreign table assignment into proxy fields cleanly.
    hub.register_pre_hook(
        &b_id,
        &inner_target,
        "return function(obj) obj.nested = { foreign = true } end",
    )
    .unwrap();
    let raw_tbl_err = hub
        .invoke_target(
            &a_id,
            &inner_target,
            &[CrossVmCallArg::TargetExportedTable("state".into())],
            20,
        )
        .unwrap_err();
    assert!(
        matches!(raw_tbl_err, ScriptError::UnsupportedValue(_)),
        "expected UnsupportedValue, got {raw_tbl_err}"
    );

    // After re-entry A -> B -> A returns, outer A's budget tracker remains armed and aborts spin.
    let outer_target = HookTargetId::new("mod-a", "outer").unwrap();
    let spin_err = hub
        .invoke_target(&a_id, &outer_target, &[], 5)
        .unwrap_err();
    assert!(
        matches!(spin_err, ScriptError::BudgetExceeded { .. }),
        "expected outer BudgetExceeded after nested re-entry, got {spin_err}"
    );
}
