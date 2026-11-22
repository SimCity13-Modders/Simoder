/**
 * Generic EAWebKit declarative DOM patch applicator (strict ES5).
 * Receives JSON packets from Simoder-Base via the verified native EAWebKit bridge:
 * - {"op":"observe"} -> probes view readiness and current logical phase/locale without mutating DOM.
 * - {"op":"sync","revision":N,"patches":[...]} -> reconciles declarative patches into view slots and drains click/toggle events.
 */
(function (packetText) {
    "use strict";
    try {
        var packet = JSON.parse(packetText);
        if (!window || !window.document) {
            return JSON.stringify({ ok: false, error: "DOM document unavailable" });
        }
        var doc = window.document;
        var sim = window.simcity || null;
        var client = window.Client || null;
        var uiMgr = (window.scrui && window.scrui.gUIManager) ? window.scrui.gUIManager : null;

        var detectLocale = function () {
            if (uiMgr && uiMgr.mLocaleManager && typeof uiMgr.mLocaleManager.mLocaleCode === "string" && uiMgr.mLocaleManager.mLocaleCode.length > 0) {
                return uiMgr.mLocaleManager.mLocaleCode;
            }
            return "en-us";
        };

        var detectPhase = function () {
            if (typeof window.__simoderMockPhase === "string" && window.__simoderMockPhase.length > 0) {
                return window.__simoderMockPhase;
            }
            if (sim && sim.gUpdater && typeof (client && client.RequestUpdaterData) === "function") {
                return "launcher.menu";
            }
            if (sim && sim.gCurtainUIHandler && sim.gCurtainUIHandler.mMoviePlaying === true) {
                return "intro";
            }
            if (client && typeof client.RequestGameData === "function") {
                try {
                    var fin = client.RequestGameData("isLoadingFinalized");
                    if (typeof fin === "string") { fin = JSON.parse(fin); }
                    if (fin && fin.json !== undefined) { fin = fin.json; }
                    if (fin === true) {
                        var boxId = client.RequestGameData("activeBoxId");
                        if (typeof boxId === "string") { boxId = JSON.parse(boxId); }
                        if (boxId && boxId.json !== undefined) { boxId = boxId.json; }
                        if (boxId !== undefined && boxId !== null && String(boxId) !== "0") {
                            return "ingame.options";
                        }
                        return "main.menu";
                    }
                } catch (_err) {
                    /* ignore transient query error before preload finishes */
                }
            }
            if (sim && sim.cLauncher3D) {
                return sim.cLauncher3D.mPreloadDone === true ? "main.menu" : "loading";
            }
            return "cold";
        };

        var phase = detectPhase();
        var activeViews = [];
        if (phase === "launcher.menu" || phase === "main.menu" || phase === "ingame.options") {
            activeViews.push(phase);
        }
        var observation = {
            phase: phase,
            activeViews: activeViews,
            locale: detectLocale()
        };

        if (packet.op === "observe") {
            return JSON.stringify({
                ok: !!doc.body,
                observation: observation,
                events: []
            });
        }

        if (!doc.body) {
            return JSON.stringify({ ok: false, error: "document.body unavailable" });
        }

        if (!window.__simoderUiHost) {
            window.__simoderUiHost = {
                revision: -1,
                reloadGen: -1,
                lastPhase: "",
                events: [],
                activeModalClose: null,
                activeRegionContext: null,
                escBound: false
            };
        }
        var host = window.__simoderUiHost;

        if (!host.escBound && doc.addEventListener) {
            host.escBound = true;
            doc.addEventListener("keydown", function (e) {
                var code = e ? (e.keyCode || e.which) : 0;
                if (code === 27 && host.activeModalClose) {
                    pushEvent(host.activeModalClose.patchId, host.activeModalClose.action, "modal_escape_close", null);
                }
            }, false);
        }

        var pushEvent = function (patchId, action, targetId, value) {
            if (!action || host.events.length >= 64) { return; }
            host.events.push({
                patchId: String(patchId),
                action: String(action),
                targetId: targetId ? String(targetId) : null,
                value: value !== undefined && value !== null ? String(value) : null
            });
        };

        var ensureStyles = function () {
            var cssText =
                ".simoder-slot-launcher{position:fixed;left:336px;bottom:8px;z-index:99980;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;}" +
                ".simoder-slot-hub{position:fixed;left:44px;bottom:84px;z-index:99980;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;display:none;}" +
                ".simoder-slot-region{position:fixed;z-index:99982;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;display:none;}" +
                ".simoder-region-tile-btn{position:absolute;right:6px;bottom:6px;z-index:25;padding:3px 9px;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;font-size:11px;font-weight:bold;text-transform:uppercase;letter-spacing:0.3px;border-radius:11px;border:1px solid #8ea3b8;background:#eef3f8;background-image:-webkit-gradient(linear,left top,left bottom,from(#ffffff),color-stop(0.55,#e5edf5),to(#cad7e5));color:#1c2f42;cursor:pointer;box-shadow:0 2px 5px rgba(8,18,30,0.55);}" +
                ".simoder-region-tile-btn:hover{border-color:#2ca7e6;background:#ffffff;}" +
                ".simoder-slot-ingame{position:fixed;z-index:99980;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;display:none;}" +
                ".simoder-modal-slot{position:fixed;top:0;left:0;right:0;bottom:0;z-index:99990;background:rgba(16,28,42,0.58);font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;overflow:hidden;}" +
                ".simoder-modal{position:fixed;top:36px;bottom:36px;left:50%;margin-left:-330px;width:660px;max-width:92%;text-align:left;background:#edf2f7;background-image:-webkit-gradient(linear,left top,left bottom,from(#f8fbfe),to(#e5edf5));border:2px solid #9db1c4;-webkit-border-image:none;-webkit-border-radius:4px;border-radius:4px;color:#1d2e3d;box-shadow:0 10px 30px rgba(8,18,30,0.55);overflow:hidden;box-sizing:border-box;}" +
                ".simoder-modal-header{position:absolute;top:0;left:0;right:0;height:42px;line-height:42px;background:#3c5268;background-image:-webkit-gradient(linear,left top,left bottom,from(#516a82),to(#334659));color:#ffffff;padding:0 44px 0 16px;font-size:15px;font-weight:bold;letter-spacing:0.4px;border-bottom:2px solid #2ca7e6;-webkit-border-image:none;-webkit-border-radius:0;border-radius:0;text-shadow:0 1px 1px rgba(0,0,0,0.45);box-sizing:border-box;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;}" +
                ".simoder-modal-header .simoder-modal-title{position:static;display:inline;width:auto;height:auto;text-align:left;color:#ffffff;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;font-size:15px;font-weight:bold;line-height:42px;}" +
                ".simoder-modal-close{-webkit-appearance:none;position:absolute;right:8px;top:7px;width:26px;height:26px;line-height:24px;text-align:center;background:rgba(255,255,255,0.14);border:1px solid rgba(255,255,255,0.35);-webkit-border-image:none;-webkit-border-radius:3px;border-radius:3px;color:#ffffff;font-size:16px;font-weight:bold;cursor:pointer;padding:0;outline:none;}" +
                ".simoder-modal-close:hover{background:#d9483b;border-color:#a82b20;}" +
                ".simoder-modal-body{position:absolute;top:42px;bottom:52px;left:0;right:0;overflow-y:auto;padding:12px 16px;box-sizing:border-box;}" +
                ".simoder-modal-body-nofooter{bottom:0;}" +
                ".simoder-modal-footer{position:absolute;bottom:0;left:0;right:0;height:52px;padding:10px 16px;box-sizing:border-box;background:#dfe8f2;background-image:-webkit-gradient(linear,left top,left bottom,from(#e8eff7),to(#d5e0ec));border-top:1px solid #b8c8d8;-webkit-border-image:none;-webkit-border-radius:0;border-radius:0;text-align:right;}" +
                ".simoder-modal-footer .simoder-row{margin:0;}" +
                ".simoder-modal-footer .simoder-row>*{margin-right:0;margin-left:8px;}" +
                ".simoder-panel{position:static;background:#ffffff;border:1px solid #c3d1df;-webkit-border-image:none;-webkit-border-radius:3px;border-radius:3px;padding:10px 12px;margin:8px 0;box-shadow:inset 0 1px 0 rgba(255,255,255,0.9),0 1px 2px rgba(25,45,68,0.08);}" +
                ".simoder-panel-scroll{max-height:210px;overflow-y:auto;background:#f7fafd;}" +
                ".simoder-row{margin:6px 0;}" +
                ".simoder-row>*{display:inline-block;vertical-align:middle;margin-right:8px;}" +
                ".simoder-text{position:static;width:auto;height:auto;color:#233547;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;font-size:12px;line-height:1.45;text-align:left;}" +
                ".simoder-text-muted{position:static;width:auto;height:auto;color:#556b80;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;font-size:11px;word-break:break-all;text-align:left;}" +
                ".simoder-text-title{position:static;width:auto;height:auto;color:#1c3144;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;font-size:13px;font-weight:bold;margin-bottom:3px;text-align:left;}" +
                ".simoder-text-title.Header_12_Center_Upper{position:static;width:auto;height:auto;text-align:left;color:#1c3144;}" +
                ".simoder-btn,.simoder-btn.cButton{-webkit-appearance:none;position:relative;display:inline-block;width:auto;height:auto;vertical-align:middle;background:#dce5ef;background-image:-webkit-gradient(linear,left top,left bottom,from(#fbfdff),color-stop(0.5,#e2eaf3),to(#cad7e5));color:#1c2f42;border:1px solid #8ea3b8;-webkit-border-image:none;-webkit-border-radius:3px;border-radius:3px;padding:5px 13px;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;font-size:12px;font-weight:bold;cursor:pointer;outline:none;text-shadow:0 1px 0 rgba(255,255,255,0.85);box-shadow:inset 0 1px 0 rgba(255,255,255,0.9),0 1px 1px rgba(0,0,0,0.12);}" +
                ".simoder-btn:hover,.simoder-btn.cButtonRollover{background:#e9f4fd;background-image:-webkit-gradient(linear,left top,left bottom,from(#ffffff),color-stop(0.5,#e5f2fc),to(#cbe2f6));border-color:#2ca7e6;}" +
                ".simoder-btn.cButtonDown{background:#c5d5e6;background-image:-webkit-gradient(linear,left top,left bottom,from(#cad8e6),to(#e7eff7));}" +
                ".simoder-btn>span,.simoder-btn-label{position:static;display:inline-block;width:auto;height:auto;vertical-align:middle;}" +
                ".simoder-btn .simoder-btn-label,.simoder-tab-btn .simoder-btn-label{color:inherit !important;background:transparent;border:0;text-shadow:none;}" +
                ".simoder-slot-launcher .simoder-btn{padding:3px 10px;font-size:11px;line-height:16px;}" +
                ".simoder-slot-hub .simoder-btn{display:block;width:100%;height:100%;padding:0 16px;font-size:16px;font-weight:bold;line-height:1;text-transform:uppercase;letter-spacing:0.6px;text-align:center;color:#243b53;border:1px solid #a8b7c6;border-radius:999px;background:#eef3f8;background-image:-webkit-gradient(linear,left top,left bottom,from(#ffffff),color-stop(0.48,#f5f8fb),to(#d4dee8));text-shadow:0 1px 0 rgba(255,255,255,0.92);box-shadow:inset 0 1px 0 rgba(255,255,255,0.95),0 2px 5px rgba(10,22,36,0.42);box-sizing:border-box;}" +
                ".simoder-slot-hub .simoder-btn:hover{background:#f5faff;background-image:-webkit-gradient(linear,left top,left bottom,from(#ffffff),color-stop(0.48,#ebf4fd),to(#c6dcf2));border-color:#2ca7e6;}" +
                ".simoder-slot-hub .simoder-btn-vanilla{background:transparent!important;background-image:none!important;border:0!important;box-shadow:none!important;padding:0!important;margin:0!important;border-radius:0!important;overflow:visible!important;position:relative!important;}" +
                ".simoder-slot-hub .simoder-btn-vanilla>.simoder-badge{position:absolute;top:2px;right:12px;z-index:2;}" +
                ".simoder-slot-region .simoder-btn{padding:5px 12px;font-size:11px;font-weight:bold;text-transform:uppercase;letter-spacing:0.4px;border-radius:14px;box-shadow:0 2px 8px rgba(8,18,30,0.45);}" +
                ".simoder-slot-ingame .simoder-btn{display:block;width:100%;height:100%;padding:0 14px;font-size:13px;font-weight:bold;text-transform:uppercase;letter-spacing:0.5px;text-align:center;border-radius:14px;box-sizing:border-box;}" +
                ".simoder-btn-primary,.simoder-btn-primary.cButton{background:#3aa526;background-image:-webkit-gradient(linear,left top,left bottom,from(#63d149),color-stop(0.55,#39a323),to(#2b8918));border-color:#237213;color:#ffffff;text-shadow:0 -1px 0 rgba(0,0,0,0.3);box-shadow:inset 0 1px 0 rgba(255,255,255,0.35),0 1px 1px rgba(0,0,0,0.15);}" +
                ".simoder-btn-primary:hover,.simoder-btn-primary.cButtonRollover{background:#46b831;background-image:-webkit-gradient(linear,left top,left bottom,from(#72dd58),color-stop(0.55,#42b32b),to(#33991e));border-color:#1c5f0f;}" +
                ".simoder-btn-danger,.simoder-btn-danger.cButton{background:#c93d32;background-image:-webkit-gradient(linear,left top,left bottom,from(#e35d52),color-stop(0.55,#c7382d),to(#a6281e));border-color:#841e16;color:#ffffff;text-shadow:0 -1px 0 rgba(0,0,0,0.3);box-shadow:inset 0 1px 0 rgba(255,255,255,0.28),0 1px 1px rgba(0,0,0,0.15);}" +
                ".simoder-btn-danger:hover,.simoder-btn-danger.cButtonRollover{background:#d8493d;background-image:-webkit-gradient(linear,left top,left bottom,from(#ed6d62),color-stop(0.55,#d34338),to(#b32f24));border-color:#6f1811;}" +
                ".simoder-btn-warning,.simoder-btn-warning.cButton{border-color:#d9821e;box-shadow:inset 0 1px 0 rgba(255,255,255,0.9),0 0 0 1px rgba(217,130,30,0.35);}" +
                ".simoder-modal .simoder-btn-warning,.simoder-modal .simoder-btn-warning.cButton{background:#df7e18;background-image:-webkit-gradient(linear,left top,left bottom,from(#f6a33e),color-stop(0.55,#e07e19),to(#c2630b));border-color:#984d08;color:#ffffff;text-shadow:0 -1px 0 rgba(0,0,0,0.3);box-shadow:inset 0 1px 0 rgba(255,255,255,0.32),0 1px 1px rgba(0,0,0,0.15);}" +
                ".simoder-modal .simoder-btn-warning:hover,.simoder-modal .simoder-btn-warning.cButtonRollover{background:#e88b26;background-image:-webkit-gradient(linear,left top,left bottom,from(#fab155),color-stop(0.55,#ea8c27),to(#d16e11));border-color:#804006;}" +
                ".simoder-btn:disabled{opacity:0.45;cursor:default;}" +
                ".simoder-badge{background:#d94235;color:#ffffff;border-radius:9px;padding:1px 6px;margin-left:6px;font-size:10px;font-weight:bold;line-height:13px;}" +
                ".simoder-badge-ok{background:#2d9620;}" +
                ".simoder-badge-muted{background:#5f7489;}" +
                ".simoder-divider{position:static;width:100%;height:1px;border:0;border-top:1px solid #d0dce8;margin:8px 0;}" +
                ".simoder-field{margin:6px 0;}" +
                ".simoder-field-label{position:static;display:block;width:auto;height:auto;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;font-size:11px;font-weight:bold;color:#30465b;margin-bottom:3px;text-align:left;}" +
                ".simoder-input,.simoder-select{position:static;width:100%;box-sizing:border-box;padding:5px 8px;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;font-size:12px;color:#1d2e3d;background:#ffffff;border:1px solid #9ab0c4;border-radius:3px;box-shadow:inset 0 1px 2px rgba(15,30,45,0.08);}" +
                ".simoder-input:focus,.simoder-select:focus{outline:none;border-color:#2ca7e6;}" +
                ".simoder-slider{width:100%;vertical-align:middle;cursor:pointer;}" +
                ".simoder-checkbox-wrap{display:inline-block;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;font-size:12px;color:#1d2e3d;cursor:pointer;margin:4px 0;}" +
                ".simoder-checkbox-wrap input{vertical-align:middle;margin-right:6px;}" +
                ".simoder-tabs{margin:4px 0;}" +
                ".simoder-tab-bar{position:static;width:auto;height:auto;background:transparent;border:0;-webkit-border-image:none;-webkit-border-radius:0;border-radius:0;border-bottom:2px solid #8ea3b8;margin-bottom:10px;padding:0 2px;}" +
                ".simoder-tab-btn{-webkit-appearance:none;position:relative;display:inline-block;vertical-align:bottom;width:auto;height:30px;line-height:28px;box-sizing:border-box;background:#d6e1ec;background-image:-webkit-gradient(linear,left top,left bottom,from(#e7eff7),to(#cbd7e5));color:#22364a;border:1px solid #96abc0;border-bottom:0;-webkit-border-image:none;-webkit-border-top-left-radius:4px;-webkit-border-top-right-radius:4px;-webkit-border-bottom-left-radius:0;-webkit-border-bottom-right-radius:0;border-radius:4px 4px 0 0;padding:0 14px;margin:0 4px 0 0;font-family:EAAGSimCityStdBk,Arial,Helvetica,sans-serif;font-size:12px;font-weight:bold;cursor:pointer;outline:none;}" +
                ".simoder-tab-btn:hover{background:#e9f2fb;color:#132536;border-color:#5ea8d6;}" +
                ".simoder-tab-btn-active{height:30px;line-height:27px;background:#ffffff;background-image:none;color:#132536;border:1px solid #2ca7e6;border-top:2px solid #2ca7e6;border-bottom:2px solid #ffffff;-webkit-border-image:none;-webkit-border-top-left-radius:4px;-webkit-border-top-right-radius:4px;-webkit-border-bottom-left-radius:0;-webkit-border-bottom-right-radius:0;border-radius:4px 4px 0 0;margin-bottom:-2px;}" +
                ".simoder-progress-track{position:static;width:100%;height:14px;background:#d3dde7;border:1px solid #9cb0c4;border-radius:3px;overflow:hidden;box-sizing:border-box;}" +
                ".simoder-progress-fill{height:100%;background:#48bf34;background-image:-webkit-gradient(linear,left top,left bottom,from(#72df5c),to(#369f22));}";
            var existingStyle = doc.getElementById("simoder-ui-styles");
            if (existingStyle) {
                if (existingStyle.textContent !== cssText) {
                    existingStyle.textContent = cssText;
                }
                return;
            }
            var style = doc.createElement("style");
            style.id = "simoder-ui-styles";
            style.textContent = cssText;
            (doc.head || doc.body).appendChild(style);
        };

        var slotClassFor = function (viewName, slotName) {
            if (slotName === "modal.overlay") { return "simoder-modal-slot Progress_Back"; }
            if (slotName === "region.actions") { return "simoder-slot-region"; }
            if (viewName === "launcher.menu") { return "simoder-slot-launcher"; }
            if (viewName === "main.menu") { return "simoder-slot-hub"; }
            return "simoder-slot-ingame";
        };

        var isSimoderNode = function (el) {
            var cur = el;
            while (cur && cur.getAttribute) {
                if (cur.getAttribute("data-simoder-slot") || (cur.id && cur.id.indexOf("simoder-") === 0)) {
                    return true;
                }
                cur = cur.parentNode;
            }
            return false;
        };

        var findLauncherFooterAnchor = function () {
            if (!doc.getElementsByTagName) { return null; }
            var viewH = window.innerHeight || (doc.documentElement && doc.documentElement.clientHeight) || 720;
            var viewW = window.innerWidth || (doc.documentElement && doc.documentElement.clientWidth) || 1280;
            var tags = ["a", "button", "div", "span"];
            var bestRect = null;
            var t, i, els, el, rect, txt, lower;
            for (t = 0; t < tags.length; t += 1) {
                els = doc.getElementsByTagName(tags[t]);
                for (i = 0; i < els.length; i += 1) {
                    el = els[i];
                    if (isSimoderNode(el) || typeof el.getBoundingClientRect !== "function") { continue; }
                    rect = el.getBoundingClientRect();
                    if (!rect || rect.width < 40 || rect.width > 260 || rect.height < 10 || rect.height > 44) { continue; }
                    if (rect.top < viewH * 0.84 || rect.left < viewW * 0.14 || rect.right > viewW * 0.55) { continue; }
                    txt = el.textContent ? String(el.textContent).replace(/^\s+|\s+$/g, "") : "";
                    if (txt.length === 0 || txt.length > 48 || txt.indexOf("2013") !== -1) { continue; }
                    lower = txt.toLowerCase();
                    if (lower.indexOf("update") !== -1 || lower.indexOf("aktualiz") !== -1 || el.tagName === "A") {
                        return { rect: rect, viewW: viewW, viewH: viewH };
                    }
                    if (!bestRect || rect.top > bestRect.top) {
                        bestRect = rect;
                    }
                }
            }
            return bestRect ? { rect: bestRect, viewW: viewW, viewH: viewH } : null;
        };

        var findMainMenuHubColumn = function () {
            var viewH = window.innerHeight || (doc.documentElement && doc.documentElement.clientHeight) || 720;
            var viewW = window.innerWidth || (doc.documentElement && doc.documentElement.clientWidth) || 1280;
            var mgr = window.gUIManager || uiMgr;
            var isCtrlVisible = function (ctrl) {
                if (!ctrl) { return false; }
                if (typeof ctrl.GetAbsoluteVisibility === "function") {
                    return !!ctrl.GetAbsoluteVisibility();
                }
                return ctrl.mIsVisible !== false;
            };
            var containsCtrl = function (root, target) {
                if (!root || !target) { return false; }
                if (typeof root.ContainsControl === "function") {
                    return !!root.ContainsControl(target);
                }
                var cur = target;
                while (cur) {
                    if (cur === root) { return true; }
                    cur = cur.mParentControl;
                }
                return false;
            };
            var readPlayTypography = function (ctrl) {
                if (!ctrl || !ctrl.mRootElement || typeof window.getComputedStyle !== "function") { return null; }
                var textEl = ctrl.mTextElement || null;
                if (!textEl && ctrl.mRootElement.getElementsByTagName) {
                    var kids = ctrl.mRootElement.getElementsByTagName("*");
                    var ki;
                    for (ki = 0; ki < kids.length; ki += 1) {
                        if (kids[ki] && kids[ki].textContent && String(kids[ki].textContent).replace(/^\s+|\s+$/g, "").length > 0) {
                            textEl = kids[ki];
                            break;
                        }
                    }
                }
                if (!textEl) { textEl = ctrl.mRootElement; }
                try {
                    var cs = window.getComputedStyle(textEl, null);
                    if (!cs) { return null; }
                    return {
                        fontFamily: cs.fontFamily || "",
                        fontSize: cs.fontSize || "",
                        fontWeight: cs.fontWeight || "",
                        color: cs.color || ""
                    };
                } catch (_e) {
                    return null;
                }
            };

            var getLocalPos = function (ctrl) {
                if (!ctrl) { return [0, 0]; }
                if (typeof ctrl.GetAbsolutePosition === "function") {
                    try {
                        var p = ctrl.GetAbsolutePosition();
                        if (p && p.length >= 2 && isFinite(p[0]) && isFinite(p[1])) {
                            return [p[0], p[1]];
                        }
                    } catch (_e) {}
                }
                return [Number(ctrl.mLeft || 0), Number(ctrl.mTop || 0)];
            };

            if (mgr && typeof mgr.FindControlByID === "function") {
                var playCtrl = mgr.FindControlByID(237051648);
                if (playCtrl && isCtrlVisible(playCtrl) && playCtrl.mRootElement &&
                    typeof playCtrl.mWidth === "number" && typeof playCtrl.mHeight === "number" &&
                    playCtrl.mWidth >= 100 && playCtrl.mHeight >= 20 &&
                    typeof playCtrl.mRootElement.getBoundingClientRect === "function") {
                    var playRect = playCtrl.mRootElement.getBoundingClientRect();
                    if (playRect && playRect.width >= 100 && playRect.height >= 20 &&
                        playRect.left >= 0 && playRect.top >= 0 && playRect.right <= viewW && playRect.bottom <= viewH) {
                        var playPos = getLocalPos(playCtrl);
                        var resumeCtrl = mgr.FindControlByID(237051575);
                        var resumePos = null;
                        var localPitch = Math.round(playCtrl.mHeight + 8);
                        if (resumeCtrl && isCtrlVisible(resumeCtrl) && resumeCtrl.mRootElement && typeof resumeCtrl.mHeight === "number") {
                            resumePos = getLocalPos(resumeCtrl);
                            if (playPos[1] > resumePos[1]) {
                                localPitch = Math.max(24, Math.min(140, Math.round(playPos[1] - resumePos[1])));
                            }
                        }
                        var hubCtrl = mgr.FindControlByID(241896001);
                        var walkRoot = hubCtrl || playCtrl.mParentControl;
                        var panels = [];
                        var playCenterX = playPos[0] + playCtrl.mWidth * 0.5;
                        var seenCtrls = [];
                        var alreadySeen = function (c) {
                            var si;
                            for (si = 0; si < seenCtrls.length; si += 1) {
                                if (seenCtrls[si] === c) { return true; }
                            }
                            return false;
                        };
                        var collectPanels = function (ctrl) {
                            if (!ctrl || !isCtrlVisible(ctrl) || alreadySeen(ctrl)) { return; }
                            seenCtrls.push(ctrl);
                            if (ctrl === playCtrl || ctrl === resumeCtrl || containsCtrl(playCtrl, ctrl) || (resumeCtrl && containsCtrl(resumeCtrl, ctrl))) {
                                return;
                            }
                            if (ctrl.mRootElement && typeof ctrl.mWidth === "number" && typeof ctrl.mHeight === "number" && ctrl.mWidth > 0 && ctrl.mHeight > 0) {
                                var cPos = getLocalPos(ctrl);
                                var ctrlCenterX = cPos[0] + ctrl.mWidth * 0.5;
                                if (Math.abs(ctrlCenterX - playCenterX) <= 40 &&
                                    ctrl.mWidth >= playCtrl.mWidth * 0.75 && ctrl.mWidth <= playCtrl.mWidth + 140 &&
                                    cPos[1] <= playPos[1] + 20 && cPos[1] + ctrl.mHeight >= playPos[1] + playCtrl.mHeight - 20 &&
                                    ctrl.mHeight >= playCtrl.mHeight * 1.4 && ctrl.mHeight <= playCtrl.mHeight * 6) {
                                    panels.push(ctrl);
                                }
                            }
                            if (ctrl.mChildren && ctrl.mChildren.length) {
                                var ci;
                                for (ci = 0; ci < ctrl.mChildren.length; ci += 1) {
                                    collectPanels(ctrl.mChildren[ci]);
                                }
                            }
                        };
                        collectPanels(walkRoot);
                        if (playCtrl.mParentControl && playCtrl.mParentControl !== walkRoot) {
                            collectPanels(playCtrl.mParentControl);
                        }
                        return {
                            rect: playRect,
                            playPos: playPos,
                            resumePos: resumePos,
                            localPitch: localPitch,
                            typography: readPlayTypography(playCtrl),
                            playCtrl: playCtrl,
                            resumeCtrl: resumeCtrl,
                            panels: panels,
                            viewW: viewW,
                            viewH: viewH
                        };
                    }
                }
            }

            if (!doc.getElementsByTagName) { return null; }
            var all = doc.getElementsByTagName("*");
            var bestRect = null;
            var i, el, bg, src, rect;
            for (i = 0; i < all.length; i += 1) {
                el = all[i];
                if (isSimoderNode(el) || typeof el.getBoundingClientRect !== "function") { continue; }
                bg = (el.style && el.style.backgroundImage) ? String(el.style.backgroundImage) : "";
                if (!bg && typeof window.getComputedStyle === "function") {
                    try { bg = String(window.getComputedStyle(el, null).backgroundImage || ""); } catch (_e) {}
                }
                src = el.src ? String(el.src) : "";
                if (bg.indexOf("Button_HUB_Main") === -1 && src.indexOf("Button_HUB_Main") === -1) {
                    continue;
                }
                rect = el.getBoundingClientRect();
                if (!rect || rect.width < 140 || rect.height < 20 || rect.top < 0 || rect.bottom > viewH - 20 || rect.left > viewW * 0.5) {
                    continue;
                }
                if (!bestRect || rect.bottom > bestRect.bottom) {
                    bestRect = rect;
                }
            }
            return bestRect ? { rect: bestRect, playPos: null, resumePos: null, localPitch: Math.round(bestRect.height + 8), typography: null, playCtrl: null, resumeCtrl: null, panels: [], viewW: viewW, viewH: viewH } : null;
        };

        var removeVanillaHubButton = function () {
            var existing = doc.getElementById("simoder-hub-vanilla-mods");
            if (existing && existing.parentNode) {
                existing.parentNode.removeChild(existing);
            }
        };

        var removeVanillaEscButton = function () {
            var existing = doc.getElementById("simoder-esc-vanilla-mods");
            if (existing && existing.parentNode) {
                existing.parentNode.removeChild(existing);
            }
        };

        var extractTriggerLabelAndBadge = function (btn, defaultLabel) {
            var labelText = "";
            var badgeText = "";
            var ci, ch;
            for (ci = 0; ci < btn.childNodes.length; ci += 1) {
                ch = btn.childNodes[ci];
                if (ch.nodeType === 1 && ch.className && String(ch.className).indexOf("simoder-badge") !== -1) {
                    badgeText = ch.textContent ? String(ch.textContent) : "";
                } else if (ch.textContent) {
                    labelText += String(ch.textContent);
                }
            }
            labelText = labelText.replace(/^\s+|\s+$/g, "") || defaultLabel;
            return { label: labelText, badge: badgeText };
        };

        var populateVanillaCloneContent = function (clone, labelText, badgeText) {
            var stripIds = function (node) {
                if (!node || node.nodeType !== 1) { return; }
                if (node.removeAttribute) { node.removeAttribute("id"); }
                var k;
                for (k = 0; k < node.childNodes.length; k += 1) {
                    stripIds(node.childNodes[k]);
                }
            };
            stripIds(clone);

            var replacedLabel = false;
            var replaceText = function (node) {
                if (!node) { return; }
                if (node.nodeType === 3) {
                    var trimmed = String(node.nodeValue || "").replace(/^\s+|\s+$/g, "");
                    if (trimmed.length > 0) {
                        node.nodeValue = replacedLabel ? "" : labelText;
                        replacedLabel = true;
                    }
                    return;
                }
                var k;
                for (k = 0; k < node.childNodes.length; k += 1) {
                    replaceText(node.childNodes[k]);
                }
            };
            replaceText(clone);

            if (badgeText) {
                var badgeEl = doc.createElement("span");
                badgeEl.className = "simoder-badge";
                badgeEl.textContent = badgeText;
                if (badgeEl.style) {
                    badgeEl.style.position = "absolute";
                    badgeEl.style.top = "2px";
                    badgeEl.style.right = "12px";
                    badgeEl.style.zIndex = "5";
                }
                clone.appendChild(badgeEl);
            }
        };

        var bindVanillaButtonClone = function (clone, templateCtrl, triggerBtn) {
            var dType = templateCtrl.mDrawableType;
            var styles = templateCtrl.mStyleList;
            var stateClasses = ["cButtonNormal", "cButtonRollover", "cButtonDown", "cButtonDisabled"];
            var applyState = function (stateIndex) {
                var stCls = stateClasses[stateIndex] || "cButtonNormal";
                if (dType !== 1 && clone.className) {
                    var clsStr = String(clone.className).replace(/\bcButton(Normal|Rollover|Down|Disabled)\b/g, stCls);
                    if (clsStr.indexOf(stCls) === -1) {
                        clsStr += " " + stCls;
                    }
                    clone.className = clsStr;
                }
                if (!styles || styles.length <= stateIndex || !styles[stateIndex]) { return; }
                var st = String(styles[stateIndex]);
                if (dType === 1) {
                    clone.className = st;
                    return;
                }
                var k, chNode, cls, spaceIdx;
                var updatedImg = false;
                for (k = 0; k < clone.childNodes.length; k += 1) {
                    chNode = clone.childNodes[k];
                    if (!chNode || chNode.nodeType !== 1) { continue; }
                    if (chNode.tagName === "IMG") {
                        chNode.src = st;
                        updatedImg = true;
                    } else if (dType === 2 && chNode.className) {
                        cls = String(chNode.className);
                        if (cls.indexOf("pin") === 0) {
                            spaceIdx = cls.indexOf(" ");
                            chNode.className = (spaceIdx !== -1 ? cls.slice(0, spaceIdx) : cls) + " " + st;
                        }
                    }
                }
                if (!updatedImg && clone.getElementsByTagName && /\.(png|jpg|jpeg|dds)$/i.test(st)) {
                    var imgs = clone.getElementsByTagName("img");
                    if (imgs && imgs.length > 0) {
                        imgs[0].src = st;
                    }
                }
            };
            var playUiSound = function (soundId) {
                if (soundId && window.scrui && typeof window.scrui.Play2DSound === "function") {
                    try { window.scrui.Play2DSound(soundId); } catch (_e) {}
                }
            };
            applyState(clone.__simoderDown ? 2 : (clone.__simoderHover ? 1 : 0));
            clone.onmouseover = function () {
                if (!clone.__simoderHover) {
                    clone.__simoderHover = true;
                    playUiSound(templateCtrl.mAudioMouseOnID);
                }
                applyState(clone.__simoderDown ? 2 : 1);
            };
            clone.onmouseout = function (e) {
                var rel = e ? (e.relatedTarget || e.toElement) : null;
                if (rel && clone.contains && (rel === clone || clone.contains(rel))) {
                    return;
                }
                clone.__simoderHover = false;
                clone.__simoderDown = false;
                applyState(0);
            };
            clone.onmousedown = function () {
                clone.__simoderDown = true;
                playUiSound(templateCtrl.mAudioMouseDownID);
                applyState(2);
            };
            clone.onmouseup = function () {
                clone.__simoderDown = false;
                applyState(clone.__simoderHover ? 1 : 0);
            };
            clone.onclick = function () {
                if (triggerBtn && typeof triggerBtn.onclick === "function") {
                    triggerBtn.onclick();
                }
            };
        };

        var mountVanillaHubButton = function (container, hub) {
            if (!container || !hub || !hub.playCtrl || !hub.playCtrl.mRootElement || !hub.playCtrl.mRootElement.parentNode || !container.querySelector) {
                return false;
            }
            var btn = container.querySelector(".simoder-btn");
            if (!btn) { return false; }

            var lblInfo = extractTriggerLabelAndBadge(btn, "MODS");
            var labelText = lblInfo.label;
            var badgeText = lblInfo.badge;

            var parentEl = hub.playCtrl.mRootElement.parentNode;
            var clone = doc.getElementById("simoder-hub-vanilla-mods");
            if (!clone || clone.parentNode !== parentEl || clone.getAttribute("data-simoder-vanilla-label") !== labelText || clone.getAttribute("data-simoder-vanilla-badge") !== badgeText) {
                removeVanillaHubButton();
                clone = hub.playCtrl.mRootElement.cloneNode(true);
                populateVanillaCloneContent(clone, labelText, badgeText);
                clone.id = "simoder-hub-vanilla-mods";
                clone.setAttribute("data-simoder-slot", "hub.vanilla.clone");
                clone.setAttribute("data-simoder-vanilla-label", labelText);
                clone.setAttribute("data-simoder-vanilla-badge", badgeText);
                parentEl.appendChild(clone);
            }

            if (clone.style) {
                var playStyle = hub.playCtrl.mRootElement.style || {};
                var baseTop = parseFloat(playStyle.top);
                if (isNaN(baseTop)) { baseTop = Number(hub.playCtrl.mTop || 0); }
                clone.style.position = "absolute";
                clone.style.left = playStyle.left || (Math.round(hub.playCtrl.mLeft || 0) + "px");
                clone.style.top = Math.round(baseTop + hub.localPitch) + "px";
                clone.style.right = "auto";
                clone.style.bottom = "auto";
                clone.style.width = playStyle.width || (Math.round(hub.playCtrl.mWidth || 0) + "px");
                clone.style.height = playStyle.height || (Math.round(hub.playCtrl.mHeight || 0) + "px");
                clone.style.cursor = "pointer";
                clone.style.pointerEvents = "auto";
                clone.style.visibility = "visible";
                clone.style.opacity = "1";
                if (!clone.style.display || clone.style.display === "none") {
                    clone.style.display = "inline";
                }
            }

            bindVanillaButtonClone(clone, hub.playCtrl, btn);

            container.style.display = "none";
            return true;
        };

        var resolveMyGamesTab = function (myGames, propName, tabKey) {
            if (!myGames) { return null; }
            if (myGames[propName]) { return myGames[propName]; }
            if (typeof myGames.GetTab === "function") {
                try {
                    var t = myGames.GetTab(tabKey);
                    if (t) { return t; }
                } catch (_e) {}
            }
            if (myGames.mTabs && myGames.mTabs[tabKey]) {
                return myGames.mTabs[tabKey];
            }
            return null;
        };

        var findActiveRegionPreview = function () {
            if (!sim) { return null; }
            var viewH = window.innerHeight || (doc.documentElement && doc.documentElement.clientHeight) || 720;
            var viewW = window.innerWidth || (doc.documentElement && doc.documentElement.clientWidth) || 1280;
            var myGames = sim.gMyGamesUIHandler || (sim.cLauncher3D && sim.cLauncher3D.mPanels && sim.cLauncher3D.mPanels.MY_GAMES_PANEL && sim.cLauncher3D.mPanels.MY_GAMES_PANEL.uiHandler);
            if (!myGames) { return null; }
            var mgr = window.gUIManager || uiMgr;
            var tabs = [
                resolveMyGamesTab(myGames, "mContinueTab", "CONTINUE_TAB"),
                resolveMyGamesTab(myGames, "mCreateTab", "CREATE_TAB")
            ];
            var i, tab, mapCtrl, reg, rootEl, rect;
            for (i = 0; i < tabs.length; i += 1) {
                tab = tabs[i];
                if (!tab || !tab.mOverviewMap || !tab.mOverviewMap.mRegionData) { continue; }
                mapCtrl = tab.mOverviewMap;
                reg = mapCtrl.mRegionData;
                if (!reg.id) { continue; }
                if (i === 0 && mgr && typeof mgr.FindControlByID === "function") {
                    var mapWin = mgr.FindControlByID(237389748);
                    if (mapWin && typeof mapWin.GetAbsoluteVisibility === "function" && !mapWin.GetAbsoluteVisibility()) {
                        continue;
                    }
                }
                rootEl = mapCtrl.mRootElement || (mapCtrl.mRootLayout && mapCtrl.mRootLayout.mRootElement) || (mapCtrl.mParentControl && mapCtrl.mParentControl.mRootElement);
                if (!rootEl && i === 0 && mgr && typeof mgr.FindControlByID === "function") {
                    var regMapCtrl = mgr.FindControlByID(237586561);
                    rootEl = regMapCtrl ? regMapCtrl.mRootElement : null;
                }
                if (!rootEl || typeof rootEl.getBoundingClientRect !== "function") { continue; }
                rect = rootEl.getBoundingClientRect();
                if (!rect || rect.width < 180 || rect.height < 120 || rect.right > viewW + 4 || rect.bottom > viewH + 4 || rect.left < 0 || rect.top < 0) {
                    continue;
                }
                return {
                    rect: rect,
                    regionId: String(reg.id),
                    regionName: String(reg.name || ""),
                    sandbox: !!(reg.sandbox === true || reg.sandbox === "true"),
                    viewW: viewW,
                    viewH: viewH
                };
            }
            return null;
        };

        var clearRegionTileButtons = function () {
            if (!doc.querySelectorAll) { return; }
            var stale = doc.querySelectorAll("[data-simoder-slot='region.tile.btn']");
            var i;
            for (i = 0; i < stale.length; i += 1) {
                if (stale[i].parentNode) {
                    stale[i].parentNode.removeChild(stale[i]);
                }
            }
        };

        var syncRegionTileButtons = function (container) {
            if (!sim || !container || !container.querySelector) {
                clearRegionTileButtons();
                return;
            }
            var mgr = window.gUIManager || uiMgr;
            var gridWin = (mgr && typeof mgr.FindControlByID === "function") ? mgr.FindControlByID(237389708) : null;
            var gridVisible = gridWin && (typeof gridWin.GetAbsoluteVisibility === "function" ? !!gridWin.GetAbsoluteVisibility() : gridWin.mIsVisible === true);
            var myGames = sim.gMyGamesUIHandler || (sim.cLauncher3D && sim.cLauncher3D.mPanels && sim.cLauncher3D.mPanels.MY_GAMES_PANEL && sim.cLauncher3D.mPanels.MY_GAMES_PANEL.uiHandler);
            var contTab = resolveMyGamesTab(myGames, "mContinueTab", "CONTINUE_TAB");
            var cacheRegs = (sim.cLauncher3D && sim.cLauncher3D.mDataCache && sim.cLauncher3D.mDataCache.mUserRegions && sim.cLauncher3D.mDataCache.mUserRegions.regions) ? sim.cLauncher3D.mDataCache.mUserRegions.regions : null;
            if (!gridVisible || !contTab || !contTab.mRegionList || !cacheRegs || !cacheRegs.length) {
                clearRegionTileButtons();
                return;
            }

            var triggerBtn = container.querySelector(".simoder-btn");
            var wrapper = container.querySelector("[data-simoder-patch]");
            var patchId = wrapper ? wrapper.getAttribute("data-simoder-patch") : "simoder.base.mods-trigger-region";
            var labelText = (triggerBtn && triggerBtn.textContent) ? String(triggerBtn.textContent).replace(/^\s+|\s+$/g, "") : "Mods";
            if (!labelText) { labelText = "Mods"; }

            var mountedIds = {};
            var idx, tile, reg, imgCtrl, hostEl, btnId, tileBtn, ctxPayload;
            for (idx = 0; idx < cacheRegs.length && idx < contTab.mRegionList.length; idx += 1) {
                tile = contTab.mRegionList[idx];
                reg = cacheRegs[idx];
                if (!tile || !reg || !reg.id || typeof tile.FindControlByID !== "function") { continue; }
                imgCtrl = tile.FindControlByID(236791249);
                hostEl = (imgCtrl && imgCtrl.mRootElement) ? imgCtrl.mRootElement : tile.mRootElement;
                if (!hostEl) { continue; }
                btnId = "simoder-region-tile-btn-" + String(reg.id);
                mountedIds[btnId] = true;
                tileBtn = doc.getElementById(btnId);
                if (!tileBtn || tileBtn.parentNode !== hostEl) {
                    if (tileBtn && tileBtn.parentNode) { tileBtn.parentNode.removeChild(tileBtn); }
                    tileBtn = doc.createElement("button");
                    tileBtn.type = "button";
                    tileBtn.id = btnId;
                    tileBtn.className = "simoder-region-tile-btn";
                    tileBtn.setAttribute("data-simoder-slot", "region.tile.btn");
                    hostEl.appendChild(tileBtn);
                }
                if (tileBtn.textContent !== labelText) {
                    tileBtn.textContent = labelText;
                }
                ctxPayload = JSON.stringify({
                    regionId: String(reg.id),
                    regionName: String(reg.name || ""),
                    sandbox: !!(reg.sandbox === true || reg.sandbox === "true")
                });
                (function (pId, payloadStr, btnRef) {
                    var stopEv = function (e) {
                        if (e && e.stopPropagation) { e.stopPropagation(); }
                        if (e && e.preventDefault) { e.preventDefault(); }
                        if (e) { e.cancelBubble = true; }
                    };
                    btnRef.onmousedown = stopEv;
                    btnRef.onmouseup = stopEv;
                    btnRef.onclick = function (e) {
                        stopEv(e);
                        pushEvent(pId, "open_region_mods", "mods_region_tile_btn", payloadStr);
                    };
                }(patchId, ctxPayload, tileBtn));
            }
            if (doc.querySelectorAll) {
                var allTileBtns = doc.querySelectorAll("[data-simoder-slot='region.tile.btn']");
                for (idx = 0; idx < allTileBtns.length; idx += 1) {
                    if (!mountedIds[allTileBtns[idx].id] && allTileBtns[idx].parentNode) {
                        allTileBtns[idx].parentNode.removeChild(allTileBtns[idx]);
                    }
                }
            }
        };

        var findInGameEscapeMenu = function () {
            if (!sim) { return null; }
            var mgr = window.gUIManager || uiMgr;
            if (!mgr || typeof mgr.FindControlByID !== "function") { return null; }
            var menuCtrl = mgr.FindControlByID(sim.kWinIDMainMenu || 185362957);
            var isCtrlVisible = function (ctrl) {
                if (!ctrl) { return false; }
                if (typeof ctrl.GetAbsoluteVisibility === "function") {
                    return !!ctrl.GetAbsoluteVisibility();
                }
                return ctrl.mIsVisible === true;
            };
            if (!isCtrlVisible(menuCtrl) || !menuCtrl.mRootElement) { return null; }
            if (typeof sim.IsOptionMenuVisible === "function" && !sim.IsOptionMenuVisible()) { return null; }

            var getCtrlAbsPos = function (ctrl) {
                if (!ctrl) { return [0, 0]; }
                if (typeof ctrl.GetAbsolutePosition === "function") {
                    try {
                        var p = ctrl.GetAbsolutePosition();
                        if (p && p.length >= 2 && isFinite(p[0]) && isFinite(p[1])) {
                            return [p[0], p[1]];
                        }
                    } catch (_e) {}
                }
                return [Number(ctrl.mLeft || 0), Number(ctrl.mTop || 0)];
            };

            var containsCtrl = function (root, target) {
                var cur = target;
                while (cur) {
                    if (cur === root) { return true; }
                    cur = cur.mParentControl;
                }
                return false;
            };

            var hasPanelStyle = function (ctrl) {
                if (!ctrl) { return false; }
                var cls = (ctrl.mRootElement && ctrl.mRootElement.className) ? String(ctrl.mRootElement.className) : "";
                if (cls.indexOf("Panel_flatFill_Bottom") !== -1) { return true; }
                if (ctrl.mStyleList && ctrl.mStyleList.length) {
                    var si;
                    for (si = 0; si < ctrl.mStyleList.length; si += 1) {
                        if (ctrl.mStyleList[si] && String(ctrl.mStyleList[si]).indexOf("Panel_flatFill_Bottom") !== -1) {
                            return true;
                        }
                    }
                }
                return false;
            };

            var panelCtrl = null;
            var mc = menuCtrl.mChildren || [];
            var i;
            for (i = 0; i < mc.length; i += 1) {
                if (isCtrlVisible(mc[i]) && mc[i].mRootElement && hasPanelStyle(mc[i]) &&
                    typeof mc[i].mWidth === "number" && typeof mc[i].mHeight === "number" &&
                    mc[i].mWidth >= 200 && mc[i].mWidth <= 800 && mc[i].mHeight >= 120) {
                    panelCtrl = mc[i];
                    break;
                }
            }
            if (!panelCtrl) { return null; }

            var buttons = [];
            var collectButtons = function (ctrl) {
                if (!ctrl || !isCtrlVisible(ctrl)) { return; }
                if (ctrl.mRootElement && !isSimoderNode(ctrl.mRootElement)) {
                    var cls = ctrl.mRootElement.className ? String(ctrl.mRootElement.className) : "";
                    if (cls.indexOf("cButton") !== -1 &&
                        typeof ctrl.mWidth === "number" && typeof ctrl.mHeight === "number" &&
                        ctrl.mWidth >= 120 && ctrl.mHeight >= 20 && ctrl.mHeight <= 64) {
                        buttons.push({ ctrl: ctrl, pos: getCtrlAbsPos(ctrl) });
                    }
                }
                if (ctrl.mChildren && ctrl.mChildren.length) {
                    var ci;
                    for (ci = 0; ci < ctrl.mChildren.length; ci += 1) {
                        collectButtons(ctrl.mChildren[ci]);
                    }
                }
            };
            collectButtons(panelCtrl);
            if (!buttons.length) { return null; }

            var settingsCtrl = mgr.FindControlByID(212665064);
            if (!settingsCtrl || !isCtrlVisible(settingsCtrl) || !settingsCtrl.mRootElement || !containsCtrl(panelCtrl, settingsCtrl)) {
                var topmost = buttons[0];
                for (i = 1; i < buttons.length; i += 1) {
                    if (buttons[i].pos[1] < topmost.pos[1]) {
                        topmost = buttons[i];
                    }
                }
                settingsCtrl = topmost.ctrl;
            }

            var topGroupCtrl = settingsCtrl.mParentControl || panelCtrl;
            var settingsPos = getCtrlAbsPos(settingsCtrl);
            var nextBelowY = null;
            for (i = 0; i < buttons.length; i += 1) {
                if (buttons[i].ctrl === settingsCtrl) { continue; }
                if (topGroupCtrl !== panelCtrl && !containsCtrl(topGroupCtrl, buttons[i].ctrl)) { continue; }
                if (Math.abs(buttons[i].pos[0] - settingsPos[0]) <= 8 && buttons[i].pos[1] > settingsPos[1] + 4) {
                    if (nextBelowY === null || buttons[i].pos[1] < nextBelowY) {
                        nextBelowY = buttons[i].pos[1];
                    }
                }
            }
            var pitch = Math.round(settingsCtrl.mHeight + 6);
            if (nextBelowY !== null) {
                var diff = Math.round(nextBelowY - settingsPos[1]);
                if (diff >= 28 && diff <= 64) {
                    pitch = diff;
                }
            }

            return {
                panelCtrl: panelCtrl,
                topGroupCtrl: topGroupCtrl,
                templateBtn: settingsCtrl,
                pitch: pitch,
                width: settingsCtrl.mWidth,
                height: settingsCtrl.mHeight
            };
        };

        var mountVanillaEscButton = function (container, esc) {
            if (!container || !esc || !esc.panelCtrl || !esc.panelCtrl.mRootElement || !esc.templateBtn || !esc.templateBtn.mRootElement || !container.querySelector) {
                removeVanillaEscButton();
                if (container && container.style) { container.style.display = "none"; }
                return false;
            }
            var btn = container.querySelector(".simoder-btn");
            if (!btn) {
                removeVanillaEscButton();
                container.style.display = "none";
                return false;
            }

            var setCtrlTop = function (ctrl, targetTop) {
                if (!ctrl) { return; }
                var curTop = Number(ctrl.mTop || 0);
                if (Math.abs(curTop - targetTop) <= 0.5) { return; }
                if (typeof ctrl.SetPosition === "function") {
                    ctrl.SetPosition(ctrl.mLeft, targetTop, ctrl.mWidth, ctrl.mHeight);
                } else if (typeof ctrl.SetTop === "function") {
                    ctrl.SetTop(targetTop);
                }
            };

            if (typeof esc.templateBtn.__simoderOrigTop !== "number") {
                esc.templateBtn.__simoderOrigTop = Number(esc.templateBtn.mTop || 0);
            }
            var settingsOrigTop = esc.templateBtn.__simoderOrigTop;
            var mountHostCtrl = esc.panelCtrl;
            var cloneLeft = Math.round((Number(esc.templateBtn.mLeft || 0)) * 10) / 10;
            var cloneTop = Math.round((settingsOrigTop + esc.pitch) * 10) / 10;

            if (esc.topGroupCtrl && esc.topGroupCtrl !== esc.panelCtrl && esc.topGroupCtrl.mRootElement) {
                mountHostCtrl = esc.topGroupCtrl;
                if (typeof esc.topGroupCtrl.__simoderOrigTop !== "number") {
                    esc.topGroupCtrl.__simoderOrigTop = Number(esc.topGroupCtrl.mTop || 0);
                }
                if (typeof esc.topGroupCtrl.__simoderOrigHeight !== "number") {
                    esc.topGroupCtrl.__simoderOrigHeight = Number(esc.topGroupCtrl.mHeight || 0);
                }
                var topGroupOrigTop = esc.topGroupCtrl.__simoderOrigTop;
                var topGroupOrigHeight = esc.topGroupCtrl.__simoderOrigHeight;

                var gKids = esc.topGroupCtrl.mChildren || [];
                var gi, gCh, gOrigTop;
                for (gi = 0; gi < gKids.length; gi += 1) {
                    gCh = gKids[gi];
                    if (!gCh || gCh === esc.templateBtn || (gCh.mRootElement && isSimoderNode(gCh.mRootElement))) { continue; }
                    if (typeof gCh.__simoderOrigTop !== "number") {
                        gCh.__simoderOrigTop = Number(gCh.mTop || 0);
                    }
                    gOrigTop = gCh.__simoderOrigTop;
                    if (gOrigTop > settingsOrigTop + 1) {
                        setCtrlTop(gCh, gOrigTop + esc.pitch);
                    }
                }

                var targetTopGroupHeight = Math.round(topGroupOrigHeight + esc.pitch);
                if (typeof esc.topGroupCtrl.SetHeight === "function" && typeof esc.topGroupCtrl.mHeight === "number" &&
                    Math.abs(esc.topGroupCtrl.mHeight - targetTopGroupHeight) > 1) {
                    esc.topGroupCtrl.SetHeight(targetTopGroupHeight);
                }

                var thresholdY = topGroupOrigTop + topGroupOrigHeight - 4;
                var pKids = esc.panelCtrl.mChildren || [];
                var maxPanelBottom = topGroupOrigTop + targetTopGroupHeight;
                var pi, pCh, pOrigTop, pTargetTop, pBottom;
                for (pi = 0; pi < pKids.length; pi += 1) {
                    pCh = pKids[pi];
                    if (!pCh || pCh === esc.topGroupCtrl || (pCh.mRootElement && isSimoderNode(pCh.mRootElement))) { continue; }
                    if (typeof pCh.__simoderOrigTop !== "number") {
                        pCh.__simoderOrigTop = Number(pCh.mTop || 0);
                    }
                    pOrigTop = pCh.__simoderOrigTop;
                    pTargetTop = pOrigTop >= thresholdY ? (pOrigTop + esc.pitch) : pOrigTop;
                    if (pOrigTop >= thresholdY) {
                        setCtrlTop(pCh, pTargetTop);
                    }
                    if (pCh.mIsVisible !== false && typeof pCh.mHeight === "number") {
                        pBottom = pTargetTop + pCh.mHeight;
                        if (pBottom > maxPanelBottom) {
                            maxPanelBottom = pBottom;
                        }
                    }
                }

                var targetPanelHeight = Math.round(maxPanelBottom);
                if (typeof esc.panelCtrl.SetHeight === "function" && typeof esc.panelCtrl.mHeight === "number" &&
                    Math.abs(esc.panelCtrl.mHeight - targetPanelHeight) > 1) {
                    esc.panelCtrl.SetHeight(targetPanelHeight);
                }
            }

            var lblInfo = extractTriggerLabelAndBadge(btn, "Mods");
            var labelText = lblInfo.label;
            var badgeText = lblInfo.badge;
            var parentEl = mountHostCtrl.mRootElement;
            var clone = doc.getElementById("simoder-esc-vanilla-mods");
            if (!clone || clone.parentNode !== parentEl ||
                clone.getAttribute("data-simoder-vanilla-label") !== labelText ||
                clone.getAttribute("data-simoder-vanilla-badge") !== badgeText) {
                removeVanillaEscButton();
                clone = esc.templateBtn.mRootElement.cloneNode(true);
                populateVanillaCloneContent(clone, labelText, badgeText);
                clone.id = "simoder-esc-vanilla-mods";
                clone.setAttribute("data-simoder-slot", "esc.vanilla.clone");
                clone.setAttribute("data-simoder-vanilla-label", labelText);
                clone.setAttribute("data-simoder-vanilla-badge", badgeText);
                parentEl.appendChild(clone);
            }

            if (clone.style) {
                clone.style.position = "absolute";
                clone.style.left = cloneLeft + "px";
                clone.style.top = cloneTop + "px";
                clone.style.right = "auto";
                clone.style.bottom = "auto";
                clone.style.width = Math.round(esc.width) + "px";
                clone.style.height = Math.round(esc.height) + "px";
                clone.style.cursor = "pointer";
                clone.style.pointerEvents = "auto";
                clone.style.visibility = "visible";
                clone.style.opacity = "1";
                clone.style.zIndex = "10";
                if (!clone.style.display || clone.style.display === "none") {
                    clone.style.display = "inline";
                }
            }

            bindVanillaButtonClone(clone, esc.templateBtn, btn);
            container.style.display = "none";
            return true;
        };

        var positionSlotContainer = function (container, viewName, slotName) {
            if (!container || !container.style) { return; }
            if (slotName === "region.actions") {
                syncRegionTileButtons(container);
                var preview = findActiveRegionPreview();
                if (!preview) {
                    container.style.display = "none";
                    host.activeRegionContext = null;
                    return;
                }
                host.activeRegionContext = {
                    regionId: preview.regionId,
                    regionName: preview.regionName,
                    sandbox: preview.sandbox
                };
                container.style.display = "block";
                container.style.left = Math.round(preview.rect.right - 138) + "px";
                container.style.top = Math.round(preview.rect.bottom - 38) + "px";
                container.style.right = "auto";
                container.style.bottom = "auto";
                return;
            }
            if (slotName !== "menu.actions") { return; }
            if (viewName === "launcher.menu") {
                container.style.display = "block";
                var anchor = findLauncherFooterAnchor();
                if (anchor) {
                    var targetLeft = Math.round(anchor.rect.right + 18);
                    var maxRight = Math.round(anchor.viewW - 265);
                    if (targetLeft > maxRight) { targetLeft = maxRight; }
                    if (targetLeft < 180) { targetLeft = 180; }
                    container.style.left = targetLeft + "px";
                    container.style.right = "auto";
                    container.style.top = Math.round(anchor.rect.top + (anchor.rect.height - 24) / 2) + "px";
                    container.style.bottom = "auto";
                }
            } else if (viewName === "main.menu") {
                var hub = findMainMenuHubColumn();
                if (!hub) {
                    container.style.display = "none";
                    removeVanillaHubButton();
                    return;
                }
                if (hub.panels && hub.panels.length && hub.playCtrl && hub.playPos) {
                    var origPlayTop = hub.playCtrl.mTop;
                    var origPlayHeight = hub.playCtrl.mHeight;
                    var origResumeTop = hub.resumeCtrl ? hub.resumeCtrl.mTop : null;
                    var origResumeHeight = hub.resumeCtrl ? hub.resumeCtrl.mHeight : null;
                    var modsBottomY = hub.playPos[1] + hub.localPitch + hub.playCtrl.mHeight;
                    var containsPanel = function (candidate) {
                        var j;
                        for (j = 0; j < hub.panels.length; j += 1) {
                            if (hub.panels[j] === candidate) { return true; }
                        }
                        return false;
                    };
                    var pi, pCtrl, vPin, pPos, targetHeight;
                    for (pi = 0; pi < hub.panels.length; pi += 1) {
                        pCtrl = hub.panels[pi];
                        if (!pCtrl || typeof pCtrl.SetHeight !== "function" || typeof pCtrl.mHeight !== "number") { continue; }
                        vPin = pCtrl.mVerticalPinType;
                        if (pCtrl.mParentControl && containsPanel(pCtrl.mParentControl) && (vPin === 2 || vPin === 4 || vPin === 5)) {
                            continue;
                        }
                        if (pCtrl.mControlID === 241896001) {
                            targetHeight = Math.round(hub.playCtrl.mTop + hub.localPitch + hub.playCtrl.mHeight);
                        } else {
                            pPos = typeof pCtrl.GetAbsolutePosition === "function" ? pCtrl.GetAbsolutePosition() : [Number(pCtrl.mLeft || 0), Number(pCtrl.mTop || 0)];
                            targetHeight = Math.round((modsBottomY - pPos[1]) - 7);
                        }
                        if (targetHeight > 0 && Math.abs(pCtrl.mHeight - targetHeight) > 1) {
                            pCtrl.SetHeight(targetHeight);
                        }
                    }
                    if (typeof hub.playCtrl.SetPosition === "function" &&
                        typeof origPlayTop === "number" && typeof origPlayHeight === "number" &&
                        (hub.playCtrl.mTop !== origPlayTop || hub.playCtrl.mHeight !== origPlayHeight)) {
                        hub.playCtrl.SetPosition(hub.playCtrl.mLeft, origPlayTop, hub.playCtrl.mWidth, origPlayHeight);
                    }
                    if (hub.resumeCtrl && typeof hub.resumeCtrl.SetPosition === "function" &&
                        typeof origResumeTop === "number" && typeof origResumeHeight === "number" &&
                        (hub.resumeCtrl.mTop !== origResumeTop || hub.resumeCtrl.mHeight !== origResumeHeight)) {
                        hub.resumeCtrl.SetPosition(hub.resumeCtrl.mLeft, origResumeTop, hub.resumeCtrl.mWidth, origResumeHeight);
                    }
                }
                if (mountVanillaHubButton(container, hub)) {
                    return;
                }
                var step = Math.round(hub.rect.height + 8);
                container.style.display = "block";
                container.style.left = Math.round(hub.rect.left) + "px";
                container.style.right = "auto";
                container.style.top = Math.round(hub.rect.top + step) + "px";
                container.style.bottom = "auto";
                container.style.width = Math.round(hub.rect.width) + "px";
                container.style.height = Math.round(hub.rect.height) + "px";
                if (hub.typography && container.querySelector) {
                    var hubBtn = container.querySelector(".simoder-btn");
                    if (hubBtn && hubBtn.style) {
                        if (hub.typography.fontSize) { hubBtn.style.fontSize = hub.typography.fontSize; }
                        if (hub.typography.fontFamily) { hubBtn.style.fontFamily = hub.typography.fontFamily; }
                        if (hub.typography.fontWeight) { hubBtn.style.fontWeight = hub.typography.fontWeight; }
                        if (hub.typography.color) { hubBtn.style.color = hub.typography.color; }
                    }
                }
            } else if (viewName === "ingame.options") {
                var esc = findInGameEscapeMenu();
                if (!esc) {
                    container.style.display = "none";
                    removeVanillaEscButton();
                    return;
                }
                mountVanillaEscButton(container, esc);
            }
        };

        var ensureSlotContainer = function (viewName, slotName) {
            var slotDomId = "simoder-slot-" + viewName.replace(/[^a-zA-Z0-9_-]/g, "_") + "-" + slotName.replace(/[^a-zA-Z0-9_-]/g, "_");
            var existing = doc.getElementById(slotDomId);
            if (existing) {
                positionSlotContainer(existing, viewName, slotName);
                return existing;
            }
            var container = doc.createElement("div");
            container.id = slotDomId;
            container.setAttribute("data-simoder-slot", slotName);
            container.setAttribute("data-simoder-view", viewName);
            container.className = slotClassFor(viewName, slotName);
            if (slotName === "modal.overlay") {
                container.onclick = function (e) {
                    var tgt = e ? (e.target || e.srcElement) : null;
                    if (tgt === container && host.activeModalClose) {
                        pushEvent(host.activeModalClose.patchId, host.activeModalClose.action, "modal_backdrop_close", null);
                    }
                };
            }
            doc.body.appendChild(container);
            positionSlotContainer(container, viewName, slotName);
            return container;
        };

        var makeFieldWrapper = function (labelText) {
            var wrap = doc.createElement("div");
            wrap.className = "simoder-field";
            if (labelText) {
                var lblEl = doc.createElement("div");
                lblEl.className = "simoder-field-label Body_12_Left";
                lblEl.textContent = labelText;
                wrap.appendChild(lblEl);
            }
            return wrap;
        };

        var appendSafeClassName = function (el, rawClassName) {
            if (!el || typeof rawClassName !== "string" || !rawClassName) { return; }
            var safe = rawClassName.replace(/[^A-Za-z0-9 _-]/g, "").slice(0, 256).replace(/^\s+|\s+$/g, "");
            if (safe) {
                el.className = (el.className ? el.className + " " : "") + safe;
            }
        };

        var getNativeUiAudioIds = function () {
            if (host.nativeAudioIds) { return host.nativeAudioIds; }
            var ids = { hover: null, down: null };
            var mgr = window.gUIManager || uiMgr;
            if (mgr && typeof mgr.FindControlByID === "function") {
                var probe = mgr.FindControlByID(212665064) || mgr.FindControlByID(237051648);
                if (probe) {
                    ids.hover = probe.mAudioMouseOnID || null;
                    ids.down = probe.mAudioMouseDownID || null;
                }
            }
            host.nativeAudioIds = ids;
            return ids;
        };

        var playNativeUiSound = function (kind) {
            if (!window.scrui || typeof window.scrui.Play2DSound !== "function") { return; }
            var ids = getNativeUiAudioIds();
            var soundId = kind === "down" ? ids.down : ids.hover;
            if (soundId) {
                try { window.scrui.Play2DSound(soundId); } catch (_e) {}
            }
        };

        var attachVanillaInteractiveFeedback = function (el, toggleStateClasses) {
            if (!el) { return; }
            var swapBtnState = function (cls) {
                if (!toggleStateClasses) { return; }
                var cur = String(el.className || "").replace(/\bcButton(Normal|Rollover|Down|Disabled)\b/g, cls);
                if (cur.indexOf(cls) === -1) { cur += " " + cls; }
                el.className = cur;
            };
            el.onmouseover = function () {
                if (el.disabled) { return; }
                if (!el.__simoderHover) {
                    el.__simoderHover = true;
                    playNativeUiSound("hover");
                }
                swapBtnState("cButtonRollover");
            };
            el.onmouseout = function (e) {
                var rel = e ? (e.relatedTarget || e.toElement) : null;
                if (rel && el.contains && (rel === el || el.contains(rel))) { return; }
                el.__simoderHover = false;
                el.__simoderDown = false;
                swapBtnState("cButtonNormal");
            };
            el.onmousedown = function () {
                if (el.disabled) { return; }
                el.__simoderDown = true;
                playNativeUiSound("down");
                swapBtnState("cButtonDown");
            };
            el.onmouseup = function () {
                el.__simoderDown = false;
                swapBtnState(el.__simoderHover ? "cButtonRollover" : "cButtonNormal");
            };
        };

        var renderNode = function (patchId, node) {
            var el;
            var childHost = null;
            var footerHost = null;
            var skipDefaultChildren = false;
            var kind = node.kind || "panel";
            if (kind === "button" || kind === "toggle") {
                el = doc.createElement("button");
                el.type = "button";
                el.className = "simoder-btn cButton cButtonNormal";
                if (node.tone === "primary") { el.className += " simoder-btn-primary"; }
                else if (node.tone === "danger") { el.className += " simoder-btn-danger"; }
                else if (node.tone === "warning") { el.className += " simoder-btn-warning"; }
                if (node.disabled === true) { el.disabled = true; }
                var lblSpan = doc.createElement("span");
                lblSpan.className = "Head_11_CR_Vmid_UC simoder-btn-label";
                var lbl = node.label || "";
                if (kind === "toggle") {
                    lbl = lbl + (node.checked ? " [ON]" : " [OFF]");
                }
                lblSpan.textContent = lbl;
                el.appendChild(lblSpan);
                if (node.badge) {
                    var b = doc.createElement("span");
                    b.className = "simoder-badge";
                    b.textContent = String(node.badge);
                    el.appendChild(b);
                }
                attachVanillaInteractiveFeedback(el, true);
                (function (pId, act, tId, chk) {
                    el.onclick = function () {
                        var payload = kind === "toggle" ? String(!chk) : null;
                        if (act === "open_region_mods" && host.activeRegionContext) {
                            payload = JSON.stringify(host.activeRegionContext);
                        }
                        pushEvent(pId, act || "click", tId, payload);
                    };
                }(patchId, node.action, node.id, !!node.checked));
            } else if (kind === "input") {
                el = makeFieldWrapper(node.label);
                var inp = doc.createElement("input");
                inp.type = "text"; inp.className = "simoder-input Browse_Tile_Recessed_SharpCorners Body_12_Left"; inp.value = node.value || "";
                if (node.placeholder) { inp.placeholder = node.placeholder; }
                if (node.disabled === true) { inp.disabled = true; }
                (function (pId, act, tId, c) {
                    c.onchange = function () { pushEvent(pId, act || "change", tId, c.value); };
                    c.onkeydown = function (e) { if ((e ? (e.keyCode || e.which) : 0) === 13) pushEvent(pId, act || "change", tId, c.value); };
                }(patchId, node.action, node.id, inp));
                el.appendChild(inp);
            } else if (kind === "select") {
                el = makeFieldWrapper(node.label);
                var sel = doc.createElement("select");
                sel.className = "simoder-select Browse_Tile_Recessed_SharpCorners Body_12_Left";
                if (node.disabled === true) { sel.disabled = true; }
                var opts = node.options || [], oi, optEl;
                for (oi = 0; oi < opts.length; oi += 1) {
                    optEl = doc.createElement("option");
                    optEl.value = opts[oi].value || "";
                    optEl.textContent = opts[oi].label || opts[oi].value || "";
                    if (node.value !== undefined && node.value !== null && String(opts[oi].value) === String(node.value)) optEl.selected = true;
                    sel.appendChild(optEl);
                }
                if (node.value !== undefined && node.value !== null) sel.value = String(node.value);
                (function (pId, act, tId, c) { c.onchange = function () { playNativeUiSound("down"); pushEvent(pId, act || "change", tId, c.value); }; }(patchId, node.action, node.id, sel));
                el.appendChild(sel);
            } else if (kind === "slider") {
                var sMin = (typeof node.min === "number") ? node.min : 0, sMax = (typeof node.max === "number") ? node.max : 100;
                var sVal = (node.value !== undefined && node.value !== null) ? String(node.value) : String(sMin);
                el = makeFieldWrapper((node.label ? node.label + ": " : "") + sVal);
                var rng = doc.createElement("input");
                rng.type = "range"; rng.className = "simoder-slider"; rng.min = String(sMin); rng.max = String(sMax);
                rng.step = String((typeof node.step === "number" && node.step > 0) ? node.step : 1);
                rng.value = sVal;
                if (node.disabled === true) rng.disabled = true;
                (function (pId, act, tId, c, lEl, bLbl) {
                    c.onchange = function () {
                        if (lEl) lEl.textContent = (bLbl ? bLbl + ": " : "") + c.value;
                        pushEvent(pId, act || "change", tId, c.value);
                    };
                }(patchId, node.action, node.id, rng, node.label ? el.firstChild : null, node.label));
                el.appendChild(rng);
            } else if (kind === "checkbox") {
                el = doc.createElement("label"); el.className = "simoder-checkbox-wrap Body_12_Left";
                var cb = doc.createElement("input");
                cb.type = "checkbox"; cb.checked = node.checked === true;
                if (node.disabled === true) cb.disabled = true;
                (function (pId, act, tId, c) { c.onchange = function () { playNativeUiSound("down"); pushEvent(pId, act || "change", tId, String(!!c.checked)); }; }(patchId, node.action, node.id, cb));
                el.appendChild(cb);
                var cbTxt = doc.createElement("span"); cbTxt.className = "Body_12_Left"; cbTxt.textContent = node.label || ""; el.appendChild(cbTxt);
            } else if (kind === "progress") {
                var pMin = (typeof node.min === "number") ? node.min : 0;
                var pMax = (typeof node.max === "number" && node.max > pMin) ? node.max : (pMin + 100);
                var pCur = Number(node.value !== undefined && node.value !== null ? node.value : pMin);
                if (isNaN(pCur)) pCur = pMin;
                var pct = Math.max(0, Math.min(100, Math.round(((pCur - pMin) * 100) / (pMax - pMin))));
                el = makeFieldWrapper((node.label ? node.label + " (" + pct + "%)" : pct + "%"));
                var track = doc.createElement("div"); track.className = "simoder-progress-track Browse_Tile_Recessed_SharpCorners";
                var fill = doc.createElement("div"); fill.className = "simoder-progress-fill";
                if (fill.style) fill.style.width = pct + "%";
                track.appendChild(fill); el.appendChild(track);
            } else if (kind === "tabs") {
                skipDefaultChildren = true;
                el = doc.createElement("div"); el.className = "simoder-tabs";
                var tabBar = doc.createElement("div"); tabBar.className = "simoder-tab-bar";
                var tabBody = doc.createElement("div"); tabBody.className = "simoder-tab-body";
                var tabNodes = node.children || [], activeId = node.value || (tabNodes[0] && tabNodes[0].id) || "";
                var activeTabNode = tabNodes[0] || null, ti, tn, tBtn, tbSpan;
                for (ti = 0; ti < tabNodes.length; ti += 1) { if (tabNodes[ti] && tabNodes[ti].id === activeId) activeTabNode = tabNodes[ti]; }
                for (ti = 0; ti < tabNodes.length; ti += 1) {
                    tn = tabNodes[ti]; if (!tn) continue;
                    tBtn = doc.createElement("button"); tBtn.type = "button";
                    tBtn.className = "simoder-tab-btn" + (tn === activeTabNode ? " simoder-tab-btn-active" : "");
                    tbSpan = doc.createElement("span"); tbSpan.className = "Head_11_CR_Vmid_UC simoder-btn-label"; tbSpan.textContent = tn.label || tn.id || ("Tab " + (ti + 1)); tBtn.appendChild(tbSpan);
                    if (tn.badge) { var tb = doc.createElement("span"); tb.className = "simoder-badge"; tb.textContent = String(tn.badge); tBtn.appendChild(tb); }
                    attachVanillaInteractiveFeedback(tBtn, false);
                    (function (pId, act, tid) { tBtn.onclick = function () { pushEvent(pId, act || "switch_tab", tid, tid); }; }(patchId, node.action || tn.action, tn.id || String(ti)));
                    tabBar.appendChild(tBtn);
                }
                el.appendChild(tabBar);
                if (activeTabNode && activeTabNode.children) {
                    for (ti = 0; ti < activeTabNode.children.length; ti += 1) tabBody.appendChild(renderNode(patchId, activeTabNode.children[ti]));
                }
                el.appendChild(tabBody);
            } else if (kind === "divider") {
                el = doc.createElement("div");
                el.className = "simoder-divider HUD_Palette_Divider";
            } else if (kind === "badge") {
                el = doc.createElement("span");
                el.className = "simoder-badge";
                if (node.tone === "primary") { el.className += " simoder-badge-ok"; }
                else if (node.tone === "muted") { el.className += " simoder-badge-muted"; }
                el.textContent = node.label || node.text || "";
            } else if (kind === "text") {
                el = doc.createElement("div");
                el.className = "simoder-text Body_12_Left";
                if (node.tone === "muted") { el.className = "simoder-text-muted Body_12_Left"; }
                else if (node.tone === "title") { el.className = "simoder-text-title Header_12_Center_Upper"; }
                el.textContent = node.text || node.label || "";
            } else {
                el = doc.createElement("div");
                if (kind === "modal") {
                    el.className = "simoder-modal Panel_flatFill_Bottom Panel_Background";
                    var vw = window.innerWidth || (doc.documentElement && doc.documentElement.clientWidth) || 1280;
                    var vh = window.innerHeight || (doc.documentElement && doc.documentElement.clientHeight) || 720;
                    if (el.style) {
                        if (vw > 0 && vw < 720) {
                            el.style.width = "auto";
                            el.style.left = "20px";
                            el.style.right = "20px";
                            el.style.marginLeft = "0";
                        }
                        if (vh > 0) {
                            var modalH = Math.min(720, Math.max(420, vh - 80));
                            el.style.height = modalH + "px";
                            el.style.top = Math.round((vh - modalH) / 2) + "px";
                            el.style.bottom = "auto";
                        }
                    }
                    if (node.label || node.action) {
                        var hdrBar = doc.createElement("div");
                        hdrBar.className = "simoder-modal-header FE_Header_BG Panel_Header";
                        if (node.action) {
                            var closeBtn = doc.createElement("button");
                            closeBtn.type = "button";
                            closeBtn.className = "simoder-modal-close cButton cButtonNormal";
                            closeBtn.textContent = "x";
                            attachVanillaInteractiveFeedback(closeBtn, true);
                            (function (pId, act) {
                                closeBtn.onclick = function () {
                                    pushEvent(pId, act, "modal_header_close", null);
                                };
                            }(patchId, node.action));
                            hdrBar.appendChild(closeBtn);
                        }
                        var titleSpan = doc.createElement("span");
                        titleSpan.className = "simoder-modal-title Header_20_Center_Upper";
                        titleSpan.textContent = node.label || "";
                        hdrBar.appendChild(titleSpan);
                        el.appendChild(hdrBar);
                    }
                    childHost = doc.createElement("div");
                    childHost.className = "simoder-modal-body";
                    el.appendChild(childHost);
                } else {
                    if (kind === "row") {
                        el.className = "simoder-row";
                    } else if (node.tone === "scroll") {
                        el.className = "simoder-panel simoder-panel-scroll Browse_Tile_Recessed_SharpCorners";
                    } else if (node.tone === "muted" || node.tone === "warning") {
                        el.className = "simoder-panel Rollover_Recessed";
                    } else {
                        el.className = "simoder-panel Browse_Tile_Recessed_SharpCorners";
                    }
                    if (node.label) {
                        var hdr = doc.createElement("div");
                        hdr.className = "simoder-text-title Header_12_Center_Upper";
                        hdr.textContent = node.label;
                        el.appendChild(hdr);
                    }
                }
                if (node.text) {
                    var bodyTxt = doc.createElement("div");
                    bodyTxt.className = "simoder-text Body_12_Left";
                    bodyTxt.textContent = node.text;
                    (childHost || el).appendChild(bodyTxt);
                }
            }
            if (node.id) {
                el.setAttribute("data-simoder-target", String(node.id));
            }
            appendSafeClassName(el, node.className);
            var children = skipDefaultChildren ? [] : (node.children || []);
            var idx, chNode, renderedChild;
            for (idx = 0; idx < children.length; idx += 1) {
                chNode = children[idx];
                renderedChild = renderNode(patchId, chNode);
                if (kind === "modal" && chNode && chNode.tone === "footer") {
                    if (!footerHost) {
                        footerHost = doc.createElement("div");
                        footerHost.className = "simoder-modal-footer Panel_flatFill_Bottom";
                        el.appendChild(footerHost);
                    }
                    footerHost.appendChild(renderedChild);
                } else {
                    (childHost || el).appendChild(renderedChild);
                }
            }
            if (kind === "modal" && childHost && !footerHost) {
                childHost.className += " simoder-modal-body-nofooter";
            }
            return el;
        };

        var reqReloadGen = typeof packet.reloadGen === "number" ? packet.reloadGen : 0;
        var needsReconcile = host.revision !== packet.revision || host.lastPhase !== phase || host.reloadGen !== reqReloadGen;
        var patches = packet.patches || [];
        var activeForPhase = [];
        var i;
        for (i = 0; i < patches.length; i += 1) {
            if (patches[i].view === phase || patches[i].view === "*") {
                activeForPhase.push(patches[i]);
            }
        }

        if (!needsReconcile && activeForPhase.length > 0) {
            var firstCheckId = "simoder-patch-" + activeForPhase[0].patchId;
            if (!doc.getElementById(firstCheckId)) {
                needsReconcile = true;
            } else {
                var actionsSlot = doc.getElementById("simoder-slot-" + phase.replace(/[^a-zA-Z0-9_-]/g, "_") + "-menu_actions");
                if (actionsSlot) {
                    positionSlotContainer(actionsSlot, phase, "menu.actions");
                }
                var regionSlot = doc.getElementById("simoder-slot-" + phase.replace(/[^a-zA-Z0-9_-]/g, "_") + "-region_actions");
                if (regionSlot) {
                    positionSlotContainer(regionSlot, phase, "region.actions");
                }
            }
        }

        if (needsReconcile) {
            ensureStyles();
            host.activeModalClose = null;
            // Remove any existing Simoder slot wrappers whose view no longer matches `phase`,
            // and rebuild active slot wrappers cleanly for deterministic composition order.
            var allNodes = doc.querySelectorAll ? doc.querySelectorAll("[data-simoder-slot]") : [];
            for (i = 0; i < allNodes.length; i += 1) {
                if (allNodes[i].parentNode) {
                    allNodes[i].parentNode.removeChild(allNodes[i]);
                }
            }
            for (i = 0; i < activeForPhase.length; i += 1) {
                var p = activeForPhase[i];
                if (p.slot === "modal.overlay" && p.node && p.node.action) {
                    host.activeModalClose = { patchId: p.patchId, action: p.node.action };
                }
                var container = ensureSlotContainer(phase, p.slot || "menu.actions");
                var wrapper = renderNode(p.patchId, p.node);
                wrapper.id = "simoder-patch-" + p.patchId;
                wrapper.setAttribute("data-simoder-patch", p.patchId);
                wrapper.setAttribute("data-simoder-owner", p.ownerModId);
                wrapper.setAttribute("data-simoder-gen", String(p.ownerGeneration));

                if (p.op === "prepend" && container.firstChild) {
                    container.insertBefore(wrapper, container.firstChild);
                } else if (p.op === "insert_before" && p.target && container.querySelector) {
                    var beforeRef = container.querySelector("[data-simoder-target='" + String(p.target) + "']");
                    if (beforeRef && beforeRef.parentNode === container) {
                        container.insertBefore(wrapper, beforeRef);
                    } else {
                        container.appendChild(wrapper);
                    }
                } else if (p.op === "insert_after" && p.target && container.querySelector) {
                    var afterRef = container.querySelector("[data-simoder-target='" + String(p.target) + "']");
                    if (afterRef && afterRef.parentNode === container) {
                        container.insertBefore(wrapper, afterRef.nextSibling);
                    } else {
                        container.appendChild(wrapper);
                    }
                } else {
                    container.appendChild(wrapper);
                }
                if (phase === "main.menu" && (p.slot || "menu.actions") === "menu.actions") {
                    mountVanillaHubButton(container, findMainMenuHubColumn());
                } else if (phase === "main.menu" && p.slot === "region.actions") {
                    syncRegionTileButtons(container);
                } else if (phase === "ingame.options" && (p.slot || "menu.actions") === "menu.actions") {
                    mountVanillaEscButton(container, findInGameEscapeMenu());
                }
            }
            host.revision = packet.revision;
            host.reloadGen = reqReloadGen;
            host.lastPhase = phase;
        }

        var drained = host.events.splice(0, host.events.length);
        return JSON.stringify({
            ok: true,
            observation: observation,
            events: drained
        });
    } catch (err) {
        return JSON.stringify({
            ok: false,
            error: String(err && err.message ? err.message : err)
        });
    }
})
