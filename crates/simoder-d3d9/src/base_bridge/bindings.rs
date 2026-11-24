#![forbid(unsafe_code)]

//! Private `__simoder_base` capability table bindings exposed exclusively to `Simoder-Base`.

use std::path::Path;
use std::sync::Arc;

use mlua::serde::SerializeOptions;
use mlua::{Lua, LuaSerdeExt, Table, Value};
use serde::Serialize;
use simoder_mods::{KeyChord, KeybindContext, KeybindDescriptor, UiPatchDescriptor};

use super::{ModsActionBackend, UiStateHost};

const SERDE_NIL_OPTIONS: SerializeOptions = SerializeOptions::new().serialize_none_to_null(false);

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct LuaActionResult {
    ok: bool,
    error: Option<String>,
}

fn action_result_to_lua(lua: &Lua, res: Result<(), String>) -> mlua::Result<Value> {
    let dto = match res {
        Ok(()) => LuaActionResult {
            ok: true,
            error: None,
        },
        Err(err) => LuaActionResult {
            ok: false,
            error: Some(err),
        },
    };
    lua.to_value_with(&dto, SERDE_NIL_OPTIONS)
}

pub(super) fn install_private_host_table(
    lua: &Lua,
    runtime: Arc<dyn UiStateHost>,
    backend: Arc<dyn ModsActionBackend>,
) -> Result<(), String> {
    let host = lua.create_table().map_err(|e| e.to_string())?;

    let values = lua.create_table().map_err(|e| e.to_string())?;
    values
        .set(
            "validate",
            lua.create_function(|_, (wire, _schema): (Value, Value)| Ok(wire))
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    values
        .set(
            "wire",
            lua.create_function(|_, val: Value| Ok(val))
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    host.set("values", values).map_err(|e| e.to_string())?;

    let tasks = lua.create_table().map_err(|e| e.to_string())?;
    tasks
        .set(
            "poll",
            lua.create_function(|_, _h: Value| Ok(Value::Nil))
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    tasks
        .set(
            "cancel",
            lua.create_function(|_, _h: Value| Ok((Value::Nil, "Unsupported")))
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    tasks
        .set(
            "observe",
            lua.create_function(|_, (_h, _cb): (Value, Value)| Ok("noop"))
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    tasks
        .set(
            "await",
            lua.create_function(|_, _h: Value| Ok((Value::Nil, "CoroutineNotRegistered")))
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    host.set("tasks", tasks).map_err(|e| e.to_string())?;

    let lifecycle = lua.create_table().map_err(|e| e.to_string())?;
    lifecycle
        .set(
            "snapshot",
            lua.create_function(|lua, ()| {
                let tbl = lua.create_table()?;
                tbl.set("epoch", "1")?;
                tbl.set("sequence", "1")?;
                Ok(tbl)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    lifecycle
        .set(
            "subscribe",
            lua.create_function(|_, _cb: Value| Ok("noop"))
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    host.set("lifecycle", lifecycle).map_err(|e| e.to_string())?;

    let registry = lua.create_table().map_err(|e| e.to_string())?;
    registry
        .set(
            "descriptors",
            lua.create_function(|lua, ()| Ok((lua.create_table()?, 2i32)))
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    host.set("registry", registry).map_err(|e| e.to_string())?;

    let rt_tbl = lua.create_table().map_err(|e| e.to_string())?;
    rt_tbl
        .set(
            "run",
            lua.create_function(|_, _cb: Value| Ok("noop"))
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    rt_tbl
        .set(
            "request",
            lua.create_function(|_, (_m, _v): (Value, Value)| Ok((Value::Nil, "Unavailable")))
                .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    rt_tbl
        .set(
            "observeSession",
            lua.create_function(|_, (_t, _g, _l): (Value, Value, Value)| {
                Ok((Value::Nil, "Unavailable"))
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    host.set("runtime", rt_tbl).map_err(|e| e.to_string())?;

    let ui_tbl = lua.create_table().map_err(|e| e.to_string())?;
    let rt_obs = runtime.clone();
    ui_tbl
        .set(
            "observeViews",
            lua.create_function(move |lua, ()| {
                lua.to_value_with(&rt_obs.get_ui_observation(), SERDE_NIL_OPTIONS)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let rt_commit = runtime.clone();
    ui_tbl
        .set(
            "commitPatches",
            lua.create_function(move |lua, (rev, patches_val): (u64, Value)| {
                let patches: Vec<UiPatchDescriptor> = match &patches_val {
                    Value::Nil => Vec::new(),
                    Value::Table(t)
                        if t.raw_len() == 0 && t.pairs::<Value, Value>().next().is_none() =>
                    {
                        Vec::new()
                    }
                    _ => lua.from_value(patches_val)?,
                };
                rt_commit
                    .commit_composed_ui_patches(rev, patches)
                    .map_err(mlua::Error::runtime)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let rt_poll = runtime;
    ui_tbl
        .set(
            "pollEvents",
            lua.create_function(move |lua, ()| {
                lua.to_value_with(&rt_poll.drain_ui_events(), SERDE_NIL_OPTIONS)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    host.set("ui", ui_tbl).map_err(|e| e.to_string())?;

    install_mods_bindings(lua, &host, &backend)?;
    install_keybinds_bindings(lua, &host, &backend)?;
    install_net_and_system_bindings(lua, &host)?;

    lua.globals()
        .set("__simoder_base", host)
        .map_err(|e| e.to_string())
}

fn install_mods_bindings(
    lua: &Lua,
    host: &Table,
    backend: &Arc<dyn ModsActionBackend>,
) -> Result<(), String> {
    let mods_tbl = lua.create_table().map_err(|e| e.to_string())?;
    let b_list = backend.clone();
    mods_tbl
        .set(
            "list",
            lua.create_function(move |lua, ()| {
                lua.to_value_with(&b_list.list_mods(), SERDE_NIL_OPTIONS)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let b_approve = backend.clone();
    mods_tbl
        .set(
            "approveNative",
            lua.create_function(move |lua, (mod_id, sha256): (String, String)| {
                action_result_to_lua(lua, b_approve.approve_native(&mod_id, &sha256))
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let b_deny = backend.clone();
    mods_tbl
        .set(
            "denyNative",
            lua.create_function(move |lua, (mod_id, sha256): (String, String)| {
                action_result_to_lua(lua, b_deny.deny_native(&mod_id, &sha256))
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let b_revoke = backend.clone();
    mods_tbl
        .set(
            "revokeNative",
            lua.create_function(move |lua, mod_id: String| {
                action_result_to_lua(lua, b_revoke.revoke_native(&mod_id))
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let b_toggle = backend.clone();
    mods_tbl
        .set(
            "setEnabled",
            lua.create_function(move |lua, (mod_id, enabled): (String, bool)| {
                action_result_to_lua(lua, b_toggle.set_enabled(&mod_id, enabled))
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let b_reg_get = backend.clone();
    mods_tbl
        .set(
            "getRegionMods",
            lua.create_function(move |lua, region_id: String| {
                lua.to_value_with(&b_reg_get.get_region_mods(&region_id), SERDE_NIL_OPTIONS)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let b_reg_set = backend.clone();
    mods_tbl
        .set(
            "setRegionModEnabled",
            lua.create_function(
                move |lua, (region_id, mod_id, enabled): (String, String, bool)| {
                    action_result_to_lua(
                        lua,
                        b_reg_set.set_region_mod_enabled(&region_id, &mod_id, enabled),
                    )
                },
            )
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    mods_tbl
        .set(
            "extractZip",
            lua.create_function(|lua, (zip_path, target_dir): (String, String)| {
                let res =
                    crate::net::extract_zip_archive(Path::new(&zip_path), Path::new(&target_dir));
                let tbl = lua.create_table()?;
                match res {
                    Ok(count) => {
                        tbl.set("ok", true)?;
                        tbl.set("count", count)?;
                    }
                    Err(err) => {
                        tbl.set("ok", false)?;
                        tbl.set("error", err)?;
                    }
                }
                Ok(tbl)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    host.set("mods", mods_tbl).map_err(|e| e.to_string())
}

fn install_keybinds_bindings(
    lua: &Lua,
    host: &Table,
    backend: &Arc<dyn ModsActionBackend>,
) -> Result<(), String> {
    let keybinds_tbl = lua.create_table().map_err(|e| e.to_string())?;
    let b_kb_list = backend.clone();
    keybinds_tbl
        .set(
            "list",
            lua.create_function(move |lua, ()| {
                lua.to_value_with(&b_kb_list.list_keybinds(), SERDE_NIL_OPTIONS)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let b_kb_reg = backend.clone();
    keybinds_tbl
        .set(
            "register",
            lua.create_function(move |lua, spec: Table| {
                let res = (|| -> Result<String, String> {
                    let owner_mod_id: String = spec
                        .get("modId")
                        .map_err(|e| format!("keybinds.register requires modId: {e}"))?;
                    let action_id: String = spec
                        .get("id")
                        .map_err(|e| format!("keybinds.register requires id: {e}"))?;
                    let title: String = spec
                        .get("title")
                        .map_err(|e| format!("keybinds.register requires title: {e}"))?;
                    let description: String = spec.get("description").unwrap_or_default();
                    let context_raw: String =
                        spec.get("context").unwrap_or_else(|_| "global".to_string());
                    let default_raw: Option<String> = spec.get("defaultKey").ok();
                    let default_chord = match default_raw
                        .as_deref()
                        .map(str::trim)
                        .filter(|s| !s.is_empty())
                    {
                        Some(raw) => Some(KeyChord::parse(raw)?),
                        None => None,
                    };
                    let desc = KeybindDescriptor {
                        owner_mod_id,
                        action_id,
                        title,
                        description,
                        context: KeybindContext::parse(&context_raw)?,
                        default_chord,
                    };
                    b_kb_reg.register_keybind(desc)
                })();
                let tbl = lua.create_table()?;
                match res {
                    Ok(id) => {
                        tbl.set("ok", true)?;
                        tbl.set("id", id)?;
                    }
                    Err(err) => {
                        tbl.set("ok", false)?;
                        tbl.set("error", err)?;
                    }
                }
                Ok(tbl)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let b_kb_rebind = backend.clone();
    keybinds_tbl
        .set(
            "rebind",
            lua.create_function(move |lua, (id, chord): (String, Option<String>)| {
                let res = b_kb_rebind.rebind_keybind(&id, chord.as_deref()).map(|_| ());
                action_result_to_lua(lua, res)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    let b_kb_reset = backend.clone();
    keybinds_tbl
        .set(
            "reset",
            lua.create_function(move |lua, id: String| {
                action_result_to_lua(lua, b_kb_reset.reset_keybind(&id))
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    host.set("keybinds", keybinds_tbl).map_err(|e| e.to_string())
}

fn install_net_and_system_bindings(lua: &Lua, host: &Table) -> Result<(), String> {
    let net_tbl = lua.create_table().map_err(|e| e.to_string())?;
    net_tbl
        .set(
            "getJson",
            lua.create_function(|lua, url: String| {
                let res = crate::net::get_json(&url);
                let tbl = lua.create_table()?;
                match res {
                    Ok(json) => {
                        tbl.set("ok", true)?;
                        tbl.set("body", json)?;
                    }
                    Err(err) => {
                        tbl.set("ok", false)?;
                        tbl.set("error", err)?;
                    }
                }
                Ok(tbl)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    net_tbl
        .set(
            "downloadFile",
            lua.create_function(|lua, (url, dest_path): (String, String)| {
                let res =
                    crate::net::download_file(&url, Path::new(&dest_path), |_cur, _tot| {});
                let tbl = lua.create_table()?;
                match res {
                    Ok(()) => {
                        tbl.set("ok", true)?;
                    }
                    Err(err) => {
                        tbl.set("ok", false)?;
                        tbl.set("error", err)?;
                    }
                }
                Ok(tbl)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    host.set("net", net_tbl).map_err(|e| e.to_string())?;

    let sys_tbl = lua.create_table().map_err(|e| e.to_string())?;
    sys_tbl
        .set(
            "openUrl",
            lua.create_function(|_, url: String| {
                #[cfg(all(windows, not(test)))]
                {
                    crate::win32_native::open_url_in_browser(&url);
                }
                #[cfg(any(not(windows), test))]
                {
                    let _ = url;
                }
                Ok(true)
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    sys_tbl
        .set(
            "copyToClipboard",
            lua.create_function(|_, text: String| {
                #[cfg(all(windows, not(test)))]
                {
                    match crate::win32_native::copy_text_to_clipboard(&text) {
                        Ok(()) => Ok((true, Option::<String>::None)),
                        Err(err) => Ok((false, Some(err))),
                    }
                }
                #[cfg(any(not(windows), test))]
                {
                    let _ = text;
                    Ok((true, Option::<String>::None))
                }
            })
            .map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
    host.set("system", sys_tbl).map_err(|e| e.to_string())
}
