use std::path::{Path, PathBuf};
use std::sync::Arc;

use rquickjs::{Context, Runtime};
use simoder_d3d9::{
    BaseRuntimeHost, IntegratedSimoderRuntime, ModsActionBackend, NativeModCandidateState,
    RuntimeOnlyModsBackend, UI_OBSERVE_PACKET_JSON, UI_PATCH_APPLICATOR_ES5, build_sync_packet,
    parse_sync_reply,
};
use simoder_mods::{NativeModStatus, NativeModUiRecord, UiPatchEvent, UiViewObservation};

/// @summary Resolve the Simoder-Base source tree used by integration tests.
fn simoder_base_dir() -> PathBuf {
    std::env::var_os("SIMODER_BASE_DIR").map_or_else(
        || {
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../..")
                .join("Simoder-Base")
        },
        PathBuf::from,
    )
}

#[test]
fn base_mods_ui_composes_and_handles_native_trust_lifecycle() {
    let runtime = Arc::new(IntegratedSimoderRuntime::new());
    let sha_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    runtime.register_native_candidate(NativeModCandidateState {
        record: NativeModUiRecord {
            mod_id: "simoder.mcp".into(),
            name: "Simoder MCP Native Mod".into(),
            version: "0.2.0".into(),
            entry: "simoder_mcp.dll".into(),
            sha256: sha_a.into(),
            status: NativeModStatus::PendingApproval,
            diagnostic: None,
        },
        dll_path: PathBuf::from("simoder_mcp.dll"),
        mapped_image_bytes: 65536,
        source_size_bytes: 12345,
        owner: None,
        trust_path: PathBuf::from("native_trust.toon"),
    });

    let backend: Arc<dyn ModsActionBackend> = Arc::new(RuntimeOnlyModsBackend::new(runtime.clone()));
    let host = BaseRuntimeHost::start_from_base_dir(&simoder_base_dir(), runtime.clone(), backend.clone())
        .expect("Simoder-Base DAG should boot cleanly");

    runtime.set_ui_observation(UiViewObservation {
        phase: "launcher.menu".into(),
        active_views: vec!["launcher.menu".into()],
        locale: "en-us".into(),
    });
    host.tick_ui().expect("initial launcher UI tick should pass");

    let (_rev, patches) = runtime.composed_ui_snapshot();
    assert!(patches.iter().any(|p| {
        p.patch_id == "simoder.base.mods-trigger-launcher"
            && p.view == "launcher.menu"
            && p.node.badge.as_deref() == Some("1")
    }));
    assert!(patches.iter().any(|p| p.patch_id == "simoder.base.mods-trigger-main"));
    assert!(patches.iter().any(|p| p.patch_id == "simoder.base.mods-trigger-ingame"));

    // A second mod contributes a button into main.menu menu.actions using Simoder-Base's shared UiKit.
    host.run_sandboxed_mod_script(
        "city.tweaks",
        1,
        r#"
            return {
                init = function(api)
                    assert(type(api.ui.kit) == "table" and type(api.ui.kit.Button) == "function")
                    api.ui.register({
                        id = "quick-camera",
                        view = "main.menu",
                        slot = "menu.actions",
                        op = "append",
                        priority = 10,
                        node = api.ui.kit.Button({
                            id = "camera_btn",
                            label = "Camera Tweaks",
                            action = "toggle_cam",
                        }),
                    })
                end,
            }
        "#,
        "init",
    )
    .expect("mod should be able to compose onto main.menu alongside mods-ui");

    // Mod attempts raw html injection or private host.mods access must be blocked.
    let err = host
        .run_sandboxed_mod_script(
            "bad.mod",
            1,
            r#"
                return {
                    init = function(api)
                        assert(api.mods == nil)
                        api.ui.register({
                            id = "xss",
                            view = "main.menu",
                            slot = "menu.actions",
                            node = { kind = "panel", html = "<script>alert(1)</script>" },
                        })
                    end,
                }
            "#,
            "init",
        )
        .expect_err("raw html key must be rejected");
    assert!(err.contains("UI_FORBIDDEN_RAW_KEY"));

    host.tick_ui().expect("second mod compose tick should succeed");
    let (_rev, main_patches) = runtime.composed_ui_snapshot();
    let main_slot_patches: Vec<&str> = main_patches
        .iter()
        .filter(|p| p.view == "main.menu" && p.slot == "menu.actions")
        .map(|p| p.patch_id.as_str())
        .collect();
    assert_eq!(
        main_slot_patches,
        vec!["city.tweaks.quick-camera", "simoder.base.mods-trigger-main"]
    );

    // Clicking Mods on launcher opens the modal overlay localized to Polish when locale changes.
    runtime.set_ui_observation(UiViewObservation {
        phase: "launcher.menu".into(),
        active_views: vec!["launcher.menu".into()],
        locale: "pl-pl".into(),
    });
    runtime.push_ui_events([UiPatchEvent {
        patch_id: "simoder.base.mods-trigger-launcher".into(),
        action: "open_mods".into(),
        target_id: Some("mods_trigger_btn".into()),
        value: None,
    }]);
    host.tick_ui().expect("open_mods click should open modal");

    let (_rev, modal_patches) = runtime.composed_ui_snapshot();
    let modal = modal_patches
        .iter()
        .find(|p| p.patch_id == "simoder.base.mods-modal")
        .expect("mods modal must appear");
    assert_eq!(modal.node.action.as_deref(), Some("close_mods"));
    assert!(
        modal
            .node
            .children
            .iter()
            .any(|c| c.tone.as_deref() == Some("footer")),
        "mods modal must expose footer action row"
    );
    let modal_json = serde_json::to_string(&modal.node).unwrap();
    assert!(modal_json.contains("Zatwierdź i załaduj"));
    assert!(modal_json.contains(sha_a));

    // Reject wrong SHA-256 directly through host backend before testing UI click approval.
    assert!(backend.approve_native("simoder.mcp", "deadbeef").is_err());

    // Simulate clicking Approve & Load inside modal.
    runtime.push_ui_events([UiPatchEvent {
        patch_id: "simoder.base.mods-modal".into(),
        action: "approve_native".into(),
        target_id: Some("native_approve_simoder.mcp".into()),
        value: None,
    }]);
    host.tick_ui().expect("approve click should promote native mod to Active");
    assert_eq!(
        runtime.get_native_candidate("simoder.mcp").unwrap().record.status,
        NativeModStatus::Active
    );

    let (_rev, updated) = runtime.composed_ui_snapshot();
    let trigger = updated
        .iter()
        .find(|p| p.patch_id == "simoder.base.mods-trigger-main")
        .unwrap();
    assert_eq!(trigger.node.label.as_deref(), Some("Mody"));
    assert_eq!(trigger.node.badge, None);

    // Simulate clicking Revoke inside modal.
    runtime.push_ui_events([UiPatchEvent {
        patch_id: "simoder.base.mods-modal".into(),
        action: "revoke_native".into(),
        target_id: Some("native_revoke_simoder.mcp".into()),
        value: None,
    }]);
    host.tick_ui().expect("revoke click should deny native mod");
    assert_eq!(
        runtime.get_native_candidate("simoder.mcp").unwrap().record.status,
        NativeModStatus::Denied
    );

    // Switch to Installed Mods tab and exercise search filter + UiKit controls from a third-party mod.
    runtime.push_ui_events([UiPatchEvent {
        patch_id: "simoder.base.mods-modal".into(),
        action: "switch_tab".into(),
        target_id: Some("mods".into()),
        value: Some("mods".into()),
    }]);
    host.tick_ui().expect("switch_tab click should select mods tab");

    runtime.push_ui_events([UiPatchEvent {
        patch_id: "simoder.base.mods-modal".into(),
        action: "filter_mods".into(),
        target_id: Some("mods_search_input".into()),
        value: Some("nonexistent-xyz".into()),
    }]);
    host.tick_ui().expect("filter_mods should re-render filtered list");
    let (_rev, filtered_patches) = runtime.composed_ui_snapshot();
    let filtered_modal = filtered_patches
        .iter()
        .find(|p| p.patch_id == "simoder.base.mods-modal")
        .unwrap();
    let filtered_json = serde_json::to_string(&filtered_modal.node).unwrap();
    assert!(filtered_json.contains("Żaden mod nie pasuje do filtra."));

    host.run_sandboxed_mod_script(
        "city.extended",
        1,
        r#"
            return {
                init = function(api)
                    local k = api.ui.kit
                    api.ui.register({
                        id = "advanced-controls",
                        view = "ingame.options",
                        slot = "menu.actions",
                        node = k.Card({
                            id = "ext_card",
                            label = "Extended Options",
                            children = {
                                k.Tabs({
                                    id = "ext_tabs",
                                    activeTab = "display",
                                    action = "ext_tab",
                                    tabs = {
                                        k.Tab({
                                            id = "display",
                                            label = "Display",
                                            children = {
                                                k.Input({ id = "preset_name", value = "High", placeholder = "Preset", action = "set_name" }),
                                                k.Select({ id = "aa_mode", value = "fxaa", action = "set_aa", options = {
                                                    { value = "off", label = "Off" },
                                                    { value = "fxaa", label = "FXAA" },
                                                }}),
                                                k.Slider({ id = "gamma", label = "Gamma", min = 50, max = 150, step = 10, value = 100, action = "set_gamma" }),
                                                k.Checkbox({ id = "vsync", label = "VSync", checked = true, action = "set_vsync" }),
                                                k.ProgressBar({ id = "shader_warm", label = "Shader Cache", min = 0, max = 100, value = 75 }),
                                            },
                                        }),
                                        k.Tab({ id = "audio", label = "Audio", children = { k.MutedText("Default") } }),
                                    },
                                }),
                            },
                        }),
                    })
                end,
            }
        "#,
        "init",
    )
    .expect("third-party mod must construct extended UiKit primitives cleanly");

    // Simulate clicking Close inside modal removes the modal overlay cleanly.
    runtime.push_ui_events([UiPatchEvent {
        patch_id: "simoder.base.mods-modal".into(),
        action: "close_mods".into(),
        target_id: Some("mods_close".into()),
        value: None,
    }]);
    host.tick_ui().expect("close click should dismiss mods modal");
    let (_rev, after_close) = runtime.composed_ui_snapshot();
    assert!(
        !after_close
            .iter()
            .any(|p| p.patch_id == "simoder.base.mods-modal"),
        "mods modal must be removed after close_mods action"
    );
}

#[test]
fn es5_dom_applicator_executes_under_quickjs_and_emits_click_events() {
    let runtime = Arc::new(IntegratedSimoderRuntime::new());
    runtime.register_native_candidate(NativeModCandidateState {
        record: NativeModUiRecord {
            mod_id: "example.native".into(),
            name: "Example Native".into(),
            version: "1.0.0".into(),
            entry: "example.dll".into(),
            sha256: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb".into(),
            status: NativeModStatus::PendingApproval,
            diagnostic: None,
        },
        dll_path: PathBuf::from("example.dll"),
        mapped_image_bytes: 4096,
        source_size_bytes: 1024,
        owner: None,
        trust_path: PathBuf::from("native_trust.toon"),
    });
    let backend: Arc<dyn ModsActionBackend> = Arc::new(RuntimeOnlyModsBackend::new(runtime.clone()));
    let luau = BaseRuntimeHost::start_from_base_dir(&simoder_base_dir(), runtime.clone(), backend)
        .unwrap();
    luau.tick_ui().unwrap();
    let (rev, patches) = runtime.composed_ui_snapshot();
    let sync_packet = build_sync_packet(rev, &patches).unwrap();

    let js_rt = Runtime::new().expect("QuickJS runtime");
    js_rt.set_memory_limit(8 * 1024 * 1024);
    let js_ctx = Context::full(&js_rt).expect("QuickJS context");
    js_ctx.with(|ctx| {
        let dom_shim = r#"
            function MockEl(tag) {
                this.tagName = tag.toUpperCase();
                this.id = "";
                this.className = "";
                this.textContent = "";
                this.type = "";
                this.value = "";
                this.placeholder = "";
                this.checked = false;
                this.disabled = false;
                this.style = {};
                this.attributes = {};
                this.children = [];
                this.parentNode = null;
                this.onclick = null;
                this.onchange = null;
                this.onkeydown = null;
            }
            MockEl.prototype.setAttribute = function(k, v) { this.attributes[k] = String(v); };
            MockEl.prototype.appendChild = function(ch) {
                if (ch.parentNode) { ch.parentNode.removeChild(ch); }
                ch.parentNode = this;
                this.children.push(ch);
                return ch;
            };
            MockEl.prototype.removeChild = function(ch) {
                var idx = this.children.indexOf(ch);
                if (idx >= 0) { this.children.splice(idx, 1); ch.parentNode = null; }
                return ch;
            };
            MockEl.prototype.insertBefore = function(ch, ref) {
                if (!ref) { return this.appendChild(ch); }
                if (ch.parentNode) { ch.parentNode.removeChild(ch); }
                var idx = this.children.indexOf(ref);
                ch.parentNode = this;
                if (idx >= 0) { this.children.splice(idx, 0, ch); }
                else { this.children.push(ch); }
                return ch;
            };
            Object.defineProperty(MockEl.prototype, "firstChild", {
                get: function() { return this.children.length > 0 ? this.children[0] : null; }
            });
            Object.defineProperty(MockEl.prototype, "nextSibling", {
                get: function() {
                    if (!this.parentNode) return null;
                    var arr = this.parentNode.children;
                    var idx = arr.indexOf(this);
                    return (idx >= 0 && idx + 1 < arr.length) ? arr[idx + 1] : null;
                }
            });
            function findIn(node, pred, out) {
                if (pred(node)) { out.push(node); }
                for (var i = 0; i < node.children.length; i += 1) {
                    findIn(node.children[i], pred, out);
                }
            }
            MockEl.prototype.querySelectorAll = function(sel) {
                var res = [];
                if (sel === "[data-simoder-slot]") {
                    findIn(this, function(n) { return !!n.attributes["data-simoder-slot"]; }, res);
                }
                return res;
            };
            MockEl.prototype.querySelector = function(sel) {
                var res = [];
                var m = sel.match(/^\[data-simoder-target='([^']+)'\]$/);
                if (m) {
                    findIn(this, function(n) { return n.attributes["data-simoder-target"] === m[1]; }, res);
                }
                return res.length > 0 ? res[0] : null;
            };
            var head = new MockEl("head");
            var body = new MockEl("body");
            var root = new MockEl("html");
            root.appendChild(head);
            root.appendChild(body);
            var doc = {
                head: head,
                body: body,
                createElement: function(tag) { return new MockEl(tag); },
                getElementById: function(id) {
                    var res = [];
                    findIn(root, function(n) { return n.id === id; }, res);
                    return res.length > 0 ? res[0] : null;
                },
                querySelectorAll: function(sel) { return root.querySelectorAll(sel); }
            };
            var window = {
                document: doc,
                simcity: { gUpdater: {} },
                Client: { RequestUpdaterData: function() {} },
                scrui: { gUIManager: { mLocaleManager: { mLocaleCode: "en-us" } } }
            };
            var document = doc;
        "#;
        ctx.eval::<(), _>(dom_shim).expect("mock DOM setup");

        let invoke_expr = format!("var __simoderApply = {UI_PATCH_APPLICATOR_ES5};");
        ctx.eval::<(), _>(invoke_expr.as_str())
            .expect("UI_PATCH_APPLICATOR_ES5 must parse cleanly in strict ES5 QuickJS");

        let obs_fn: rquickjs::Function = ctx.globals().get("__simoderApply").unwrap();
        let obs_raw: String = obs_fn.call((UI_OBSERVE_PACKET_JSON,)).unwrap();
        let obs_reply = parse_sync_reply(&obs_raw).unwrap();
        let obs = obs_reply.observation.unwrap();
        assert_eq!(obs.phase, "launcher.menu");
        assert_eq!(obs.locale, "en-us");

        let sync_raw: String = obs_fn.call((sync_packet.as_str(),)).unwrap();
        let sync_reply = parse_sync_reply(&sync_raw).unwrap();
        assert!(sync_reply.events.is_empty());

        // Trigger button exists in DOM and clicking it queues open_mods event for next poll.
        ctx.eval::<(), _>(
            "var btn = document.getElementById('simoder-patch-simoder.base.mods-trigger-launcher'); if (!btn || !btn.onclick) { throw new Error('missing launcher trigger button'); } btn.onclick();",
        )
        .unwrap();

        let poll_raw: String = obs_fn.call((sync_packet.as_str(),)).unwrap();
        let poll_reply = parse_sync_reply(&poll_raw).unwrap();
        assert_eq!(poll_reply.events.len(), 1);
        assert_eq!(poll_reply.events[0].action, "open_mods");
        assert_eq!(
            poll_reply.events[0].patch_id,
            "simoder.base.mods-trigger-launcher"
        );
    });

    // Simulate open_mods in Luau and verify modal header, scroll panels, footer, and close action inside QuickJS DOM.
    runtime.push_ui_events([UiPatchEvent {
        patch_id: "simoder.base.mods-trigger-launcher".into(),
        action: "open_mods".into(),
        target_id: Some("mods_trigger_btn".into()),
        value: None,
    }]);
    luau.tick_ui().unwrap();
    let (modal_rev, modal_patches) = runtime.composed_ui_snapshot();
    let modal_sync = build_sync_packet(modal_rev, &modal_patches).unwrap();

    js_ctx.with(|ctx| {
        let obs_fn: rquickjs::Function = ctx.globals().get("__simoderApply").unwrap();
        let reply_raw: String = obs_fn.call((modal_sync.as_str(),)).unwrap();
        let reply = parse_sync_reply(&reply_raw).unwrap();
        assert!(reply.events.is_empty());

        ctx.eval::<(), _>(
            "var footerBtn = document.getElementById('simoder-patch-simoder.base.mods-modal'); \
             if (!footerBtn) throw new Error('missing modal patch element'); \
             var allBtns = footerBtn.children; \
             var foundHeader = false, foundBody = false, foundFooter = false; \
             for (var k = 0; k < allBtns.length; k += 1) { \
                 if (allBtns[k].className.indexOf('simoder-modal-header') !== -1) foundHeader = true; \
                 if (allBtns[k].className.indexOf('simoder-modal-body') !== -1) foundBody = true; \
                 if (allBtns[k].className.indexOf('simoder-modal-footer') !== -1) foundFooter = true; \
             } \
             if (!foundHeader || !foundBody || !foundFooter) throw new Error('missing header/body/footer layout'); \
             var hdrClose = footerBtn.children[0].children[0]; \
             if (!hdrClose || !hdrClose.onclick) throw new Error('missing header close button'); \
             hdrClose.onclick();",
        )
        .unwrap();

        let close_raw: String = obs_fn.call((modal_sync.as_str(),)).unwrap();
        let close_reply = parse_sync_reply(&close_raw).unwrap();
        assert_eq!(close_reply.events.len(), 1);
        assert_eq!(close_reply.events[0].action, "close_mods");
        assert_eq!(
            close_reply.events[0].target_id.as_deref(),
            Some("modal_header_close")
        );

        // Verify extended ES5 primitives (tabs, input, select, slider, checkbox, progress).
        let ext_patch = simoder_mods::UiPatchDescriptor {
            patch_id: "ext.demo.controls".into(),
            owner_mod_id: "ext.demo".into(),
            owner_generation: 1,
            view: "launcher.menu".into(),
            slot: "menu.actions".into(),
            op: simoder_mods::UiPatchOp::Append,
            target: None,
            priority: 200,
            node: simoder_mods::UiNode {
                kind: simoder_mods::UiNodeKind::Tabs,
                id: Some("ext_tabs".into()),
                value: Some("t1".into()),
                action: Some("switch_tab".into()),
                children: vec![
                    simoder_mods::UiNode {
                        kind: simoder_mods::UiNodeKind::Tab,
                        id: Some("t1".into()),
                        label: Some("One".into()),
                        children: vec![
                            simoder_mods::UiNode {
                                kind: simoder_mods::UiNodeKind::Input,
                                id: Some("q".into()),
                                value: Some("init".into()),
                                action: Some("filter".into()),
                                ..Default::default()
                            },
                            simoder_mods::UiNode {
                                kind: simoder_mods::UiNodeKind::Select,
                                id: Some("sel".into()),
                                value: Some("b".into()),
                                action: Some("pick".into()),
                                options: vec![
                                    simoder_mods::UiSelectOption { value: "a".into(), label: "A".into() },
                                    simoder_mods::UiSelectOption { value: "b".into(), label: "B".into() },
                                ],
                                ..Default::default()
                            },
                            simoder_mods::UiNode {
                                kind: simoder_mods::UiNodeKind::Slider,
                                id: Some("sl".into()),
                                label: Some("Scale".into()),
                                min: Some(0),
                                max: Some(200),
                                step: Some(10),
                                value: Some("120".into()),
                                action: Some("slide".into()),
                                ..Default::default()
                            },
                            simoder_mods::UiNode {
                                kind: simoder_mods::UiNodeKind::Checkbox,
                                id: Some("cb".into()),
                                label: Some("Opt".into()),
                                checked: Some(false),
                                action: Some("chk".into()),
                                ..Default::default()
                            },
                            simoder_mods::UiNode {
                                kind: simoder_mods::UiNodeKind::Progress,
                                id: Some("pb".into()),
                                min: Some(0),
                                max: Some(100),
                                value: Some("60".into()),
                                ..Default::default()
                            },
                        ],
                        ..Default::default()
                    },
                    simoder_mods::UiNode {
                        kind: simoder_mods::UiNodeKind::Tab,
                        id: Some("t2".into()),
                        label: Some("Two".into()),
                        ..Default::default()
                    },
                ],
                ..Default::default()
            },
        };
        let ext_sync = build_sync_packet(99, &[ext_patch]).unwrap();
        let _ = obs_fn.call::<_, String>((ext_sync.as_str(),)).unwrap();
        ctx.eval::<(), _>(
            "var t = document.getElementById('simoder-patch-ext.demo.controls'); \
             if (!t) throw new Error('missing ext tabs'); \
             var bar = t.children[0], body = t.children[1]; \
             bar.children[1].onclick(); \
             var inpWrap = body.children[0]; inpWrap.children[0].value = 'abc'; inpWrap.children[0].onchange(); \
             var selWrap = body.children[1]; selWrap.children[0].value = 'a'; selWrap.children[0].onchange(); \
             var sldWrap = body.children[2]; sldWrap.children[1].value = '150'; sldWrap.children[1].onchange(); \
             var chkWrap = body.children[3]; chkWrap.children[0].checked = true; chkWrap.children[0].onchange(); \
             var prgFill = body.children[4].children[1].children[0]; \
             if (prgFill.style.width !== '60%') throw new Error('bad progress width: ' + prgFill.style.width);",
        )
        .unwrap();
        let ext_raw: String = obs_fn.call((ext_sync.as_str(),)).unwrap();
        let ext_reply = parse_sync_reply(&ext_raw).unwrap();
        let acts: Vec<(&str, Option<&str>)> = ext_reply
            .events
            .iter()
            .map(|e| (e.action.as_str(), e.value.as_deref()))
            .collect();
        assert_eq!(
            acts,
            vec![
                ("switch_tab", Some("t2")),
                ("filter", Some("abc")),
                ("pick", Some("a")),
                ("slide", Some("150")),
                ("chk", Some("true")),
            ]
        );

        // Unrelated advertisement / billboard webviews without window.Client/simcity must report phase="cold".
        ctx.eval::<(), _>("window.__simoderMockPhase = ''; window.Client = null; window.simcity = null;").unwrap();
        let cold_raw: String = obs_fn.call((UI_OBSERVE_PACKET_JSON,)).unwrap();
        let cold_reply = parse_sync_reply(&cold_raw).unwrap();
        assert_eq!(cold_reply.observation.as_ref().unwrap().phase, "cold");
        assert!(cold_reply.observation.as_ref().unwrap().active_views.is_empty());
    });
}

#[test]
fn base_mods_ui_supports_region_mods_context_isolation() {
    let runtime = Arc::new(IntegratedSimoderRuntime::new());
    let backend: Arc<dyn ModsActionBackend> = Arc::new(RuntimeOnlyModsBackend::new(runtime.clone()));
    let host = BaseRuntimeHost::start_from_base_dir(&simoder_base_dir(), runtime.clone(), backend)
        .expect("Simoder-Base DAG should boot cleanly");

    runtime.set_ui_observation(UiViewObservation {
        phase: "main.menu".into(),
        active_views: vec!["main.menu".into()],
        locale: "en-us".into(),
    });
    host.tick_ui().expect("initial main menu tick should pass");

    let (_rev, patches) = runtime.composed_ui_snapshot();
    assert!(
        patches
            .iter()
            .any(|p| p.patch_id == "simoder.base.mods-trigger-region" && p.slot == "region.actions"),
        "mods-trigger-region must be registered on region.actions"
    );

    runtime.push_ui_events([UiPatchEvent {
        patch_id: "simoder.base.mods-trigger-region".into(),
        action: "open_region_mods".into(),
        target_id: Some("mods_region_trigger_btn".into()),
        value: Some(r#"{"regionId":"6","regionName":"Simoder Sandbox"}"#.into()),
    }]);
    host.tick_ui().expect("open_region_mods should open region modal");

    let (_rev, modal_patches) = runtime.composed_ui_snapshot();
    let modal = modal_patches
        .iter()
        .find(|p| p.patch_id == "simoder.base.mods-modal")
        .expect("region mods modal must appear");
    assert!(
        modal.node.label.as_deref().unwrap_or("").contains("Simoder Sandbox (#6)"),
        "modal title should reflect selected region context"
    );

    runtime
        .set_region_mod_enabled("6", "city.tweaks", true)
        .unwrap();
    runtime
        .set_region_mod_enabled("7", "city.tweaks", false)
        .unwrap();
    assert_eq!(runtime.get_region_mods("6").get("city.tweaks"), Some(&true));
    assert_eq!(runtime.get_region_mods("7").get("city.tweaks"), Some(&false));
}

#[test]
fn base_hot_reload_swaps_scripts_and_applicator_and_rejects_broken_edits() {
    let tmp = tempfile::tempdir().unwrap();
    let base_src = simoder_base_dir();
    for entry in walkdir::WalkDir::new(&base_src)
        .follow_links(false)
        .into_iter()
        .filter_map(Result::ok)
    {
        if !entry.file_type().is_file() {
            continue;
        }
        let rel = entry.path().strip_prefix(&base_src).unwrap();
        let rel_str = rel.to_string_lossy().replace('\\', "/");
        if !rel_str.starts_with("scripts/") && !rel_str.starts_with("ui/") && rel_str != "mod.toml" {
            continue;
        }
        let dst = tmp.path().join(rel);
        std::fs::create_dir_all(dst.parent().unwrap()).unwrap();
        std::fs::copy(entry.path(), dst).unwrap();
    }

    let runtime = Arc::new(IntegratedSimoderRuntime::new());
    let backend: Arc<dyn ModsActionBackend> = Arc::new(RuntimeOnlyModsBackend::new(runtime.clone()));
    let mut host = BaseRuntimeHost::start_from_base_dir(tmp.path(), runtime.clone(), backend.clone())
        .expect("temp Simoder-Base should boot");
    let mut watcher = simoder_d3d9::BaseHotReloadState::new(tmp.path().to_path_buf());
    assert_eq!(watcher.reload_gen(), 0);
    assert!(
        watcher
            .check_and_reload(&mut host, runtime.clone(), backend.clone())
            .unwrap()
            .is_none(),
        "unchanged files must not trigger hot reload"
    );

    // Modify both Luau script and ui/applicator.js on disk.
    let mods_ui_path = tmp.path().join("scripts/services/mods-ui.luau");
    let mut mods_ui_src = std::fs::read_to_string(&mods_ui_path).unwrap();
    mods_ui_src.push_str("\n-- hot-reload test marker\n");
    std::fs::write(&mods_ui_path, mods_ui_src).unwrap();

    let app_path = tmp.path().join("ui/applicator.js");
    let mut app_src = std::fs::read_to_string(&app_path).unwrap();
    app_src.push_str("\n/* hot-reload-marker */\n");
    std::fs::write(&app_path, app_src).unwrap();

    let report = watcher
        .check_and_reload(&mut host, runtime.clone(), backend.clone())
        .expect("valid edit should hot reload cleanly")
        .expect("changed files should report reload");
    assert_eq!(report.reload_gen, 1);
    assert!(report.external_applicator);
    assert!(watcher.applicator_script().contains("hot-reload-marker"));
    let (_rev, reloaded_patches) = runtime.composed_ui_snapshot();
    assert!(
        reloaded_patches
            .iter()
            .any(|p| p.patch_id == "simoder.base.mods-trigger-main"),
        "dropping old BaseRuntimeHost must not leave composed patches wiped after HotReload"
    );

    // Introduce a Luau syntax error and ensure the previous working VM stays active.
    std::fs::write(&mods_ui_path, "this is invalid luau @@@").unwrap();
    let err = watcher
        .check_and_reload(&mut host, runtime.clone(), backend)
        .expect_err("broken Luau edit must be rejected");
    assert!(!err.is_empty());
    assert_eq!(watcher.reload_gen(), 1);
    host.tick_ui()
        .expect("previous working Luau VM must remain functional after failed hot reload");
}

#[test]
fn jsx_runtime_emits_valid_ui_nodes_with_tailwind_and_native_classes_and_renders_in_applicator() {
    let jsx_src = std::fs::read_to_string(simoder_base_dir().join("ui/jsx-runtime.js"))
        .expect("Simoder-Base/ui/jsx-runtime.js must exist");
    let stripped = jsx_src.replace("\nexport ", "\n").replace("export ", "");

    let rt = Runtime::new().unwrap();
    let ctx = Context::full(&rt).unwrap();
    let node_json: String = ctx.with(|ctx| {
        let script = format!(
            r#"
            const __jsxRt = (function () {{
                {stripped}
                return {{ jsx, jsxs, Fragment, Modal, Tabs, Tab, Panel, PrimaryButton, Text, NativeClasses }};
            }})();
            const {{ jsx, jsxs, Fragment, Modal, Tabs, Tab, Panel, PrimaryButton, Text, NativeClasses }} = __jsxRt;

            function DemoBody(props) {{
                return jsxs(Fragment, {{
                    children: [
                        jsx(Text, {{ className: "text-sm font-bold mb-2", children: props.heading }}),
                        jsx(Panel, {{
                            id: "demo_panel",
                            className: "p-2 mb-2 " + NativeClasses.RecessedTile,
                            children: jsx(Text, {{ text: "Vanilla & Tailwind" }}),
                        }}),
                    ],
                }});
            }}

            const tree = jsx(Modal, {{
                id: "demo_modal",
                title: "TSX Demo",
                closeAction: "close_demo",
                className: NativeClasses.PanelBackground,
                footer: [jsx(PrimaryButton, {{ id: "ok_btn", label: "OK", action: "confirm_demo", className: "px-3" }})],
                children: jsx(Tabs, {{
                    id: "demo_tabs",
                    activeTab: "main",
                    action: "switch_tab",
                    children: jsx(Tab, {{
                        id: "main",
                        label: "Main",
                        children: jsx(DemoBody, {{ heading: "Hello JSX" }}),
                    }}),
                }}),
            }});

            JSON.stringify(tree);
            "#
        );
        ctx.eval::<String, _>(script.as_str())
            .expect("JSX runtime script must evaluate cleanly")
    });

    let node: simoder_mods::UiNode =
        serde_json::from_str(&node_json).expect("JSX output must deserialize into UiNode");
    let mut registry = simoder_mods::UiPatchRegistry::new();
    registry
        .register_patch(simoder_mods::UiPatchDescriptor {
            patch_id: "jsx.demo.modal".into(),
            owner_mod_id: "jsx.demo".into(),
            owner_generation: 1,
            view: "launcher.menu".into(),
            slot: "modal.overlay".into(),
            op: simoder_mods::UiPatchOp::Append,
            target: None,
            priority: 10,
            node,
        })
        .expect("JSX UiNode must pass simoder-mods validation");
    let (rev, composed, conflicts) = registry.composed_patches_for_view("launcher.menu");
    assert!(conflicts.is_empty());
    let sync_pkt = build_sync_packet(rev, &composed).unwrap();

    let dom_rt = Runtime::new().unwrap();
    let dom_ctx = Context::full(&dom_rt).unwrap();
    dom_ctx.with(|ctx| {
        let dom_shim = r#"
            var __nodesById = {};
            function makeEl(tag) {
                var el = {
                    tagName: String(tag).toUpperCase(),
                    _id: '',
                    className: '',
                    textContent: '',
                    style: {},
                    attrs: {},
                    children: [],
                    childNodes: [],
                    parentNode: null,
                    setAttribute: function (k, v) { this.attrs[k] = String(v); },
                    getAttribute: function (k) { return this.attrs[k] !== undefined ? this.attrs[k] : null; },
                    appendChild: function (c) { c.parentNode = this; this.children.push(c); this.childNodes.push(c); return c; },
                    removeChild: function (c) {
                        var idx = this.children.indexOf(c);
                        if (idx !== -1) { this.children.splice(idx, 1); this.childNodes.splice(idx, 1); }
                        c.parentNode = null;
                    },
                    querySelectorAll: function (sel) {
                        if (sel === '[data-simoder-slot]') {
                            var res = [];
                            function walk(n) {
                                if (n.attrs && n.attrs['data-simoder-slot']) res.push(n);
                                for (var i = 0; i < n.children.length; i += 1) walk(n.children[i]);
                            }
                            walk(this);
                            return res;
                        }
                        return [];
                    },
                    querySelector: function () { return null; }
                };
                Object.defineProperty(el, 'id', {
                    get: function () { return this._id; },
                    set: function (v) { this._id = String(v); if (v) __nodesById[this._id] = this; }
                });
                return el;
            }
            var doc = {
                head: makeEl('head'),
                body: makeEl('body'),
                documentElement: { clientWidth: 1280, clientHeight: 720 },
                createElement: function (t) { return makeEl(t); },
                getElementById: function (id) { return __nodesById[id] || null; },
                querySelectorAll: function (sel) { return doc.body.querySelectorAll(sel); }
            };
            var window = {
                document: doc,
                innerWidth: 1280,
                innerHeight: 720,
                simcity: { gUpdater: {} },
                Client: { RequestUpdaterData: function () {} },
                scrui: { gUIManager: { mLocaleManager: { mLocaleCode: 'en-us' } } }
            };
            var document = doc;
        "#;
        ctx.eval::<(), _>(dom_shim).unwrap();
        let invoke_expr = format!("var __simoderApply = {UI_PATCH_APPLICATOR_ES5};");
        ctx.eval::<(), _>(invoke_expr.as_str()).unwrap();
        let apply_fn: rquickjs::Function = ctx.globals().get("__simoderApply").unwrap();
        let raw: String = apply_fn.call((sync_pkt.as_str(),)).unwrap();
        let reply = parse_sync_reply(&raw).unwrap();
        assert!(reply.ok);

        ctx.eval::<(), _>(
            r#"
            var modal = document.getElementById('simoder-patch-jsx.demo.modal');
            if (!modal) throw new Error('missing JSX modal');
            if (modal.className.indexOf('Panel_Background') === -1) throw new Error('missing Panel_Background');
            var styleTag = document.getElementById('simoder-ui-styles');
            if (!styleTag || styleTag.textContent.indexOf('.p-2{padding:8px;}') === -1) {
                throw new Error('missing Tailwind subset CSS in applicator');
            }
            function findTarget(n, t) {
                if (n.attrs && n.attrs['data-simoder-target'] === t) return n;
                for (var i = 0; i < n.children.length; i += 1) {
                    var found = findTarget(n.children[i], t);
                    if (found) return found;
                }
                return null;
            }
            var panel = findTarget(modal, 'demo_panel');
            if (!panel || panel.className.indexOf('p-2 mb-2 Browse_Tile_Recessed_SharpCorners') === -1) {
                throw new Error('missing Tailwind + NativeClasses on JSX panel: ' + (panel ? panel.className : 'null'));
            }
            "#,
        )
        .expect("rendered JSX DOM must preserve both NativeClasses and Tailwind utility classes");
    });
}

#[test]
fn base_mods_ui_auto_updater_and_manual_modal_dialog() {
    let runtime = Arc::new(IntegratedSimoderRuntime::new());
    let backend: Arc<dyn ModsActionBackend> = Arc::new(RuntimeOnlyModsBackend::new(runtime.clone()));
    let host = BaseRuntimeHost::start_from_base_dir(&simoder_base_dir(), runtime.clone(), backend)
        .expect("Simoder-Base DAG should boot cleanly with auto-updater");

    runtime.set_ui_observation(UiViewObservation {
        phase: "main.menu".into(),
        active_views: vec!["main.menu".into()],
        locale: "en-us".into(),
    });
    host.tick_ui().expect("initial main menu UI tick should pass");

    // Open Mods modal.
    runtime.push_ui_events([UiPatchEvent {
        patch_id: "simoder.base.mods-trigger-main".into(),
        action: "open_mods".into(),
        target_id: Some("mods_trigger_btn".into()),
        value: None,
    }]);
    host.tick_ui().expect("tick after open_mods should succeed");

    let (_rev, patches) = runtime.composed_ui_snapshot();
    let modal_patch = patches.iter().find(|p| p.patch_id == "simoder.base.mods-modal");
    assert!(modal_patch.is_some(), "mods-modal must be composed on open_mods");

    // Trigger manual modal open.
    runtime.push_ui_events([UiPatchEvent {
        patch_id: "simoder.base.mods-modal".into(),
        action: "open_manual_modal".into(),
        target_id: Some("manual_update_simoder".into()),
        value: None,
    }]);
    host.tick_ui().expect("tick after open_manual_modal should succeed");

    let (_rev2, patches2) = runtime.composed_ui_snapshot();
    let modal_patch2 = patches2.iter().find(|p| p.patch_id == "simoder.base.mods-modal");
    assert!(modal_patch2.is_some());
    let node_json = serde_json::to_string(&modal_patch2.unwrap().node).unwrap();
    assert!(node_json.contains("manual_update_modal") || node_json.contains("Manual Update Required"), "manual update dialog must be presented");
}
