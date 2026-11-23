//! Isolated Luau engine backend (`mlua`) with memory caps, interrupt deadlines,
//! frozen `simoder` tables, and cycle-checked in-memory module resolution.

use std::sync::Arc;

use mlua::{Function, Lua, RegistryKey, Table, Value, VmState};

use crate::config::SCRIPT_API_VERSION;
use crate::types::{PropCommandAction, ScriptDefinition, ScriptError};
use crate::vm::{
    BudgetGuard, ExecutionBudgetTracker, HostBridge, PropertyKey, validate_finite_f32,
    validate_u32_number,
};

pub(crate) trait LuauResultExt<T> {
    fn map_luau(self, timeout: Option<ScriptError>) -> Result<T, ScriptError>;
}

impl<T> LuauResultExt<T> for Result<T, mlua::Error> {
    fn map_luau(self, timeout: Option<ScriptError>) -> Result<T, ScriptError> {
        self.map_err(|err| map_luau_error(err, timeout))
    }
}

pub(crate) struct LuauBackend {
    lua: Lua,
    on_tick: Option<RegistryKey>,
    on_disable: Option<RegistryKey>,
}

impl LuauBackend {
    pub(crate) fn spawn(
        bridge: &Arc<HostBridge>,
        definition: &ScriptDefinition,
        budget: &Arc<ExecutionBudgetTracker>,
    ) -> Result<Self, ScriptError> {
        let lua = Lua::new();
        install_environment(&lua, bridge, budget, &definition.entry)?;
        lua.sandbox(true).map_luau(None)?;
        lua.set_memory_limit(definition.limits.memory_bytes).map_luau(None)?;

        let (on_enable, on_tick, on_disable) = {
            let guard = budget.arm(definition.limits.budget_ms);
            let cmd_guard = bridge.begin_callback();
            let lifecycle_val: Value = lua
                .load(&definition.entry_code)
                .set_name(format!("@{}", definition.entry))
                .eval()
                .map_luau(guard.into_timeout_error())?;
            let lifecycle = lifecycle_val.as_table().ok_or_else(|| {
                ScriptError::InvocationFailed("Script entry must return a lifecycle table".into())
            })?;
            let enable = extract_callback(&lua, lifecycle, "onEnable")?;
            let tick = extract_callback(&lua, lifecycle, "onTick")?;
            let disable = extract_callback(&lua, lifecycle, "onDisable")?;
            cmd_guard.keep();
            (enable, tick, disable)
        };

        if let Some(enable_key) = on_enable {
            let guard = budget.arm(definition.limits.budget_ms);
            let cmd_guard = bridge.begin_callback();
            let func: Function = lua.registry_value(&enable_key).map_luau(None)?;
            func.call::<()>(()).map_luau(guard.into_timeout_error())?;
            let _ = lua.remove_registry_value(enable_key);
            cmd_guard.keep();
        }

        Ok(Self {
            lua,
            on_tick,
            on_disable,
        })
    }

    pub(crate) fn call_tick(&self, dt: f64, guard: &BudgetGuard<'_>) -> Result<(), ScriptError> {
        let Some(key) = &self.on_tick else {
            return Ok(());
        };
        let func: Function = self.lua.registry_value(key).map_luau(None)?;
        func.call::<()>(dt).map_luau(guard.into_timeout_error())
    }

    pub(crate) fn call_disable(&mut self, guard: &BudgetGuard<'_>) -> Result<(), ScriptError> {
        let Some(key) = self.on_disable.take() else {
            return Ok(());
        };
        let res = self
            .lua
            .registry_value::<Function>(&key)
            .and_then(|func| func.call::<()>(()))
            .map_luau(guard.into_timeout_error());
        let _ = self.lua.remove_registry_value(key);
        res
    }

    pub(crate) fn release_keys(&mut self) {
        if let Some(key) = self.on_tick.take() {
            let _ = self.lua.remove_registry_value(key);
        }
        if let Some(key) = self.on_disable.take() {
            let _ = self.lua.remove_registry_value(key);
        }
    }

    pub(crate) fn memory_bytes(&self) -> usize {
        self.lua.used_memory()
    }
}

pub(crate) fn install_luau_interrupt(lua: &Lua, budget: &Arc<ExecutionBudgetTracker>) {
    let budget_int = Arc::clone(budget);
    lua.set_interrupt(move |_| {
        if let Some((budget_ms, elapsed_us)) = budget_int.poll_exceeded() {
            return Err(mlua::Error::external(ScriptError::BudgetExceeded {
                budget_ms,
                elapsed_us,
            }));
        }
        Ok(VmState::Continue)
    });
}

pub(crate) fn strip_luau_unsafe_globals(lua: &Lua) -> Result<(), ScriptError> {
    let globals = lua.globals();
    for forbidden in ["getfenv", "setfenv", "debug", "loadstring", "require"] {
        globals.set(forbidden, Value::Nil).map_luau(None)?;
    }
    Ok(())
}

fn install_environment(
    lua: &Lua,
    bridge: &Arc<HostBridge>,
    budget: &Arc<ExecutionBudgetTracker>,
    entry_path: &str,
) -> Result<(), ScriptError> {
    install_luau_interrupt(lua, budget);
    strip_luau_unsafe_globals(lua)?;
    let globals = lua.globals();

    let simoder = lua.create_table().map_luau(None)?;
    simoder.set("apiVersion", SCRIPT_API_VERSION).map_luau(None)?;
    simoder.set("modId", bridge.identity.mod_id.as_str()).map_luau(None)?;

    let log_bridge = Arc::clone(bridge);
    let log_fn = lua
        .create_function(move |_, msg: mlua::LuaString| {
            log_bridge.log_message(&msg.to_str()?).map_err(mlua::Error::external)
        })
        .map_luau(None)?;
    simoder.set("log", log_fn.clone()).map_luau(None)?;
    globals.set("print", log_fn).map_luau(None)?;

    let time_bridge = Arc::clone(bridge);
    let time_fn = lua.create_function(move |_, ()| Ok(time_bridge.elapsed_seconds())).map_luau(None)?;
    simoder.set("time", time_fn).map_luau(None)?;

    let cap_bridge = Arc::clone(bridge);
    let cap_fn = lua
        .create_function(move |_, name: mlua::LuaString| Ok(cap_bridge.has_capability(&name.to_str()?)))
        .map_luau(None)?;
    simoder.set("hasCapability", cap_fn).map_luau(None)?;

    if !bridge.bootstrap_base {
        let props = lua.create_table().map_luau(None)?;
        install_props(lua, &props, bridge)?;
        props.set_readonly(true);
        simoder.set("props", props).map_luau(None)?;
    }
    simoder.set_readonly(true);
    globals.set("simoder", simoder).map_luau(None)?;

    let cache = Arc::new(lua.create_registry_value(lua.create_table().map_luau(None)?).map_luau(None)?);
    let require_fn = build_require(lua, Arc::clone(bridge), entry_path.to_string(), cache)?;
    globals.set("require", require_fn).map_luau(None)?;
    Ok(())
}

fn install_props(lua: &Lua, props: &Table, bridge: &Arc<HostBridge>) -> Result<(), ScriptError> {
    let get_b = Arc::clone(bridge);
    props
        .set(
            "getFloat",
            lua.create_function(move |lua, (res, prop): (mlua::LuaString, Value)| {
                let obs = get_b
                    .get_float(&res.to_str()?, parse_prop_key(prop)?)
                    .map_err(mlua::Error::external)?;
                let Some(obs) = obs else { return Ok(Value::Nil); };
                let tbl = lua.create_table()?;
                tbl.set("value", f64::from(obs.value))?;
                tbl.set("vanilla", f64::from(obs.vanilla))?;
                tbl.set("observedAtUnixMs", obs.observed_at_unix_ms as f64)?;
                tbl.set("generation", obs.generation as f64)?;
                tbl.set("instances", obs.instances)?;
                tbl.set("status", obs.status)?;
                tbl.set("historical", true)?;
                tbl.set_readonly(true);
                Ok(Value::Table(tbl))
            })
            .map_luau(None)?,
        )
        .map_luau(None)?;

    let set_b = Arc::clone(bridge);
    props
        .set(
            "setFloat",
            lua.create_function(move |_, (res, prop, val): (mlua::LuaString, Value, f64)| {
                let num = validate_finite_f32(val).map_err(mlua::Error::external)?;
                set_b
                    .queue_prop_command(&res.to_str()?, parse_prop_key(prop)?, PropCommandAction::SetFloat(num))
                    .map_err(mlua::Error::external)
            })
            .map_luau(None)?,
        )
        .map_luau(None)?;

    let clr_b = Arc::clone(bridge);
    props
        .set(
            "clearFloat",
            lua.create_function(move |_, (res, prop): (mlua::LuaString, Value)| {
                clr_b
                    .queue_prop_command(&res.to_str()?, parse_prop_key(prop)?, PropCommandAction::ClearFloat)
                    .map_err(mlua::Error::external)
            })
            .map_luau(None)?,
        )
        .map_luau(None)?;
    Ok(())
}

fn build_require(
    lua: &Lua,
    bridge: Arc<HostBridge>,
    caller_path: String,
    cache_key: Arc<RegistryKey>,
) -> Result<Function, ScriptError> {
    lua.create_function(move |lua, requested: mlua::LuaString| {
        let resolved = bridge
            .resolve_module_path(&caller_path, &requested.to_str()?, &["luau", "lua"], ".luau")
            .map_err(mlua::Error::external)?;
        let cache: Table = lua.registry_value(&cache_key)?;
        let cached: Value = cache.get(resolved.as_str())?;
        if !cached.is_nil() {
            return Ok(cached);
        }

        let _cycle_guard = bridge.begin_module_load(&resolved).map_err(mlua::Error::external)?;
        let code = bridge
            .modules
            .get(&resolved)
            .ok_or_else(|| mlua::Error::external(ScriptError::ModuleError(format!("module '{resolved}' missing"))))?
            .clone();

        let env = lua.create_table()?;
        env.set(
            "require",
            build_require(lua, Arc::clone(&bridge), resolved.clone(), Arc::clone(&cache_key))
                .map_err(mlua::Error::external)?,
        )?;
        let mt = lua.create_table()?;
        mt.set("__index", lua.globals())?;
        mt.set_readonly(true);
        env.set_metatable(Some(mt))?;

        let value: Value = lua.load(&code).set_name(format!("@{resolved}")).set_environment(env).eval()?;
        if value.is_nil() {
            return Err(mlua::Error::external(ScriptError::ModuleError(format!(
                "Luau module '{resolved}' must return a non-nil value"
            ))));
        }
        cache.set(resolved.as_str(), value.clone())?;
        Ok(value)
    })
    .map_luau(None)
}

fn parse_prop_key(value: Value) -> Result<PropertyKey, mlua::Error> {
    match value {
        Value::Integer(i) => u32::try_from(i)
            .map(PropertyKey::Numeric)
            .map_err(|_| mlua::Error::external(ScriptError::PropApiError("Property ID must be exact uint32".into()))),
        Value::Number(n) => validate_u32_number(n).map(PropertyKey::Numeric).map_err(mlua::Error::external),
        Value::String(s) => Ok(PropertyKey::Text(s.to_str()?.to_string())),
        _ => Err(mlua::Error::external(ScriptError::PropApiError(
            "Property argument must be numeric uint32 or string alias/hex".into(),
        ))),
    }
}

fn extract_callback(lua: &Lua, lifecycle: &Table, name: &str) -> Result<Option<RegistryKey>, ScriptError> {
    match lifecycle.get::<Value>(name).map_luau(None)? {
        Value::Nil => Ok(None),
        Value::Function(func) => lua.create_registry_value(func).map(Some).map_luau(None),
        _ => Err(ScriptError::InvocationFailed(format!(
            "lifecycle field '{name}' must be a function when provided"
        ))),
    }
}

pub(crate) fn map_luau_error(err: mlua::Error, timeout_override: Option<ScriptError>) -> ScriptError {
    if let Some(timeout) = timeout_override {
        return timeout;
    }
    if let Some(inner) = err.downcast_ref::<ScriptError>() {
        return inner.clone();
    }
    match err {
        mlua::Error::MemoryError(msg) => ScriptError::MemoryLimitExceeded(msg),
        other => ScriptError::InvocationFailed(other.to_string()),
    }
}
