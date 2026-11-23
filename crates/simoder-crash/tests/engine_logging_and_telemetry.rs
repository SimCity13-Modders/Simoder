//! Integration tests for `simoder-core` central bounded logging pipeline (`2048` queue / `64` batch)
//! and `simoder-crash` `Simoder-OpenSCP` Backend <-> Frontend bridge observers (`BridgeTime`,
//! 14-channel pre-limiter correlation, JSON diffs, `CaptureQualityReport`), `EAWebKitClient::DebugLog`,
//! retail `SLDelta` v4 outer envelope decoding, `EA::Trace` descriptors, and rule telemetry (`CRASH-005`).

use serde_json::json;
use simoder_core::{
    DiagnosticLogPipeline, LOG_DRAIN_BATCH_SIZE, LogLevel, LogSourceType, MAX_CAPTURED_LOG_BYTES,
};
use simoder_crash::{
    BridgeChannel, BridgeCorrelationTracker, BridgeDirection, BridgePairingIssue, BridgeTime,
    CrashError, DEFAULT_BRIDGE_QUERY_QUOTA, EAWEBKIT_ADD_MESSAGE_TO_CONSOLE_RVA,
    EaWebKitConsoleKind, RETAIL_EAWEBKIT_DLL_SHA256, RETAIL_EATRACE_SERVER_TRACE_VA,
    RETAIL_EXE_SHARED_RET4_NOOP_RVA, RuleStreamContext, RuleTelemetryEvent,
    SLDELTA_RECORD_MAGIC_BE, SLDELTA_RETAIL_CONTAINER_VERSION, UI_EVENT_SPEED_CHANGED_ID,
    UI_SPEED_VALUE_PAUSED, capture_eawebkit_to_pipeline, decode_rule_action_header,
    parse_sldelta_v4_envelopes, retail_eatrace_entrypoints, validate_eawebkit_log_hook_rva,
};
use simoder_formats::DeveloperConsoleConfig;

#[test]
fn pipeline_enforces_2048_capacity_64_batch_drain_overflow_warning_and_console_filters() {
    let pipeline = DiagnosticLogPipeline::new();
    let console_cfg = DeveloperConsoleConfig {
        enabled: true,
        capture_game_logs: false,
        capture_loader_logs: true,
        capture_mod_logs: true,
        level: LogLevel::Warning,
    };

    for i in 0..2048 {
        let level = if i % 2 == 0 {
            LogLevel::Info
        } else {
            LogLevel::Warning
        };
        let source_type = if i % 4 == 0 {
            LogSourceType::Game
        } else {
            LogSourceType::Mod
        };
        assert!(pipeline.push(
            1000 + i as u64,
            source_type,
            "author.traffic",
            level,
            42,
            &format!(r"Event {i} from C:\Users\dawid\SimCity")
        ));
    }
    assert_eq!(pipeline.len(), 2048);

    // 5 overflow events beyond 2048 must be dropped without blocking.
    for _ in 0..5 {
        assert!(!pipeline.push(
            9999,
            LogSourceType::Loader,
            "simoder",
            LogLevel::Error,
            42,
            "overflow"
        ));
    }
    assert_eq!(pipeline.total_dropped(), 5);

    let batch1 = pipeline.drain_batch(5000, &console_cfg);
    assert_eq!(batch1.dropped_before_batch, 5);
    // 1 prepended overflow warning + 64 drained events = 65 file records.
    assert_eq!(batch1.file_records.len(), LOG_DRAIN_BATCH_SIZE + 1);
    assert!(
        batch1.file_records[0]
            .message
            .contains("dropped 5 event(s)")
    );
    assert!(
        batch1.file_records[1]
            .message
            .contains(r"<USERPROFILE>\SimCity")
    );
    assert_eq!(
        batch1.file_records[1].format_pipeline_line(),
        r"1000 | GAME | author.traffic | INFO | 42 | Event 0 from <USERPROFILE>\SimCity"
    );

    // Console sink filters out `GAME` records and `INFO` records (minimum level = `warn`).
    assert!(
        batch1
            .console_records
            .iter()
            .all(|r| r.source_type != LogSourceType::Game && r.level == LogLevel::Warning)
    );
}

#[test]
fn output_debug_string_truncates_4096_bytes_and_eawebkit_parses_console_messages_safely() {
    let pipeline = DiagnosticLogPipeline::with_limits(16, 16);
    let oversized = format!("{}TAIL_MARKER", "A".repeat(MAX_CAPTURED_LOG_BYTES + 200));
    assert!(pipeline.capture_output_debug_string(100, false, 77, &oversized));

    let js_err = capture_eawebkit_to_pipeline(
        &pipeline,
        105,
        77,
        r"JSError: game://localhost/resource/SCRUI_min.js (412): Uncaught TypeError at C:\Users\dawid\ui.js",
    );
    assert_eq!(js_err.kind, EaWebKitConsoleKind::JsError);
    assert_eq!(
        js_err.source.as_deref(),
        Some("game://localhost/resource/SCRUI_min.js")
    );
    assert_eq!(js_err.line, Some(412));
    assert!(!js_err.message.contains("dawid"));

    let js_warn = capture_eawebkit_to_pipeline(
        &pipeline,
        106,
        77,
        "JSWarning: game://localhost/resource/WebKitPanel.html (19): Deprecated key",
    );
    assert_eq!(js_warn.kind, EaWebKitConsoleKind::JsWarning);
    assert_eq!(js_warn.line, Some(19));

    let batch = pipeline.drain_batch(200, DeveloperConsoleConfig::default());
    assert_eq!(batch.file_records.len(), 3);
    assert_eq!(batch.file_records[0].source_name, "OutputDebugStringA");
    assert_eq!(batch.file_records[0].message.len(), MAX_CAPTURED_LOG_BYTES);
    assert!(!batch.file_records[0].message.contains("TAIL_MARKER"));
    assert_eq!(batch.file_records[1].source_name, "EAWebKit::DebugLog");
    assert_eq!(batch.file_records[1].level, LogLevel::Error);

    // Reject global detours on the shared `ret 4` stub `0x81E7C0` (retail-eawebkit-runtime.md).
    assert!(matches!(
        validate_eawebkit_log_hook_rva(RETAIL_EXE_SHARED_RET4_NOOP_RVA),
        Err(CrashError::UnsafeHookTarget(_))
    ));
    validate_eawebkit_log_hook_rva(EAWEBKIT_ADD_MESSAGE_TO_CONSOLE_RVA).unwrap();
    assert_eq!(RETAIL_EAWEBKIT_DLL_SHA256.len(), 64);
}

#[test]
fn bridge_correlation_tracker_routes_14_openscp_channels_and_preserves_mutations_during_flood() {
    assert_eq!(
        BridgeChannel::classify_request("RequestGameData", "periodTimes/0"),
        BridgeChannel::Clock
    );
    assert_eq!(
        BridgeChannel::classify_request("RequestGameData", "RegionTemplates"),
        BridgeChannel::Template
    );
    assert_eq!(
        BridgeChannel::classify_request("RequestGameData", "GetRegionDetails/12"),
        BridgeChannel::Region
    );
    assert_eq!(
        BridgeChannel::classify_request("RequestGameData", "SaveLocalGame"),
        BridgeChannel::Mutation
    );
    assert_eq!(
        BridgeChannel::classify_request("PostGameCommand", "PlaceBuilding"),
        BridgeChannel::Command
    );

    let mut tracker = BridgeCorrelationTracker::new();

    // Flood ordinary RequestGameData queries beyond DEFAULT_BRIDGE_QUERY_QUOTA (40).
    let mut admitted_queries = 0;
    for i in 0..(DEFAULT_BRIDGE_QUERY_QUOTA + 25) {
        if let Some(seq) = tracker.record_request(
            "RequestGameData",
            &format!("resourceAmount/932594546/{i}"),
            51960,
            None,
            1000 + i as u64,
        ) {
            admitted_queries += 1;
            assert!(tracker.record_response(
                seq,
                BridgeChannel::Query,
                1001 + i as u64,
                150,
                true
            ));
        }
    }
    assert_eq!(admitted_queries, DEFAULT_BRIDGE_QUERY_QUOTA);
    assert_eq!(tracker.dropped_count(BridgeChannel::Query), 25);

    // State-mutating requests (SaveLocalGame, UpdateBoxName) and Clock queries use separate channels!
    let clock_seq = tracker
        .record_request("RequestGameData", "periodTimes/0", 51960, None, 1900)
        .expect("periodTimes/0 must use dedicated Clock quota");
    assert!(tracker.record_response_with_value(
        clock_seq,
        BridgeChannel::Clock,
        1905,
        5_000,
        true,
        Some(json!(0.5)),
    ));

    let save_seq = tracker
        .record_request("RequestGameData", "SaveLocalGame", 51960, None, 2000)
        .expect("SaveLocalGame must use dedicated Mutation quota");
    assert!(tracker.record_response_with_value(
        save_seq,
        BridgeChannel::Mutation,
        2205,
        205_000,
        true,
        Some(json!({"saved": true, "slot": 1})),
    ));

    let rename_seq = tracker
        .record_request(
            "RequestGameData",
            r#"UpdateBoxName/8/NoweMiasto/{"secret":"C:\Users\dawid"}"#,
            51960,
            Some(("author.ui-helper".into(), 1)),
            2300,
        )
        .expect("UpdateBoxName must use dedicated Mutation quota");
    assert!(tracker.record_response_with_value(
        rename_seq,
        BridgeChannel::Mutation,
        2315,
        15_000,
        true,
        Some(json!({"saved": true, "slot": 2})),
    ));
    assert_eq!(tracker.dropped_count(BridgeChannel::Mutation), 0);

    // `tracker.records(BridgeChannel::Mutation)` returns both paired requests and MutationResult responses sorted by sequence!
    let mutations = tracker.records(BridgeChannel::Mutation);
    assert_eq!(mutations.len(), 4);
    assert_eq!(mutations[0].sequence, save_seq);
    assert_eq!(mutations[0].direction, BridgeDirection::UiToNative);
    assert_eq!(mutations[0].pairing_issue, None);
    assert_eq!(mutations[1].sequence, save_seq);
    assert_eq!(mutations[1].direction, BridgeDirection::NativeToUi);
    assert_eq!(mutations[1].duration_us, Some(205_000));
    assert!(!mutations[2].operation.contains("dawid"));
    assert_eq!(mutations[2].operation, "UpdateBoxName/8/NoweMiasto");

    let quality = tracker.capture_quality();
    assert_eq!(quality.dropped, 25);
    assert_eq!(quality.incomplete_requests, 0);
}

#[test]
fn openscp_bridge_time_distinguishes_stationary_clock_from_pause_and_diffs_consecutive_results() {
    let mut bridge_time = BridgeTime::new();

    // First sample at t=1000ms: invoke + result for periodTimes/0 (day fraction 0.25 -> 21600.0 seconds).
    bridge_time.observe_json_row(&json!({
        "channel": "invoke",
        "binding": "scrui.Client",
        "page": "city_hud",
        "sequence": 1,
        "time": 995,
        "requestName": "periodTimes/0"
    }));
    bridge_time.observe_json_row(&json!({
        "channel": "result",
        "binding": "scrui.Client",
        "page": "city_hud",
        "callSequence": 1,
        "time": 1000,
        "success": true,
        "result": { "value": 0.25 }
    }));
    bridge_time.finish();
    assert_eq!(bridge_time.latest_clock(), Some(21_600.0));
    assert_eq!(bridge_time.advancing(), None);
    assert_eq!(bridge_time.paused(), None);

    // Annotate an unrelated command row within 5000ms.
    let mut cmd_row = json!({
        "channel": "command",
        "binding": "scrui.Client",
        "page": "city_hud",
        "time": 2000
    });
    bridge_time.annotate_row(&mut cmd_row);
    assert_eq!(cmd_row["gameClockSeconds"], json!(21_600.0));
    assert_eq!(cmd_row["gameClockSource"], json!("ui_bridge.periodTimes/0"));
    assert_eq!(cmd_row["gameClockAgeMs"], json!(1000));

    // Identical clock sample after >= 3000ms (at t=4500ms) -> stationary clock (advancing = Some(false)),
    // but NOT paused because no pause event or gamespeed=182362487 was observed!
    bridge_time.observe_json_row(&json!({
        "channel": "invoke",
        "binding": "scrui.Client",
        "page": "city_hud",
        "sequence": 2,
        "time": 4495,
        "requestName": "periodTimes/0"
    }));
    bridge_time.observe_json_row(&json!({
        "channel": "result",
        "binding": "scrui.Client",
        "page": "city_hud",
        "callSequence": 2,
        "time": 4500,
        "success": true,
        "result": { "value": 0.25 }
    }));
    bridge_time.finish();
    assert_eq!(bridge_time.advancing(), Some(false));
    assert_eq!(bridge_time.paused(), None);

    // Explicit ReceiveGameEventFromClient SPEED_CHANGED (182429322) with value 182362487 -> paused = Some(true).
    bridge_time.observe_json_row(&json!({
        "channel": "event",
        "time": 4600,
        "eventID": UI_EVENT_SPEED_CHANGED_ID,
        "event": {
            "items": [
                {},
                {
                    "properties": [
                        {
                            "name": "speed",
                            "value": { "value": UI_SPEED_VALUE_PAUSED }
                        }
                    ]
                }
            ]
        }
    }));
    assert_eq!(bridge_time.paused(), Some(true));

    // Consecutive JSON responses for the same operation compute changed_paths and track incomplete requests.
    let mut tracker = BridgeCorrelationTracker::new();
    let seq1 = tracker
        .record_request("RequestGameData", "GetRegionDetails/1", 100, None, 5000)
        .unwrap();
    tracker.record_response_with_value(
        seq1,
        BridgeChannel::Region,
        5010,
        10_000,
        true,
        Some(json!({"name": "Alpha", "status": "idle"})),
    );

    let seq2 = tracker
        .record_request("RequestGameData", "GetRegionDetails/1", 100, None, 5100)
        .unwrap();
    let _seq3_unpaired = tracker
        .record_request("RequestGameData", "GetRegionDetails/1", 100, None, 5105)
        .unwrap();
    tracker.record_response_with_value(
        seq2,
        BridgeChannel::Region,
        5120,
        20_000,
        true,
        Some(json!({"name": "Beta", "status": "idle"})),
    );

    let region_results = tracker.records(BridgeChannel::RegionResult);
    assert_eq!(region_results.len(), 2);
    assert_eq!(region_results[1].pairing_issue, None);
    assert_eq!(
        region_results[1].changed_paths,
        vec!["$.name".to_owned()]
    );

    // Response with an unknown sequence is flagged as Unpaired.
    tracker.record_response_with_value(
        9999,
        BridgeChannel::Region,
        5130,
        5_000,
        true,
        None,
    );
    let region_results = tracker.records(BridgeChannel::RegionResult);
    assert_eq!(
        region_results[2].pairing_issue,
        Some(BridgePairingIssue::Unpaired)
    );

    let quality = tracker.capture_quality();
    assert_eq!(quality.incomplete_requests, 1);
}

#[test]
fn openscp_sldelta_v4_envelope_parser_and_retail_eatrace_descriptors_validate_retail_contracts() {
    // Build a valid retail SLDelta v4 buffer with 2 records (delta_id 10 -> delta_id 9 regression).
    let mut buf = Vec::new();
    buf.extend_from_slice(&SLDELTA_RETAIL_CONTAINER_VERSION.to_le_bytes());

    for (delta_id, sim_ticks, kind, payload) in [
        (10_u64, 120_u32, 2_u32, &[0xAA, 0xBB][..]),
        (9_u64, 125_u32, 2_u32, &[0xCC][..]),
    ] {
        let size = (24 + payload.len()) as u32;
        buf.extend_from_slice(&SLDELTA_RECORD_MAGIC_BE.to_be_bytes());
        buf.extend_from_slice(&size.to_be_bytes());
        buf.extend_from_slice(&delta_id.to_be_bytes());
        buf.extend_from_slice(&sim_ticks.to_be_bytes());
        buf.extend_from_slice(&kind.to_be_bytes());
        buf.extend_from_slice(payload);
    }

    let records = parse_sldelta_v4_envelopes(&buf, "SLDelta_0.bin").unwrap();
    assert_eq!(records.len(), 2);
    assert_eq!(records[0].delta_id, 10);
    assert_eq!(records[0].payload_bytes, 2);
    assert!(!records[0].id_regressed);
    assert_eq!(records[1].delta_id, 9);
    assert!(records[1].id_regressed);
    assert_eq!(records[1].previous_delta_id, Some(10));

    // Verify retail EA::Trace entrypoints catalog from Simoder-OpenSCP.
    let eatrace = retail_eatrace_entrypoints();
    assert_eq!(eatrace.len(), 6);
    assert!(
        eatrace
            .iter()
            .any(|e| e.retail_va == RETAIL_EATRACE_SERVER_TRACE_VA
                && e.name == "EA::Trace::Server::Trace")
    );
}

#[test]
fn rule_telemetry_decoder_requires_explicit_version_and_tracks_contextual_action2_unit_slot() {
    // Reject calls without an explicit stream_version.
    assert!(matches!(
        decode_rule_action_header(None, &[2, 0x03, 0x3B, 0]),
        Err(CrashError::RuleStreamDecode(_))
    ));

    // In retail stream_version >= 27, data2 values 62 (0x3E), 63 (0x3F), and 253 (0xFD) are direct values!
    for direct_data2 in [62_u8, 63_u8, 253_u8] {
        let hdr = decode_rule_action_header(Some(27), &[1, 0, 0, direct_data2]).unwrap();
        assert_eq!(hdr.action_id, 1);
        assert_eq!(hdr.data1, 0);
        assert_eq!(hdr.data2, u32::from(direct_data2));
        assert_eq!(hdr.bytes_consumed, 4);
    }

    // Retail 0xFE extension for data2 + 0xFFFE extension for data1, and 0xFF sentinel (-1 -> u32::MAX).
    let ext_bytes = [
        1_u8, 0xFF, 0xFE, 0xFE, // header: action 1, data1=ext, data2=ext
        0x00, 0x00, 0x10, 0x00, // data1 = 0x1000
        0x00, 0x00, 0x01, 0x3E, // data2 = 318
    ];
    let ext_hdr = decode_rule_action_header(Some(27), &ext_bytes).unwrap();
    assert_eq!(ext_hdr.data1, 0x1000);
    assert_eq!(ext_hdr.data2, 318);
    assert_eq!(ext_hdr.bytes_consumed, 12);

    let sentinel_hdr = decode_rule_action_header(Some(27), &[5, 0xFF, 0xFF, 0xFF]).unwrap();
    assert_eq!(sentinel_hdr.data1, u32::MAX);
    assert_eq!(sentinel_hdr.data2, u32::MAX);

    // Contextual unitSlot tracking: action 1 before action 2 must report unit_slot = None (never unit 0!).
    let mut ctx = RuleStreamContext::new();
    let mid_stream = decode_rule_action_header(Some(27), &[1, 0, 0, 135]).unwrap();
    assert_eq!(
        ctx.observe_header(mid_stream),
        RuleTelemetryEvent::UnitRuleApplied {
            unit_slot: None,
            state_index: 135,
        }
    );

    // Action 2 sets unitSlot = 827 (0x033B), then subsequent action 1 (with data1=0, data2=135) attributes to 827.
    let sel_slot = decode_rule_action_header(Some(27), &[2, 0x03, 0x3B, 0]).unwrap();
    assert_eq!(
        ctx.observe_header(sel_slot),
        RuleTelemetryEvent::UnitSlotSelected { unit_slot: 827 }
    );
    assert_eq!(
        ctx.observe_header(mid_stream),
        RuleTelemetryEvent::UnitRuleApplied {
            unit_slot: Some(827),
            state_index: 135,
        }
    );
}
