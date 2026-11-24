#![forbid(unsafe_code)]

//! Runtime lifecycle, SHA-256 `native_trust.toon` admission, live `Mods UI` approval,
//! hot-reload, keybind synchronization, clipboard polling, and MCP tool dispatch for
//! `dev_ui` script mods.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, OnceLock};

use parking_lot::Mutex;
use simoder_core::{LogLevel, LogSourceType};
use simoder_mods::{
    NATIVE_TRUST_FILE_NAME, NativeModStatus, NativeModUiRecord, NativeTrustDecision,
    NativeTrustStore,
};

use crate::dev_ui_mods::{
    DevUiModDescriptor, build_dev_ui_packet, discover_dev_ui_mods, file_mtime_ms,
    load_dev_ui_keybind_manifest, load_dev_ui_tool_manifest, read_dev_ui_script_with_sha256,
    register_dev_ui_keybinds, register_dev_ui_tools,
};
use crate::eawebkit_bridge::eval_active_view_script;
use crate::proxy_ffi::global_simoder_runtime;
use crate::runtime::NativeModCandidateState;
use crate::win32_console::unix_time_ms;
use crate::win32_loader::emit_log;
use crate::win32_semantic_abi::copy_text_to_clipboard;
use crate::win32_window_dispatch::{
    eval_js_on_game_thread, set_dev_ui_overlay_rects, take_dev_ui_drag_delta, wake_ui_dispatch,
};

static LAST_DEV_UI_POLL_MS: AtomicU64 = AtomicU64::new(0);
static DEV_UI_STATE: OnceLock<Mutex<Option<DevUiRuntimeState>>> = OnceLock::new();

struct ActiveDevUiMod {
    generation: u64,
    mtime_ms: u64,
    keybinds_revision: u64,
    sha256: String,
    script: String,
}

struct DevUiRuntimeState {
    mods: Vec<DevUiModDescriptor>,
    active: BTreeMap<String, ActiveDevUiMod>,
    approved_mods: BTreeSet<String>,
    trust_path: PathBuf,
    dev_hot_reload: bool,
    next_generation: u64,
}

fn dev_ui_state_slot() -> &'static Mutex<Option<DevUiRuntimeState>> {
    DEV_UI_STATE.get_or_init(|| Mutex::new(None))
}

/// Discover `dev_ui` script mods under `mods_root` and register them with the unified
/// `Mods UI` / `native_trust.toon` approval pipeline (zero `config.toon` edits required).
pub(crate) fn init_dev_ui_mods(data_root: &Path, mods_root: &Path, dev_hot_reload: bool) {
    let discovered = discover_dev_ui_mods(mods_root);
    if discovered.is_empty() {
        return;
    }

    let trust_path = data_root.join(NATIVE_TRUST_FILE_NAME);
    let store = NativeTrustStore::load(&trust_path).unwrap_or_default();
    let runtime = global_simoder_runtime();
    let mut approved_mods = BTreeSet::new();
    let mut pending_count = 0usize;

    for desc in &discovered {
        let Ok((script, sha256)) = read_dev_ui_script_with_sha256(&desc.script_path) else {
            continue;
        };
        let script_len = script.len();
        let (status, diagnostic) = match store.decision_for(&desc.mod_id, &sha256) {
            Some(NativeTrustDecision::Approved) => {
                approved_mods.insert(desc.mod_id.clone());
                (NativeModStatus::Active, None)
            }
            Some(NativeTrustDecision::Denied) => (NativeModStatus::Denied, None),
            None => {
                pending_count += 1;
                (
                    NativeModStatus::PendingApproval,
                    Some(
                        "Direct EAWebKit DOM overlay (dev_ui) requires user approval in Mods UI"
                            .to_string(),
                    ),
                )
            }
        };

        runtime.register_dev_ui_candidate(NativeModCandidateState {
            record: NativeModUiRecord {
                mod_id: desc.mod_id.clone(),
                name: desc.name.clone(),
                version: desc.version.clone(),
                entry: desc.entry.clone(),
                sha256,
                status,
                diagnostic,
            },
            dll_path: desc.script_path.clone(),
            trust_path: trust_path.clone(),
            source_size_bytes: script_len as u64,
            mapped_image_bytes: 0,
            owner: None,
        });
    }

    emit_log(
        LogLevel::Info,
        LogSourceType::Loader,
        "simoder.dev_ui",
        &format!(
            "Discovered {} dev_ui script mod(s) ({} approved, {} pending UI consent)",
            discovered.len(),
            approved_mods.len(),
            pending_count
        ),
    );
    if pending_count > 0 {
        wake_ui_dispatch();
    }

    *dev_ui_state_slot().lock() = Some(DevUiRuntimeState {
        mods: discovered,
        active: BTreeMap::new(),
        approved_mods,
        trust_path,
        dev_hot_reload,
        next_generation: 1,
    });
}

/// Return `true` when `mod_id` is a discovered `dev_ui` script mod.
pub(crate) fn is_dev_ui_mod(mod_id: &str) -> bool {
    dev_ui_state_slot()
        .lock()
        .as_ref()
        .is_some_and(|st| st.mods.iter().any(|m| m.mod_id == mod_id))
}

/// Approve, persist in `native_trust.toon`, and immediately activate a `dev_ui` mod without restart.
pub(crate) fn approve_dev_ui_mod(mod_id: &str, expected_sha256: &str) -> Result<(), String> {
    let runtime = global_simoder_runtime();
    let (entry_rel, script_path, trust_path) = {
        let guard = dev_ui_state_slot().lock();
        let st = guard
            .as_ref()
            .ok_or_else(|| "dev_ui runtime not initialized".to_string())?;
        let desc = st
            .mods
            .iter()
            .find(|m| m.mod_id == mod_id)
            .ok_or_else(|| format!("Unknown dev_ui mod '{mod_id}'"))?;
        (
            desc.entry.clone(),
            desc.script_path.clone(),
            st.trust_path.clone(),
        )
    };

    let (_script, actual_sha256) = read_dev_ui_script_with_sha256(&script_path)?;
    if !actual_sha256.eq_ignore_ascii_case(expected_sha256) {
        runtime.update_dev_ui_candidate_status(
            mod_id,
            NativeModStatus::PendingApproval,
            Some(actual_sha256.clone()),
            Some("dev_ui script changed on disk; re-verify SHA-256 before approving".into()),
        );
        return Err(format!(
            "dev_ui script SHA-256 changed on disk (expected {expected_sha256}, observed {actual_sha256})"
        ));
    }

    let mut store = NativeTrustStore::load(&trust_path).unwrap_or_default();
    store
        .set_decision(mod_id, &entry_rel, &actual_sha256, NativeTrustDecision::Approved)
        .map_err(|e| e.to_string())?;
    store.save(&trust_path).map_err(|e| e.to_string())?;

    if let Some(st) = dev_ui_state_slot().lock().as_mut() {
        st.approved_mods.insert(mod_id.to_string());
    }
    runtime.update_dev_ui_candidate_status(
        mod_id,
        NativeModStatus::Active,
        Some(actual_sha256),
        None,
    );
    LAST_DEV_UI_POLL_MS.store(0, Ordering::Relaxed);
    wake_ui_dispatch();
    Ok(())
}

/// Record user refusal for a `dev_ui` mod in `native_trust.toon`.
pub(crate) fn deny_dev_ui_mod(mod_id: &str, expected_sha256: &str) -> Result<(), String> {
    let runtime = global_simoder_runtime();
    let candidate = runtime
        .get_dev_ui_candidate(mod_id)
        .ok_or_else(|| format!("Unknown dev_ui mod '{mod_id}'"))?;
    if !candidate.record.sha256.eq_ignore_ascii_case(expected_sha256) {
        return Err(format!(
            "SHA-256 mismatch denying '{mod_id}': expected {}, got {expected_sha256}",
            candidate.record.sha256
        ));
    }
    let mut store = NativeTrustStore::load(&candidate.trust_path).unwrap_or_default();
    store
        .set_decision(
            mod_id,
            &candidate.record.entry,
            &candidate.record.sha256,
            NativeTrustDecision::Denied,
        )
        .map_err(|e| e.to_string())?;
    store.save(&candidate.trust_path).map_err(|e| e.to_string())?;
    if let Some(st) = dev_ui_state_slot().lock().as_mut() {
        st.approved_mods.remove(mod_id);
    }
    runtime.update_dev_ui_candidate_status(mod_id, NativeModStatus::Denied, None, None);
    LAST_DEV_UI_POLL_MS.store(0, Ordering::Relaxed);
    wake_ui_dispatch();
    Ok(())
}

/// Revoke a live `dev_ui` mod immediately and mark it denied in `native_trust.toon`.
pub(crate) fn revoke_dev_ui_mod(mod_id: &str) -> Result<(), String> {
    let runtime = global_simoder_runtime();
    let candidate = runtime
        .get_dev_ui_candidate(mod_id)
        .ok_or_else(|| format!("Unknown dev_ui mod '{mod_id}'"))?;
    if !candidate.record.sha256.is_empty() {
        let mut store = NativeTrustStore::load(&candidate.trust_path).unwrap_or_default();
        store
            .set_decision(
                mod_id,
                &candidate.record.entry,
                &candidate.record.sha256,
                NativeTrustDecision::Denied,
            )
            .map_err(|e| e.to_string())?;
        store.save(&candidate.trust_path).map_err(|e| e.to_string())?;
    }
    if let Some(st) = dev_ui_state_slot().lock().as_mut() {
        st.approved_mods.remove(mod_id);
    }
    runtime.update_dev_ui_candidate_status(
        mod_id,
        NativeModStatus::Denied,
        None,
        Some("Revoked by user in Mods UI".into()),
    );
    LAST_DEV_UI_POLL_MS.store(0, Ordering::Relaxed);
    wake_ui_dispatch();
    Ok(())
}

/// Forward any accumulated Win32 drag-handle delta to active `dev_ui` overlays.
pub(crate) fn dispatch_dev_ui_drag_move(hwnd: usize) {
    if let Some([dx, dy]) = take_dev_ui_drag_delta()
        && let Ok(pkt) = build_dev_ui_packet("drag_move", &serde_json::json!({ "dx": dx, "dy": dy }))
        && let Some(state) = dev_ui_state_slot().lock().as_ref()
    {
        for active in state.active.values() {
            let _ = eval_active_view_script(hwnd, &active.script, &pkt);
        }
    }
}

/// Revoke all active `dev_ui` tools and keybinds and clear overlay hit-test rects on shutdown.
pub(crate) fn shutdown_dev_ui_mods() {
    set_dev_ui_overlay_rects(None, None);
    let mut guard = dev_ui_state_slot().lock();
    if let Some(state) = guard.take() {
        let runtime = global_simoder_runtime();
        runtime.with_tool_registry_mut(|reg| {
            for (mod_id, active) in state.active {
                let _ = reg.revoke_mod_generation(&mod_id, active.generation);
                runtime.revoke_mod_keybinds(&mod_id);
            }
        });
    }
}

/// Poll `dev_ui` script mods for trust approval, enable/disable, hot-reload, keybind updates,
/// overlay rects, and generic clipboard requests on the owning UI thread.
pub(crate) fn sync_dev_ui_mods_on_tick(hwnd: usize, view_changed: bool) {
    let now_ms = unix_time_ms();
    let prev = LAST_DEV_UI_POLL_MS.load(Ordering::Relaxed);
    if !view_changed && now_ms.saturating_sub(prev) < 350 {
        return;
    }
    LAST_DEV_UI_POLL_MS.store(now_ms, Ordering::Relaxed);

    let mut guard = dev_ui_state_slot().lock();
    let Some(state) = guard.as_mut() else {
        return;
    };
    let runtime = global_simoder_runtime();

    let active_region = runtime.active_region();
    let mut active_overlay_rect: Option<[f32; 4]> = None;
    let mut active_drag_handle_rect: Option<[f32; 4]> = None;
    let dev_hot_reload = state.dev_hot_reload;
    for desc in &state.mods {
        let approved = state.approved_mods.contains(&desc.mod_id);
        let enabled =
            approved && runtime.effective_mod_enabled(active_region.as_deref(), &desc.mod_id);
        if !enabled {
            if let Some(prev_active) = state.active.remove(&desc.mod_id) {
                if let Ok(pkt) = build_dev_ui_packet("destroy", &serde_json::json!({})) {
                    let _ = eval_active_view_script(hwnd, &prev_active.script, &pkt);
                }
                runtime.with_tool_registry_mut(|reg| {
                    let _ = reg.revoke_mod_generation(&desc.mod_id, prev_active.generation);
                });
                runtime.revoke_mod_keybinds(&desc.mod_id);
            }
            continue;
        }

        let mtime = file_mtime_ms(&desc.script_path);
        let needs_init = match state.active.get(&desc.mod_id) {
            None => true,
            Some(cur) => cur.mtime_ms != mtime && mtime != 0,
        };

        if needs_init {
            let Ok((script, new_sha256)) = read_dev_ui_script_with_sha256(&desc.script_path)
            else {
                continue;
            };
            if !dev_hot_reload
                && let Some(prev_active) = state.active.get(&desc.mod_id)
                && !prev_active.sha256.eq_ignore_ascii_case(&new_sha256)
            {
                let old = state.active.remove(&desc.mod_id);
                if let Some(old_active) = old {
                    if let Ok(pkt) = build_dev_ui_packet("destroy", &serde_json::json!({})) {
                        let _ = eval_active_view_script(hwnd, &old_active.script, &pkt);
                    }
                    runtime.with_tool_registry_mut(|reg| {
                        let _ = reg.revoke_mod_generation(&desc.mod_id, old_active.generation);
                    });
                    runtime.revoke_mod_keybinds(&desc.mod_id);
                }
                state.approved_mods.remove(&desc.mod_id);
                runtime.update_dev_ui_candidate_status(
                    &desc.mod_id,
                    NativeModStatus::PendingApproval,
                    Some(new_sha256),
                    Some("dev_ui script modified on disk; re-approval required in Mods UI".into()),
                );
                wake_ui_dispatch();
                continue;
            }
            if dev_hot_reload {
                runtime.update_dev_ui_candidate_status(
                    &desc.mod_id,
                    NativeModStatus::Active,
                    Some(new_sha256.clone()),
                    None,
                );
            }

            runtime.revoke_mod_keybinds(&desc.mod_id);
            let mut registered_kb_count = 0usize;
            if let Some(kb_path) = &desc.keybinds_path
                && let Ok(raw_kb) = std::fs::read_to_string(kb_path)
            {
                match load_dev_ui_keybind_manifest(&raw_kb) {
                    Ok(entries) => {
                        match runtime.with_keybinds_mut(|reg| {
                            register_dev_ui_keybinds(reg, &desc.mod_id, &entries)
                        }) {
                            Ok(cnt) => registered_kb_count = cnt,
                            Err(err) => {
                                emit_log(LogLevel::Warning, LogSourceType::Mod, &desc.mod_id, &err)
                            }
                        }
                    }
                    Err(err) => emit_log(LogLevel::Warning, LogSourceType::Mod, &desc.mod_id, &err),
                }
            }
            let keybinds_map = runtime.mod_keybinds_map(&desc.mod_id);
            let kb_rev = runtime.keybinds_revision();
            let Ok(init_pkt) = build_dev_ui_packet(
                "init",
                &serde_json::json!({ "modId": desc.mod_id, "keybinds": keybinds_map }),
            ) else {
                continue;
            };
            if eval_active_view_script(hwnd, &script, &init_pkt).is_err() {
                continue;
            }
            if registered_kb_count > 0 {
                emit_log(
                    LogLevel::Info,
                    LogSourceType::Mod,
                    &desc.mod_id,
                    &format!("Registered {registered_kb_count} keybind(s) from dev_ui/keybinds.json"),
                );
            }
            if let Some(old) = state.active.remove(&desc.mod_id) {
                runtime.with_tool_registry_mut(|reg| {
                    let _ = reg.revoke_mod_generation(&desc.mod_id, old.generation);
                });
            }
            let generation = state.next_generation;
            state.next_generation = state.next_generation.saturating_add(1);

            if let Some(tools_path) = &desc.tools_path
                && let Ok(raw_tools) = std::fs::read_to_string(tools_path)
                && let Ok(entries) = load_dev_ui_tool_manifest(&raw_tools)
            {
                let script_arc = Arc::new(script.clone());
                let mod_id_log = desc.mod_id.clone();
                let res = runtime.with_tool_registry_mut(|reg| {
                    register_dev_ui_tools(
                        reg,
                        &desc.mod_id,
                        generation,
                        &entries,
                        move |op, args| {
                            let pkt = build_dev_ui_packet(op, args)?;
                            let reply_raw = eval_js_on_game_thread(&script_arc, &pkt, 5_000)?;
                            let parsed: serde_json::Value = serde_json::from_str(&reply_raw)
                                .map_err(|e| format!("dev_ui reply JSON error: {e}"))?;
                            if parsed.get("ok").and_then(|v| v.as_bool()) != Some(true) {
                                return Err(parsed
                                    .get("error")
                                    .and_then(|v| v.as_str())
                                    .unwrap_or("dev_ui tool failed")
                                    .to_string());
                            }
                            let result = parsed
                                .get("result")
                                .cloned()
                                .unwrap_or(serde_json::Value::Null);
                            if let Some(clip) = result.get("clipboardText").and_then(|v| v.as_str())
                                && !clip.is_empty()
                            {
                                let _ = copy_text_to_clipboard(clip);
                            }
                            Ok(result)
                        },
                    )
                });
                match res {
                    Ok(cnt) => emit_log(
                        LogLevel::Info,
                        LogSourceType::Mod,
                        &mod_id_log,
                        &format!("Initialized dev_ui mod ({cnt} optional MCP tool(s) registered)"),
                    ),
                    Err(err) => emit_log(LogLevel::Warning, LogSourceType::Mod, &mod_id_log, &err),
                }
            }
            state.active.insert(
                desc.mod_id.clone(),
                ActiveDevUiMod {
                    generation,
                    mtime_ms: mtime,
                    keybinds_revision: kb_rev,
                    sha256: new_sha256,
                    script,
                },
            );
        } else if let Some(cur) = state.active.get_mut(&desc.mod_id) {
            let current_kb_rev = runtime.keybinds_revision();
            if view_changed {
                cur.keybinds_revision = current_kb_rev;
                let keybinds_map = runtime.mod_keybinds_map(&desc.mod_id);
                if let Ok(init_pkt) = build_dev_ui_packet(
                    "init",
                    &serde_json::json!({ "modId": desc.mod_id, "keybinds": keybinds_map }),
                ) {
                    let _ = eval_active_view_script(hwnd, &cur.script, &init_pkt);
                }
            } else if cur.keybinds_revision != current_kb_rev {
                cur.keybinds_revision = current_kb_rev;
                let keybinds_map = runtime.mod_keybinds_map(&desc.mod_id);
                if let Ok(kb_pkt) = build_dev_ui_packet(
                    "keybinds_updated",
                    &serde_json::json!({ "keybinds": keybinds_map }),
                ) {
                    let _ = eval_active_view_script(hwnd, &cur.script, &kb_pkt);
                }
            }
            if let Ok(pkt) = build_dev_ui_packet("poll_clipboard", &serde_json::json!({}))
                && let Ok(raw_reply) = eval_active_view_script(hwnd, &cur.script, &pkt)
                && let Ok(parsed) = serde_json::from_str::<serde_json::Value>(&raw_reply)
            {
                let res_obj = parsed.get("result");
                if let Some(rect_obj) = res_obj.and_then(|r| r.get("overlayRect"))
                    && let (Some(l), Some(t), Some(r), Some(b)) = (
                        rect_obj.get("left").and_then(|v| v.as_f64()),
                        rect_obj.get("top").and_then(|v| v.as_f64()),
                        rect_obj.get("right").and_then(|v| v.as_f64()),
                        rect_obj.get("bottom").and_then(|v| v.as_f64()),
                    )
                {
                    active_overlay_rect = Some([l as f32, t as f32, r as f32, b as f32]);
                } else if res_obj
                    .and_then(|r| r.get("overlayActive"))
                    .and_then(|v| v.as_bool())
                    == Some(true)
                    && active_overlay_rect.is_none()
                {
                    active_overlay_rect = Some([0.0, 0.0, 1.0, 1.0]);
                }
                if let Some(handle_obj) = res_obj.and_then(|r| r.get("dragHandleRect"))
                    && let (Some(l), Some(t), Some(r), Some(b)) = (
                        handle_obj.get("left").and_then(|v| v.as_f64()),
                        handle_obj.get("top").and_then(|v| v.as_f64()),
                        handle_obj.get("right").and_then(|v| v.as_f64()),
                        handle_obj.get("bottom").and_then(|v| v.as_f64()),
                    )
                {
                    active_drag_handle_rect = Some([l as f32, t as f32, r as f32, b as f32]);
                }
                if let Some(text) = res_obj
                    .and_then(|r| r.get("clipboardText"))
                    .and_then(|v| v.as_str())
                    && !text.is_empty()
                {
                    match copy_text_to_clipboard(text) {
                        Ok(()) => emit_log(
                            LogLevel::Info,
                            LogSourceType::Mod,
                            &desc.mod_id,
                            "Copied dev_ui clipboard payload to Windows clipboard",
                        ),
                        Err(err) => {
                            emit_log(LogLevel::Warning, LogSourceType::Mod, &desc.mod_id, &err)
                        }
                    }
                }
            }
        }
    }
    set_dev_ui_overlay_rects(active_overlay_rect, active_drag_handle_rect);
}
