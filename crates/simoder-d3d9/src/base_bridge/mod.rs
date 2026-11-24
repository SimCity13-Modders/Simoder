#![forbid(unsafe_code)]

//! Sandboxed `mlua` Luau host running `Simoder-Base` (`init.luau`, `services/ui-manager.luau`,
//! `services/mods-ui.luau`) and binding private `__simoder_base.{ui, mods, keybinds}` capabilities.

mod bindings;

use std::collections::{BTreeMap, HashSet};
use std::fs;
use std::path::Path;
use std::sync::Arc;

use mlua::{Function, Lua, Table, Value};
use parking_lot::Mutex;
use simoder_mods::{
    KeybindDescriptor, KeybindSnapshotEntry, UiPatchDescriptor, UiPatchEvent, UiViewObservation,
};
use walkdir::WalkDir;

use self::bindings::install_private_host_table;
use crate::runtime::{IntegratedSimoderRuntime, ModsUiSnapshot};

/// Host UI transport state contract shared between runtime-owned and static global runtimes.
pub trait UiStateHost: Send + Sync {
    /// Read the latest cached EAWebKit view observation.
    fn get_ui_observation(&self) -> UiViewObservation;
    /// Replace active composed patches committed by `Simoder-Base`'s `ui-manager`.
    fn commit_composed_ui_patches(
        &self,
        revision_hint: u64,
        patches: Vec<UiPatchDescriptor>,
    ) -> Result<u64, String>;
    /// Drain queued DOM click/toggle events.
    fn drain_ui_events(&self) -> Vec<UiPatchEvent>;
    /// Record or clear whether `mod_id` requests skipping the startup intro movie.
    fn set_mod_skip_intro(&self, _mod_id: &str, _enabled: bool) {}
    /// Clear any game-action flags owned by `mod_id`.
    fn revoke_mod_game_actions(&self, _mod_id: &str) {}
}

impl UiStateHost for IntegratedSimoderRuntime {
    fn get_ui_observation(&self) -> UiViewObservation {
        IntegratedSimoderRuntime::get_ui_observation(self)
    }

    fn commit_composed_ui_patches(
        &self,
        revision_hint: u64,
        patches: Vec<UiPatchDescriptor>,
    ) -> Result<u64, String> {
        IntegratedSimoderRuntime::commit_composed_ui_patches(self, revision_hint, patches)
    }

    fn drain_ui_events(&self) -> Vec<UiPatchEvent> {
        IntegratedSimoderRuntime::drain_ui_events(self)
    }

    fn set_mod_skip_intro(&self, mod_id: &str, enabled: bool) {
        IntegratedSimoderRuntime::set_mod_skip_intro(self, mod_id, enabled);
    }

    fn revoke_mod_game_actions(&self, mod_id: &str) {
        IntegratedSimoderRuntime::revoke_mod_game_actions(self, mod_id);
    }
}

impl UiStateHost for &'static IntegratedSimoderRuntime {
    fn get_ui_observation(&self) -> UiViewObservation {
        IntegratedSimoderRuntime::get_ui_observation(self)
    }

    fn commit_composed_ui_patches(
        &self,
        revision_hint: u64,
        patches: Vec<UiPatchDescriptor>,
    ) -> Result<u64, String> {
        IntegratedSimoderRuntime::commit_composed_ui_patches(self, revision_hint, patches)
    }

    fn drain_ui_events(&self) -> Vec<UiPatchEvent> {
        IntegratedSimoderRuntime::drain_ui_events(self)
    }

    fn set_mod_skip_intro(&self, mod_id: &str, enabled: bool) {
        IntegratedSimoderRuntime::set_mod_skip_intro(self, mod_id, enabled);
    }

    fn revoke_mod_game_actions(&self, mod_id: &str) {
        IntegratedSimoderRuntime::revoke_mod_game_actions(self, mod_id);
    }
}

const BASE_MEMORY_LIMIT_BYTES: usize = 16 * 1024 * 1024;

/// Host callback interface wiring `Simoder-Base` `mods-ui` actions to either Win32 native loading or test doubles.
pub trait ModsActionBackend: Send + Sync {
    /// Return a combined snapshot of discovered native `.dll` and authoring mods.
    fn list_mods(&self) -> ModsUiSnapshot;
    /// Explicitly trust `sha256`, persist `native_trust.toon`, and load/admit `mod_id`.
    fn approve_native(&self, mod_id: &str, sha256: &str) -> Result<(), String>;
    /// Record an explicit refusal in `native_trust.toon` for `(mod_id, sha256)`.
    fn deny_native(&self, mod_id: &str, sha256: &str) -> Result<(), String>;
    /// Cooperatively shut down and deny future loads for `mod_id`.
    fn revoke_native(&self, mod_id: &str) -> Result<(), String>;
    /// Enable or disable an authoring/script mod in [`simoder_mods::ModManager`].
    fn set_enabled(&self, mod_id: &str, enabled: bool) -> Result<(), String>;
    /// Read per-region mod enablement overrides (`mod_id -> enabled`).
    fn get_region_mods(&self, _region_id: &str) -> BTreeMap<String, bool> {
        BTreeMap::new()
    }
    /// Set a per-region mod enablement override for `region_id`.
    fn set_region_mod_enabled(
        &self,
        _region_id: &str,
        _mod_id: &str,
        _enabled: bool,
    ) -> Result<(), String> {
        Err("Per-region mod contexts are unavailable".into())
    }
    /// List all registered keybinds and any active conflicts.
    fn list_keybinds(&self) -> Vec<KeybindSnapshotEntry> {
        Vec::new()
    }
    /// Register or update a keybind descriptor.
    fn register_keybind(&self, _desc: KeybindDescriptor) -> Result<String, String> {
        Err("Keybind registry is unavailable".into())
    }
    /// Rebind a keybind to `chord` (`None` or empty string unbinds).
    fn rebind_keybind(&self, _id: &str, _chord: Option<&str>) -> Result<Option<String>, String> {
        Err("Keybind registry is unavailable".into())
    }
    /// Reset a keybind back to its default chord.
    fn reset_keybind(&self, _id: &str) -> Result<(), String> {
        Err("Keybind registry is unavailable".into())
    }
}

/// Pure in-memory [`ModsActionBackend`] backed by [`IntegratedSimoderRuntime`] for unit and integration tests.
pub struct RuntimeOnlyModsBackend {
    runtime: Arc<IntegratedSimoderRuntime>,
}

impl RuntimeOnlyModsBackend {
    /// Construct a test/headless backend bound to `runtime`.
    pub const fn new(runtime: Arc<IntegratedSimoderRuntime>) -> Self {
        Self { runtime }
    }
}

impl ModsActionBackend for RuntimeOnlyModsBackend {
    fn list_mods(&self) -> ModsUiSnapshot {
        self.runtime.mods_ui_snapshot()
    }

    fn approve_native(&self, mod_id: &str, sha256: &str) -> Result<(), String> {
        let Some(candidate) = self.runtime.get_native_candidate(mod_id) else {
            return Err(format!("Unknown native mod '{mod_id}'"));
        };
        if !candidate.record.sha256.eq_ignore_ascii_case(sha256) {
            return Err(format!(
                "SHA-256 mismatch for '{mod_id}': expected {}, got {sha256}",
                candidate.record.sha256
            ));
        }
        self.runtime.update_native_candidate_status(
            mod_id,
            simoder_mods::NativeModStatus::Active,
            None,
            None,
            None,
        );
        Ok(())
    }

    fn deny_native(&self, mod_id: &str, sha256: &str) -> Result<(), String> {
        let Some(candidate) = self.runtime.get_native_candidate(mod_id) else {
            return Err(format!("Unknown native mod '{mod_id}'"));
        };
        if !candidate.record.sha256.eq_ignore_ascii_case(sha256) {
            return Err(format!(
                "SHA-256 mismatch for '{mod_id}': expected {}, got {sha256}",
                candidate.record.sha256
            ));
        }
        self.runtime.update_native_candidate_status(
            mod_id,
            simoder_mods::NativeModStatus::Denied,
            None,
            None,
            None,
        );
        Ok(())
    }

    fn revoke_native(&self, mod_id: &str) -> Result<(), String> {
        if !self.runtime.update_native_candidate_status(
            mod_id,
            simoder_mods::NativeModStatus::Denied,
            None,
            None,
            Some("Revoked by user".into()),
        ) {
            return Err(format!("Unknown native mod '{mod_id}'"));
        }
        Ok(())
    }

    fn set_enabled(&self, mod_id: &str, enabled: bool) -> Result<(), String> {
        self.runtime.with_mod_manager_mut(|maybe| {
            let mgr = maybe.ok_or_else(|| "ModManager unavailable".to_string())?;
            mgr.set_enabled(mod_id, enabled).map_err(|e| e.to_string())
        })
    }

    fn get_region_mods(&self, region_id: &str) -> BTreeMap<String, bool> {
        self.runtime.get_region_mods(region_id)
    }

    fn set_region_mod_enabled(
        &self,
        region_id: &str,
        mod_id: &str,
        enabled: bool,
    ) -> Result<(), String> {
        self.runtime
            .set_region_mod_enabled(region_id, mod_id, enabled)
    }

    fn list_keybinds(&self) -> Vec<KeybindSnapshotEntry> {
        self.runtime.list_keybinds()
    }

    fn register_keybind(&self, desc: KeybindDescriptor) -> Result<String, String> {
        self.runtime.register_keybind(desc)
    }

    fn rebind_keybind(&self, id: &str, chord: Option<&str>) -> Result<Option<String>, String> {
        self.runtime.rebind_keybind(id, chord)
    }

    fn reset_keybind(&self, id: &str) -> Result<(), String> {
        self.runtime.reset_keybind(id)
    }
}

struct ModuleLoaderState {
    scripts: BTreeMap<String, String>,
    loaded: BTreeMap<String, Value>,
    loading: HashSet<String>,
    stack: Vec<String>,
}

/// Embedded sandboxed `mlua` Luau runtime hosting `Simoder-Base` UI composition and `mods-ui`.
pub struct BaseRuntimeHost {
    lua: Mutex<Lua>,
    runtime: Arc<dyn UiStateHost>,
}

impl BaseRuntimeHost {
    /// Load every non-symlink `.luau` script under `base_dir/scripts` and boot `Simoder-Base`.
    pub fn start_from_base_dir(
        base_dir: &Path,
        runtime: Arc<dyn UiStateHost>,
        backend: Arc<dyn ModsActionBackend>,
    ) -> Result<Self, String> {
        let scripts_dir = base_dir.join("scripts");
        let scripts = read_luau_scripts_tree(&scripts_dir)?;
        Self::start_from_script_map(scripts, runtime, backend)
    }

    /// Boot `Simoder-Base` from an explicit canonical map (`"init"`, `"services/ui-manager"`, ...).
    pub fn start_from_script_map(
        scripts: BTreeMap<String, String>,
        runtime: Arc<dyn UiStateHost>,
        backend: Arc<dyn ModsActionBackend>,
    ) -> Result<Self, String> {
        if !scripts.contains_key("init") {
            return Err("Simoder-Base script tree is missing 'init.luau'".into());
        }

        let lua = Lua::new();
        let _ = lua.set_memory_limit(BASE_MEMORY_LIMIT_BYTES);
        install_private_host_table(&lua, runtime.clone(), backend)?;

        let loader = Arc::new(Mutex::new(ModuleLoaderState {
            scripts,
            loaded: BTreeMap::new(),
            loading: HashSet::new(),
            stack: Vec::new(),
        }));
        install_require(&lua, loader.clone())?;
        lua.sandbox(true).map_err(|e| format!("failed enabling Luau sandbox: {e}"))?;

        let init_table = load_module(&lua, &loader, "init")?;
        let init_tbl = into_table(init_table)?;
        let on_enable: Function = init_tbl
            .get("onEnable")
            .map_err(|e| format!("Simoder-Base init.onEnable missing: {e}"))?;
        on_enable
            .call::<()>(())
            .map_err(|e| format!("Simoder-Base init.onEnable failed: {e}"))?;
        lua.globals()
            .set("__simoder_base_exports", init_tbl)
            .map_err(|e| format!("failed storing base exports: {e}"))?;

        Ok(Self {
            lua: Mutex::new(lua),
            runtime,
        })
    }

    /// Execute one UI sync tick (`mods-ui.tick()`) and update composed patches inside `IntegratedSimoderRuntime`.
    pub fn tick_ui(&self) -> Result<(), String> {
        self.call_mods_ui_method("tick")
    }

    /// Force `mods-ui.refresh()` so all active composed patches are re-committed to `IntegratedSimoderRuntime`.
    pub fn refresh_ui(&self) -> Result<(), String> {
        self.call_mods_ui_method("refresh")
    }

    fn call_mods_ui_method(&self, method: &str) -> Result<(), String> {
        let lua = self.lua.lock();
        let exports: Table = lua
            .globals()
            .get("__simoder_base_exports")
            .map_err(|e| format!("missing __simoder_base_exports: {e}"))?;
        let services: Table = exports
            .get("services")
            .map_err(|e| format!("Simoder-Base services not started: {e}"))?;
        let mods_ui: Table = services
            .get("mods-ui")
            .map_err(|e| format!("Simoder-Base service 'mods-ui' missing: {e}"))?;
        let fn_ref: Function = mods_ui
            .get(method)
            .map_err(|e| format!("Simoder-Base mods-ui.{method} missing: {e}"))?;
        fn_ref
            .call::<Value>(())
            .map(|_| ())
            .map_err(|e| format!("Simoder-Base mods-ui.{method} failed: {e}"))
    }

    /// Evaluate a third-party Luau script binding it through `Simoder-Base`'s frozen mod facade (no `host.mods`).
    pub fn run_sandboxed_mod_script(
        &self,
        mod_id: &str,
        generation: u64,
        source: &str,
        entry_fn_name: &str,
    ) -> Result<(), String> {
        let lua = self.lua.lock();
        let exports: Table = lua
            .globals()
            .get("__simoder_base_exports")
            .map_err(|e| format!("missing exports: {e}"))?;
        let services: Table = exports
            .get("services")
            .map_err(|e| format!("missing services: {e}"))?;
        let ui_mgr: Table = services
            .get("ui-manager")
            .map_err(|e| format!("missing ui-manager: {e}"))?;
        let for_mod: Function = ui_mgr
            .get("forMod")
            .map_err(|e| format!("missing ui-manager.forMod: {e}"))?;
        let mod_ui: Table = for_mod
            .call((mod_id, generation))
            .map_err(|e| format!("forMod failed: {e}"))?;

        let game_api = lua.create_table().map_err(|e| e.to_string())?;
        let rt_for_skip = self.runtime.clone();
        let mod_id_for_skip = mod_id.to_string();
        game_api
            .set(
                "setSkipIntro",
                lua.create_function(move |_, enabled: bool| {
                    rt_for_skip.set_mod_skip_intro(&mod_id_for_skip, enabled);
                    Ok(())
                })
                .map_err(|e| e.to_string())?,
            )
            .map_err(|e| e.to_string())?;
        game_api.set_readonly(true);

        let api = lua.create_table().map_err(|e| e.to_string())?;
        api.set("ui", mod_ui).map_err(|e| e.to_string())?;
        api.set("game", game_api).map_err(|e| e.to_string())?;
        api.set_readonly(true);

        let mod_table: Table = lua
            .load(source)
            .set_name(format!("@mod/{mod_id}"))
            .eval()
            .map_err(|e| format!("mod '{mod_id}' load error: {e}"))?;
        let entry: Function = mod_table
            .get(entry_fn_name)
            .map_err(|e| format!("mod '{mod_id}.{entry_fn_name}' missing: {e}"))?;
        entry
            .call::<()>(api)
            .map_err(|e| format!("mod '{mod_id}.{entry_fn_name}' error: {e}"))
    }

    /// Revoke all UI patches and game-action flags owned by `mod_id`.
    pub fn revoke_mod_in_base(&self, mod_id: &str) {
        self.runtime.revoke_mod_game_actions(mod_id);
        let lua = self.lua.lock();
        if let Ok(exports) = lua.globals().get::<Table>("__simoder_base_exports")
            && let Ok(services) = exports.get::<Table>("services")
            && let Ok(ui_mgr) = services.get::<Table>("ui-manager")
            && let Ok(revoke) = ui_mgr.get::<Function>("revokeMod")
        {
            let _ = revoke.call::<()>(mod_id);
        }
    }

    /// Invoke `exports.onDisable()` during clean shutdown.
    pub fn shutdown(&self) {
        let lua = self.lua.lock();
        if let Ok(exports) = lua.globals().get::<Table>("__simoder_base_exports")
            && let Ok(on_disable) = exports.get::<Function>("onDisable")
        {
            let _ = on_disable.call::<()>(());
        }
    }
}

impl Drop for BaseRuntimeHost {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn into_table(val: Value) -> Result<Table, String> {
    match val {
        Value::Table(t) => Ok(t),
        other => Err(format!("expected table module return, found {other:?}")),
    }
}

fn read_luau_scripts_tree(scripts_dir: &Path) -> Result<BTreeMap<String, String>, String> {
    let meta = fs::symlink_metadata(scripts_dir).map_err(|e| {
        format!(
            "Simoder-Base scripts directory unavailable '{}': {e}",
            scripts_dir.display()
        )
    })?;
    if meta.file_type().is_symlink() || !meta.is_dir() {
        return Err(format!(
            "Simoder-Base scripts path '{}' is invalid or a symlink",
            scripts_dir.display()
        ));
    }

    let mut out = BTreeMap::new();
    for entry in WalkDir::new(scripts_dir).follow_links(false) {
        let entry = entry.map_err(|e| format!("walkdir error under scripts: {e}"))?;
        if entry.file_type().is_symlink() {
            return Err(format!(
                "rejected symlink inside Simoder-Base scripts: '{}'",
                entry.path().display()
            ));
        }
        if !entry.file_type().is_file() {
            continue;
        }
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("luau") {
            continue;
        }
        let rel = path
            .strip_prefix(scripts_dir)
            .map_err(|e| format!("failed normalizing script path: {e}"))?;
        let key = rel
            .with_extension("")
            .to_string_lossy()
            .replace('\\', "/");
        let text = fs::read_to_string(path)
            .map_err(|e| format!("failed reading '{}': {e}", path.display()))?;
        out.insert(key, text);
    }
    Ok(out)
}

fn resolve_require_path(caller_key: Option<&str>, request: &str) -> Result<String, String> {
    let normalized = request.trim().replace('\\', "/");
    let unaliased = normalized
        .strip_prefix("@simoder.base/")
        .or_else(|| normalized.strip_prefix("@self/"))
        .unwrap_or(&normalized);
    let stripped = unaliased.strip_suffix(".luau").unwrap_or(unaliased);
    let mut parts: Vec<String> = Vec::new();

    if stripped.starts_with("./") || stripped.starts_with("../") {
        if let Some(caller) = caller_key {
            let mut caller_segs: Vec<&str> = caller.split('/').collect();
            caller_segs.pop();
            parts.extend(caller_segs.into_iter().map(str::to_string));
        }
        for seg in stripped.split('/') {
            match seg {
                "" | "." => {}
                ".." => {
                    if parts.pop().is_none() {
                        return Err(format!("require path escapes scripts root: '{request}'"));
                    }
                }
                name => parts.push(name.to_string()),
            }
        }
    } else {
        for seg in stripped.split('/') {
            if seg.is_empty() || seg == "." || seg == ".." {
                return Err(format!("invalid non-relative require path: '{request}'"));
            }
            parts.push(seg.to_string());
        }
    }

    if parts.is_empty() {
        return Err(format!("empty require path: '{request}'"));
    }
    Ok(parts.join("/"))
}

fn load_module(lua: &Lua, state: &Arc<Mutex<ModuleLoaderState>>, key: &str) -> Result<Value, String> {
    if let Some(cached) = state.lock().loaded.get(key) {
        return Ok(cached.clone());
    }
    let source = {
        let mut st = state.lock();
        if st.loading.contains(key) {
            return Err(format!("cyclic require detected for '{key}'"));
        }
        let Some(src) = st.scripts.get(key).cloned() else {
            return Err(format!("module '{key}' not found in Simoder-Base scripts"));
        };
        st.loading.insert(key.to_string());
        st.stack.push(key.to_string());
        src
    };

    let result = lua
        .load(&source)
        .set_name(format!("@simoder.base/{key}.luau"))
        .eval::<Value>();

    let mut st = state.lock();
    st.stack.pop();
    st.loading.remove(key);
    let val = result.map_err(|e| format!("error loading '{key}.luau': {e}"))?;
    st.loaded.insert(key.to_string(), val.clone());
    Ok(val)
}

fn install_require(lua: &Lua, state: Arc<Mutex<ModuleLoaderState>>) -> Result<(), String> {
    let require_fn = lua
        .create_function(move |lua, req: String| {
            let caller = state.lock().stack.last().cloned();
            let resolved =
                resolve_require_path(caller.as_deref(), &req).map_err(mlua::Error::runtime)?;
            load_module(lua, &state, &resolved).map_err(mlua::Error::runtime)
        })
        .map_err(|e| e.to_string())?;
    lua.globals()
        .set("require", require_fn)
        .map_err(|e| e.to_string())
}
