//! Scheduler lifecycle, PROP patch reducer, EventBus, DelayedCalls, bridge state/cache/UI,
//! and `0x0A98EAF0` localization integration tests.

mod common;

use std::collections::{BTreeMap, HashMap};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde_json::json;
use simoder_core::fnv1_name_hash;
use simoder_formats::LocaleItem;
use simoder_scripting::config::{
    MAX_AGGREGATE_SCRIPT_MEMORY_BYTES, MAX_DELAYED_CALLS_PER_TURN, MAX_PROP_OVERRIDES_PER_SCRIPT,
    VANILLA_LOCALE_TABLE_IDS,
};
use simoder_scripting::{
    AuthoritativeCityState, CityContextState, CityLoadedVerdict, CityStateUnavailableReason,
    CommitOutcome, DelayClock, DelayedCallPolicyOnNoCity, DelayedCallQueue,
    DelayedCallScheduleRequest, EcoGameAddOnKey, EcoGameScriptCacheKey, EventBus, EventSchema,
    FaultPolicy, HookValue, LocaleFallbackChain, ModLocalizationCatalog, ModLocalizationConfig,
    PropCommandAction, PropHostAccess, ResolvedText, ScriptError, ScriptOverrideEntry,
    ScriptPatchCommand, ScriptScheduler, SimulationClockSample, StagedLocaleFile, UiClientEvent,
    UiCommand, VmGeneration, decode_mod_locale_resource, export_mod_locale_resource,
    reduce_script_patches,
};

use common::{MockPropHost, RecordingTickSink, js_def, luau_def, mod_id, sample_tgi};

#[test]
fn patch_reducer_merges_clears_and_enforces_256_override_cap() {
    let target = sample_tgi();
    let initial = vec![ScriptOverrideEntry {
        target,
        property_id: 1,
        value: 10.0,
        source: "Prior".into(),
        symbol_origin: None,
    }];
    let commands = vec![
        ScriptPatchCommand {
            target,
            property_id: 1,
            action: PropCommandAction::ClearFloat,
            symbol_origin: None,
            default_source: "Luau host API",
        },
        ScriptPatchCommand {
            target,
            property_id: 2,
            action: PropCommandAction::SetFloat(42.5),
            symbol_origin: None,
            default_source: "JS host API",
        },
    ];
    let reduced = reduce_script_patches(&initial, &commands).unwrap();
    assert_eq!(reduced.len(), 1);
    assert_eq!(reduced[0].property_id, 2);
    assert_eq!(reduced[0].value, 42.5);
    assert_eq!(reduced[0].source, "JS host API");

    let overflow_cmds: Vec<ScriptPatchCommand> = (0..=MAX_PROP_OVERRIDES_PER_SCRIPT)
        .map(|i| ScriptPatchCommand {
            target,
            property_id: i as u32 + 10,
            action: PropCommandAction::SetFloat(1.0),
            symbol_origin: None,
            default_source: "Luau host API",
        })
        .collect();
    let err = reduce_script_patches(&[], &overflow_cmds).unwrap_err();
    assert!(matches!(
        err,
        ScriptError::PatchLimitExceeded {
            limit: MAX_PROP_OVERRIDES_PER_SCRIPT,
            ..
        }
    ));
}

#[test]
fn scheduler_runs_mixed_luau_and_js_vms_with_fault_isolation_and_128_mib_pool() {
    let scheduler = ScriptScheduler::new(FaultPolicy::Regular);
    let host = MockPropHost::new();

    let luau_ok = luau_def(
        "author.mixed-luau",
        r#"
            return {
                onEnable = function()
                    simoder.log("luau-start")
                end,
                onTick = function(_)
                    simoder.props.setFloat("sanitizer", "operatingCost", 250)
                end,
                onDisable = function()
                    simoder.log("luau-bye")
                end,
            }
        "#,
        vec![],
    );
    let js_bad = js_def(
        "author.mixed-js",
        r#"
            let count = 0;
            module.exports = {
                onEnable() { simoder.log("js-start"); },
                onTick() {
                    count += 1;
                    if (count >= 1) throw new Error("js-tick-fault");
                }
            };
        "#,
        vec![],
    );

    let luau_id = scheduler
        .stage_script(&luau_ok, PropHostAccess::Attached(host.clone()))
        .unwrap();
    let js_id = scheduler
        .stage_script(&js_bad, PropHostAccess::Attached(host.clone()))
        .unwrap();

    let staged_discard = luau_def(
        "author.discarded",
        r#"return { onDisable = function() error("must never run onDisable") end }"#,
        vec![],
    );
    let discard_id = scheduler
        .stage_script(&staged_discard, PropHostAccess::Detached)
        .unwrap();
    scheduler.discard_staged(&discard_id).unwrap();

    let luau_commit: CommitOutcome = scheduler.commit_staged(&luau_id).unwrap();
    assert_eq!(luau_commit.startup_logs, vec!["luau-start"]);
    let js_commit = scheduler.commit_staged(&js_id).unwrap();
    assert_eq!(js_commit.startup_logs, vec!["js-start"]);

    let mut sink = RecordingTickSink::new();
    let report = scheduler.tick_cycle(Instant::now(), SimulationClockSample::NoCity, &mut sink);
    assert_eq!(report.ticked_mods.len(), 2);
    assert_eq!(report.faulted_mods.len(), 1);
    assert_eq!(report.faulted_mods[0].0, js_id);
    assert_eq!(sink.patches.len(), 1);
    assert_eq!(sink.patches[0].0, luau_id);

    let report2 = scheduler.tick_cycle(Instant::now(), SimulationClockSample::NoCity, &mut sink);
    assert_eq!(report2.ticked_mods, vec![luau_id.clone()]);

    let mut big_def = luau_def("author.big-a", "return {}", vec![]);
    big_def.limits.memory_bytes = 64 * 1024 * 1024;
    let big_id = scheduler.stage_script(&big_def, PropHostAccess::Detached).unwrap();
    let mut too_big = js_def("author.big-b", "module.exports = {}", vec![]);
    too_big.limits.memory_bytes = 64 * 1024 * 1024;
    let pool_err = scheduler.stage_script(&too_big, PropHostAccess::Detached).unwrap_err();
    assert!(
        matches!(pool_err, ScriptError::MemoryLimitExceeded(_)),
        "expected aggregate >128 MiB rejection, got {pool_err:?}"
    );
    assert!(scheduler.memory_pool().reserved_bytes() <= MAX_AGGREGATE_SCRIPT_MEMORY_BYTES);
    scheduler.discard_staged(&big_id).unwrap();

    let stopped = scheduler.shutdown();
    assert_eq!(stopped.len(), 2);
    assert_eq!(scheduler.memory_pool().reserved_bytes(), 0);
}

#[test]
fn events_bus_enforces_namespaces_impersonation_ordering_fault_policy_and_retirement() {
    let mut bus = EventBus::new();
    let emitter = mod_id("author.weather", 1);
    let impersonator = mod_id("author.rogue", 2);
    let sub_a = mod_id("author.sub-a", 3);
    let sub_b = mod_id("author.sub-b", 4);

    let key = bus
        .register_event(
            emitter.clone(),
            "stormStarted",
            EventSchema::new(1, "City storm notification", true).unwrap(),
        )
        .unwrap();
    assert_eq!(key.canonical_name(), "author.weather:stormStarted");

    let err = bus
        .emit(
            &impersonator,
            &key,
            &HookValue::int(1),
            &[],
            FaultPolicy::Regular,
        )
        .unwrap_err();
    assert!(matches!(err, ScriptError::PermissionDenied(_)));

    let received = Arc::new(Mutex::new(Vec::new()));
    let r1 = Arc::clone(&received);
    let _sid_b = bus
        .subscribe(
            sub_b.clone(),
            &key,
            Arc::new(move |_| {
                r1.lock().unwrap().push("sub-b");
                Ok(Some(HookValue::text("ack-b")))
            }),
        )
        .unwrap();
    let r2 = Arc::clone(&received);
    let _sid_a = bus
        .subscribe(
            sub_a.clone(),
            &key,
            Arc::new(move |_| {
                r2.lock().unwrap().push("sub-a");
                Err(ScriptError::InvocationFailed("listener glitch".into()))
            }),
        )
        .unwrap();

    let load_order = vec!["author.sub-a".to_string(), "author.sub-b".to_string()];
    let outcome = bus
        .emit(
            &emitter,
            &key,
            &HookValue::text("typhoon"),
            &load_order,
            FaultPolicy::Regular,
        )
        .unwrap();
    assert_eq!(*received.lock().unwrap(), vec!["sub-a", "sub-b"]);
    assert_eq!(outcome.diagnostics.len(), 1);
    assert_eq!(
        outcome.responses,
        vec![(sub_b.clone(), HookValue::text("ack-b"))]
    );

    let dev_err = bus
        .emit(
            &emitter,
            &key,
            &HookValue::text("typhoon"),
            &load_order,
            FaultPolicy::Dev,
        )
        .unwrap_err();
    assert!(matches!(dev_err, ScriptError::InvocationFailed(_)));

    bus.retire_generation(&sub_a);
    let outcome2 = bus
        .emit(&emitter, &key, &HookValue::nil(), &load_order, FaultPolicy::Dev)
        .unwrap();
    assert_eq!(outcome2.delivered, 1);

    bus.retire_generation(&emitter);
    assert!(bus.schema(&key).is_none());

    // Void event return is isolated in `FaultPolicy::Regular` and continues delivering to later listeners.
    let void_key = bus
        .register_event(
            emitter.clone(),
            "citySaved",
            EventSchema::new(1, "City saved notification", false).unwrap(),
        )
        .unwrap();
    let second_called = Arc::new(Mutex::new(false));
    let second_flag = Arc::clone(&second_called);
    bus.subscribe(
        sub_a.clone(),
        &void_key,
        Arc::new(|_| Ok(Some(HookValue::int(99)))),
    )
    .unwrap();
    bus.subscribe(
        sub_b.clone(),
        &void_key,
        Arc::new(move |_| {
            *second_flag.lock().unwrap() = true;
            Ok(None)
        }),
    )
    .unwrap();
    let void_regular = bus
        .emit(&emitter, &void_key, &HookValue::nil(), &load_order, FaultPolicy::Regular)
        .unwrap();
    assert_eq!(void_regular.delivered, 2);
    assert_eq!(void_regular.diagnostics.len(), 1);
    assert!(*second_called.lock().unwrap());
    let void_dev = bus
        .emit(&emitter, &void_key, &HookValue::nil(), &load_order, FaultPolicy::Dev)
        .unwrap_err();
    assert!(matches!(void_dev, ScriptError::EventError(_)));
}

#[test]
fn delayed_calls_support_real_and_simulation_pause_fairness_and_retirement() {
    let mut queue = DelayedCallQueue::new();
    let mod_a = mod_id("mod-a", 1);
    let mod_b = mod_id("mod-b", 2);
    let active: BTreeMap<String, VmGeneration> = [
        ("mod-a".to_string(), mod_a.generation),
        ("mod-b".to_string(), mod_b.generation),
    ]
    .into_iter()
    .collect();

    let t0 = Instant::now();
    let bad = queue.schedule(
        t0,
        SimulationClockSample::Running { sim_elapsed_ms: 0 },
        DelayedCallScheduleRequest {
            owner: mod_a.clone(),
            call_id: "bad".into(),
            delay_seconds: f64::NAN,
            clock: DelayClock::Real,
            on_no_city: DelayedCallPolicyOnNoCity::Pause,
            payload: HookValue::nil(),
        },
    );
    assert!(matches!(bad, Err(ScriptError::DelayedCallError(_))));

    queue
        .schedule(
            t0,
            SimulationClockSample::Running { sim_elapsed_ms: 100 },
            DelayedCallScheduleRequest {
                owner: mod_a.clone(),
                call_id: "sim-call".into(),
                delay_seconds: 0.5,
                clock: DelayClock::Simulation,
                on_no_city: DelayedCallPolicyOnNoCity::Pause,
                payload: HookValue::int(1),
            },
        )
        .unwrap();
    queue
        .schedule(
            t0,
            SimulationClockSample::Running { sim_elapsed_ms: 100 },
            DelayedCallScheduleRequest {
                owner: mod_a.clone(),
                call_id: "sim-call".into(),
                delay_seconds: 0.2,
                clock: DelayClock::Simulation,
                on_no_city: DelayedCallPolicyOnNoCity::Pause,
                payload: HookValue::int(2),
            },
        )
        .unwrap();

    assert!(
        queue
            .poll_due(
                t0 + Duration::from_secs(10),
                SimulationClockSample::Paused { sim_elapsed_ms: 100 },
                &active
            )
            .is_empty()
    );
    let due = queue.poll_due(
        t0 + Duration::from_secs(10),
        SimulationClockSample::Running { sim_elapsed_ms: 300 },
        &active,
    );
    assert_eq!(due.len(), 1);
    assert_eq!(due[0].payload, HookValue::int(2));

    for i in 0..40 {
        queue
            .schedule(
                t0,
                SimulationClockSample::NoCity,
                DelayedCallScheduleRequest {
                    owner: if i % 2 == 0 { mod_a.clone() } else { mod_b.clone() },
                    call_id: format!("burst-{i}"),
                    delay_seconds: 0.01,
                    clock: DelayClock::Real,
                    on_no_city: DelayedCallPolicyOnNoCity::Pause,
                    payload: HookValue::nil(),
                },
            )
            .unwrap();
    }
    let turn1 = queue.poll_due(
        t0 + Duration::from_secs(1),
        SimulationClockSample::NoCity,
        &active,
    );
    assert_eq!(turn1.len(), MAX_DELAYED_CALLS_PER_TURN);
    let turn2 = queue.poll_due(
        t0 + Duration::from_secs(2),
        SimulationClockSample::NoCity,
        &active,
    );
    assert_eq!(turn2.len(), 40 - MAX_DELAYED_CALLS_PER_TURN);

    queue
        .schedule(
            t0,
            SimulationClockSample::Running { sim_elapsed_ms: 300 },
            DelayedCallScheduleRequest {
                owner: mod_b.clone(),
                call_id: "city-only".into(),
                delay_seconds: 1.0,
                clock: DelayClock::Simulation,
                on_no_city: DelayedCallPolicyOnNoCity::CancelOnCityUnload,
                payload: HookValue::nil(),
            },
        )
        .unwrap();
    let _ = queue.poll_due(t0, SimulationClockSample::NoCity, &active);
    assert!(queue.is_empty());

    // Scheduling at a later simulation timestamp advances earlier simulation timers first.
    queue
        .schedule(
            t0,
            SimulationClockSample::Running { sim_elapsed_ms: 100 },
            DelayedCallScheduleRequest {
                owner: mod_a.clone(),
                call_id: "early".into(),
                delay_seconds: 0.5,
                clock: DelayClock::Simulation,
                on_no_city: DelayedCallPolicyOnNoCity::Pause,
                payload: HookValue::int(1),
            },
        )
        .unwrap();
    queue
        .schedule(
            t0,
            SimulationClockSample::Running { sim_elapsed_ms: 400 },
            DelayedCallScheduleRequest {
                owner: mod_a,
                call_id: "late".into(),
                delay_seconds: 0.5,
                clock: DelayClock::Simulation,
                on_no_city: DelayedCallPolicyOnNoCity::Pause,
                payload: HookValue::int(2),
            },
        )
        .unwrap();
    let due_adv = queue.poll_due(
        t0,
        SimulationClockSample::Running { sim_elapsed_ms: 600 },
        &active,
    );
    assert_eq!(due_adv.len(), 1);
    assert_eq!(due_adv[0].call_id, "early");
}

#[test]
fn bridge_city_state_ecogame_cache_and_ui_protocol() {
    let mut city = AuthoritativeCityState::new();
    assert_eq!(
        city.is_city_loaded(),
        CityLoadedVerdict::Unavailable(CityStateUnavailableReason::Unknown)
    );
    city.set_state(CityContextState::NotReady);
    assert_eq!(
        city.is_city_loaded(),
        CityLoadedVerdict::Unavailable(CityStateUnavailableReason::NotReady)
    );
    city.set_state(CityContextState::NotLoaded);
    assert_eq!(city.is_city_loaded(), CityLoadedVerdict::Qualified(false));
    city.set_state(CityContextState::Loaded {
        city_name: "SimPolis".into(),
        revision: 4,
    });
    assert_eq!(city.is_city_loaded(), CityLoadedVerdict::Qualified(true));
    city.set_state(CityContextState::Stale);
    assert_eq!(
        city.is_city_loaded(),
        CityLoadedVerdict::Unavailable(CityStateUnavailableReason::Stale)
    );

    let mut key_a = EcoGameScriptCacheKey {
        game_id: 1,
        current_version: 5,
        addons: vec![EcoGameAddOnKey {
            addon_id: 0x100,
            version: 1,
        }],
    };
    let key_b = key_a.clone();
    assert!(!key_a.needs_invalidation(&key_b));
    assert_eq!(key_a.bump_version_for_hot_reload().unwrap(), 6);
    assert!(key_a.needs_invalidation(&key_b));

    let cmd = UiCommand::new("LoadRegion/region=12&mode=play").unwrap();
    assert_eq!(cmd.as_str(), "LoadRegion/region=12&mode=play");
    assert_eq!(cmd.command_name_hash(), fnv1_name_hash("loadregion"));

    let signal = UiClientEvent::from_json_value(&json!(42)).unwrap();
    assert_eq!(signal, UiClientEvent::Signal { event_id: 42 });
    let payload = UiClientEvent::from_json_value(&json!([77, {"ok": true}])).unwrap();
    assert_eq!(
        payload,
        UiClientEvent::Payload {
            event_id: 77,
            data: json!({"ok": true})
        }
    );
}

#[test]
fn locale_fallback_chain_does_not_guess_unconfigured_variants() {
    let mut chain = LocaleFallbackChain::new("en-US", &["en-GB"], "pl-PL");
    chain.insert_dictionary(
        "en-GB".to_string(),
        HashMap::from([("road.title".into(), "Avenue".into())]),
    );
    chain.insert_dictionary(
        "pl-PL".to_string(),
        HashMap::from([("road.only_pl".into(), "Droga".into())]),
    );

    assert_eq!(
        chain.resolve("road.title"),
        ResolvedText::Found {
            locale: "en-GB",
            text: "Avenue"
        }
    );
    assert_eq!(
        chain.resolve("road.only_pl"),
        ResolvedText::Found {
            locale: "pl-PL",
            text: "Droga"
        }
    );
    assert_eq!(
        chain.resolve("missing.key"),
        ResolvedText::MissingDiagnostic("missing.key".into())
    );
}

#[test]
fn localization_validates_duplicates_cycles_atomic_reload_and_0x0a98eaf0_export() {
    let cfg = ModLocalizationConfig {
        mod_id: "author.loc".into(),
        game_locale: "de-DE".into(),
        author_fallbacks: vec!["en-GB".into()],
        mod_default: "pl-PL".into(),
    };
    let mut catalog = ModLocalizationCatalog::load(
        &cfg,
        &[
            StagedLocaleFile {
                locale: "en-GB",
                relative_path: "locales/en-GB.json",
                bytes: br#"{"menu.open":"Open","only.gb":"Colour"}"#,
            },
            StagedLocaleFile {
                locale: "pl-PL",
                relative_path: "locales/pl-PL.json",
                bytes: br#"{"menu.default":"Domyslny"}"#,
            },
        ],
        1,
    )
    .unwrap();
    assert_eq!(
        catalog.resolve("only.gb"),
        ResolvedText::Found {
            locale: "en-GB",
            text: "Colour"
        }
    );
    assert_eq!(
        catalog.resolve("menu.default"),
        ResolvedText::Found {
            locale: "pl-PL",
            text: "Domyslny"
        }
    );
    assert_eq!(catalog.namespaced_key("only.gb"), "author.loc:only.gb");

    let bad_reload = catalog.stage_and_publish(
        &cfg,
        &[StagedLocaleFile {
            locale: "en-GB",
            relative_path: "locales/en-GB.json",
            bytes: br#"{"dup":"1","dup":"2"}"#,
        }],
    );
    assert!(matches!(bad_reload, Err(ScriptError::LocalizationError(_))));
    assert_eq!(catalog.generation(), 1);
    assert_eq!(
        catalog.resolve("only.gb"),
        ResolvedText::Found {
            locale: "en-GB",
            text: "Colour"
        }
    );

    let bad_cfg = ModLocalizationConfig {
        mod_id: "author.loc".into(),
        game_locale: "en-US".into(),
        author_fallbacks: vec!["pl-PL".into()],
        mod_default: "pl-PL".into(),
    };
    assert!(matches!(
        bad_cfg.validate(),
        Err(ScriptError::LocalizationError(_))
    ));

    let items = vec![
        LocaleItem {
            key: "0x00000001".into(),
            id: Some(0x0000_0001),
            text: "Witaj w Simoder".into(),
        },
        LocaleItem {
            key: "0x00000002".into(),
            id: Some(0x0000_0002),
            text: "Ratusz".into(),
        },
    ];
    let exported = export_mod_locale_resource("author.loc", &items).unwrap();
    assert!(!VANILLA_LOCALE_TABLE_IDS.contains(&exported.table_id));
    let decoded = decode_mod_locale_resource(&exported.payload).unwrap();
    assert_eq!(decoded, items);
}
