//! Isolated ECMAScript engine backend (`rquickjs` QuickJS-NG) with heap/stack caps,
//! single-shot interrupt registration, frozen `simoder` APIs, and CommonJS `require`.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use parking_lot::Mutex;
use rquickjs::function::Constructor;
use rquickjs::{Context, Ctx, Exception, Function, Object, Runtime, Value};

use crate::config::SCRIPT_API_VERSION;
use crate::types::{PropCommandAction, ScriptDefinition, ScriptError};
use crate::vm::{
    BudgetGuard, ExecutionBudgetTracker, HostBridge, PropertyKey, validate_finite_f32,
    validate_u32_number,
};

const BAG_LIFECYCLE: &str = "__simoder_lifecycle";
const BAG_CACHE: &str = "__simoder_module_cache";

type LastHostError = Arc<Mutex<Option<(String, ScriptError)>>>;

trait JsResultExt<T> {
    fn map_js(self, ctx: &Ctx<'_>, host_err: &LastHostError) -> Result<T, ScriptError>;
}

impl<T> JsResultExt<T> for rquickjs::Result<T> {
    fn map_js(self, ctx: &Ctx<'_>, host_err: &LastHostError) -> Result<T, ScriptError> {
        self.map_err(|err| map_ctx_error(ctx, err, None, host_err))
    }
}

pub(crate) struct JsBackend {
    // Context (and its userdata) must drop before Runtime.
    context: Context,
    runtime: Runtime,
    has_on_tick: bool,
    has_on_disable: bool,
    interrupted: Arc<AtomicBool>,
    last_host_error: LastHostError,
}

impl JsBackend {
    pub(crate) fn spawn(
        bridge: &Arc<HostBridge>,
        definition: &ScriptDefinition,
        budget: &Arc<ExecutionBudgetTracker>,
    ) -> Result<Self, ScriptError> {
        let last_host_error = Arc::new(Mutex::new(None));
        let runtime = Runtime::new()
            .map_err(|err| ScriptError::MemoryLimitExceeded(format!("failed to create QuickJS runtime: {err}")))?;
        runtime.set_max_stack_size(definition.limits.js_max_stack_bytes);

        let interrupted = Arc::new(AtomicBool::new(false));
        let budget_int = Arc::clone(budget);
        let flag_int = Arc::clone(&interrupted);
        // Registered once per VM; armed/disarmed lock-free via ExecutionBudgetTracker.
        runtime.set_interrupt_handler(Some(Box::new(move || {
            if budget_int.poll_exceeded().is_some() {
                flag_int.store(true, Ordering::Release);
                return true;
            }
            false
        })));

        let context = Context::full(&runtime)
            .map_err(|err| ScriptError::MemoryLimitExceeded(format!("failed to create QuickJS context: {err}")))?;

        context.with(|ctx| -> Result<(), ScriptError> {
            let bag = Object::new(ctx.clone()).map_js(&ctx, &last_host_error)?;
            let cache = Object::new_proto(ctx.clone(), None).map_js(&ctx, &last_host_error)?;
            bag.set(BAG_CACHE, cache).map_js(&ctx, &last_host_error)?;
            ctx.store_userdata(bag)
                .map_err(|_| ScriptError::SchedulerError("failed to bind QuickJS userdata".into()))?;

            install_environment(&ctx, bridge, &last_host_error)
        })?;

        // Set memory limit outside `context.with` to avoid re-entrant `Runtime` lock under `parallel`.
        runtime.set_memory_limit(definition.limits.memory_bytes);

        let (has_enable, has_on_tick, has_on_disable) = context.with(|ctx| -> Result<(bool, bool, bool), ScriptError> {
            interrupted.store(false, Ordering::Release);
            *last_host_error.lock() = None;
            let guard = budget.arm(definition.limits.budget_ms);
            let cmd_guard = bridge.begin_callback();

            let req = build_require(&ctx, Arc::clone(bridge), definition.entry.clone(), Arc::clone(&last_host_error))?;
            let exports = eval_commonjs_module(&ctx, &definition.entry, &definition.entry_code, req)
                .map_err(|err| finish_js_error(&ctx, err, &guard, &interrupted, &last_host_error))?;
            *last_host_error.lock() = None;

            let lifecycle = exports.into_object().ok_or_else(|| {
                ScriptError::InvocationFailed("JS script entry must export a lifecycle object".into())
            })?;
            let has_e = has_js_fn(&ctx, &lifecycle, "onEnable", &last_host_error)?;
            let has_t = has_js_fn(&ctx, &lifecycle, "onTick", &last_host_error)?;
            let has_d = has_js_fn(&ctx, &lifecycle, "onDisable", &last_host_error)?;

            if let Some(bag_guard) = ctx.userdata::<Object>() {
                bag_guard.set(BAG_LIFECYCLE, lifecycle).map_js(&ctx, &last_host_error)?;
            }
            cmd_guard.keep();
            Ok((has_e, has_t, has_d))
        })?;

        let backend = Self {
            context,
            runtime,
            has_on_tick,
            has_on_disable,
            interrupted,
            last_host_error,
        };
        if has_enable {
            let guard = budget.arm(definition.limits.budget_ms);
            let cmd_guard = bridge.begin_callback();
            backend.call_lifecycle("onEnable", (), &guard)?;
            cmd_guard.keep();
        }
        Ok(backend)
    }

    pub(crate) fn call_tick(&self, dt: f64, guard: &BudgetGuard<'_>) -> Result<(), ScriptError> {
        if !self.has_on_tick {
            return Ok(());
        }
        self.call_lifecycle("onTick", (dt,), guard)
    }

    pub(crate) fn call_disable(&self, guard: &BudgetGuard<'_>) -> Result<(), ScriptError> {
        if !self.has_on_disable {
            return Ok(());
        }
        self.call_lifecycle("onDisable", (), guard)
    }

    fn call_lifecycle<'js, A>(&self, name: &str, args: A, guard: &BudgetGuard<'_>) -> Result<(), ScriptError>
    where
        A: for<'a> rquickjs::function::IntoArgs<'a>,
    {
        self.interrupted.store(false, Ordering::Release);
        *self.last_host_error.lock() = None;
        self.context.with(|ctx| {
            let func = get_lifecycle_fn(&ctx, name, &self.last_host_error)?
                .ok_or_else(|| ScriptError::InvocationFailed(format!("missing '{name}' export")))?;
            match func.call::<_, Value>(args) {
                Ok(_) => {
                    *self.last_host_error.lock() = None;
                    Ok(())
                }
                Err(err) => Err(finish_js_error(&ctx, err, guard, &self.interrupted, &self.last_host_error)),
            }
        })
    }

    pub(crate) fn release_userdata(&self) {
        self.context.with(|ctx| {
            let _ = ctx.remove_userdata::<Object>();
        });
        self.runtime.run_gc();
    }

    pub(crate) fn memory_bytes(&self) -> usize {
        self.runtime.memory_usage().malloc_size.max(0) as usize
    }
}

impl Drop for JsBackend {
    fn drop(&mut self) {
        self.release_userdata();
    }
}

fn install_environment<'js>(
    ctx: &Ctx<'js>,
    bridge: &Arc<HostBridge>,
    last_err: &LastHostError,
) -> Result<(), ScriptError> {
    let globals = ctx.globals();
    let simoder = Object::new(ctx.clone()).map_js(ctx, last_err)?;
    simoder.prop("apiVersion", SCRIPT_API_VERSION).map_js(ctx, last_err)?;
    simoder.prop("modId", bridge.identity.mod_id.as_str()).map_js(ctx, last_err)?;

    let log_b = Arc::clone(bridge);
    let log_e = Arc::clone(last_err);
    let log_fn = Function::new(ctx.clone(), move |ctx: Ctx<'_>, msg: String| {
        log_b.log_message(&msg).map_err(|e| throw_host_error(&ctx, e, &log_e))
    })
    .map_js(ctx, last_err)?;
    simoder.prop("log", log_fn.clone()).map_js(ctx, last_err)?;
    globals.set("print", log_fn.clone()).map_js(ctx, last_err)?;

    let console = Object::new(ctx.clone()).map_js(ctx, last_err)?;
    console.prop("log", log_fn).map_js(ctx, last_err)?;
    freeze_js_object(ctx, &console, last_err)?;
    globals.set("console", console).map_js(ctx, last_err)?;

    let time_b = Arc::clone(bridge);
    simoder
        .prop("time", Function::new(ctx.clone(), move || time_b.elapsed_seconds()).map_js(ctx, last_err)?)
        .map_js(ctx, last_err)?;

    let cap_b = Arc::clone(bridge);
    simoder
        .prop(
            "hasCapability",
            Function::new(ctx.clone(), move |name: String| cap_b.has_capability(&name)).map_js(ctx, last_err)?,
        )
        .map_js(ctx, last_err)?;

    if !bridge.bootstrap_base {
        let props = Object::new(ctx.clone()).map_js(ctx, last_err)?;
        install_props(ctx, &props, bridge, last_err)?;
        freeze_js_object(ctx, &props, last_err)?;
        simoder.prop("props", props).map_js(ctx, last_err)?;
    }

    freeze_js_object(ctx, &simoder, last_err)?;
    globals.prop("simoder", simoder).map_js(ctx, last_err)
}

fn install_props<'js>(
    ctx: &Ctx<'js>,
    props: &Object<'js>,
    bridge: &Arc<HostBridge>,
    last_err: &LastHostError,
) -> Result<(), ScriptError> {
    let get_b = Arc::clone(bridge);
    let get_e = Arc::clone(last_err);
    props
        .prop(
            "getFloat",
            Function::new(ctx.clone(), move |ctx: Ctx<'js>, res: String, prop: Value<'js>| -> rquickjs::Result<Value<'js>> {
                let key = parse_prop_key(&ctx, prop, &get_e)?;
                let obs = get_b.get_float(&res, key).map_err(|e| throw_host_error(&ctx, e, &get_e))?;
                let Some(obs) = obs else { return Ok(Value::new_null(ctx)); };
                let obj = Object::new(ctx.clone())?;
                obj.prop("value", f64::from(obs.value))?;
                obj.prop("vanilla", f64::from(obs.vanilla))?;
                obj.prop("observedAtUnixMs", obs.observed_at_unix_ms as f64)?;
                obj.prop("generation", obs.generation as f64)?;
                obj.prop("instances", obs.instances)?;
                obj.prop("status", obs.status)?;
                obj.prop("historical", true)?;
                if let Ok(ctor) = ctx.globals().get::<_, Object>("Object")
                    && let Ok(freeze) = ctor.get::<_, Function>("freeze")
                {
                    let _: Value = freeze.call((obj.clone(),))?;
                }
                Ok(obj.into_value())
            })
            .map_js(ctx, last_err)?,
        )
        .map_js(ctx, last_err)?;

    let set_b = Arc::clone(bridge);
    let set_e = Arc::clone(last_err);
    props
        .prop(
            "setFloat",
            Function::new(ctx.clone(), move |ctx: Ctx<'js>, res: String, prop: Value<'js>, val: f64| {
                let key = parse_prop_key(&ctx, prop, &set_e)?;
                let num = validate_finite_f32(val).map_err(|e| throw_host_error(&ctx, e, &set_e))?;
                set_b
                    .queue_prop_command(&res, key, PropCommandAction::SetFloat(num))
                    .map_err(|e| throw_host_error(&ctx, e, &set_e))
            })
            .map_js(ctx, last_err)?,
        )
        .map_js(ctx, last_err)?;

    let clr_b = Arc::clone(bridge);
    let clr_e = Arc::clone(last_err);
    props
        .prop(
            "clearFloat",
            Function::new(ctx.clone(), move |ctx: Ctx<'js>, res: String, prop: Value<'js>| -> rquickjs::Result<()> {
                let key = parse_prop_key(&ctx, prop, &clr_e)?;
                clr_b
                    .queue_prop_command(&res, key, PropCommandAction::ClearFloat)
                    .map_err(|e| throw_host_error(&ctx, e, &clr_e))
            })
            .map_js(ctx, last_err)?,
        )
        .map_js(ctx, last_err)
}

fn build_require<'js>(
    ctx: &Ctx<'js>,
    bridge: Arc<HostBridge>,
    caller_path: String,
    last_err: LastHostError,
) -> Result<Function<'js>, ScriptError> {
    let err_slot = Arc::clone(&last_err);
    Function::new(ctx.clone(), move |ctx: Ctx<'js>, requested: String| -> rquickjs::Result<Value<'js>> {
        let resolved = bridge
            .resolve_module_path(&caller_path, &requested, &["js", "mjs", "cjs"], ".js")
            .map_err(|e| throw_host_error(&ctx, e, &err_slot))?;
        let cache_obj: Option<Object<'js>> = ctx.userdata::<Object>().and_then(|bag| bag.get(BAG_CACHE).ok());
        if let Some(cache) = &cache_obj
            && cache.contains_key(resolved.as_str())?
        {
            return cache.get(resolved.as_str());
        }

        let _cycle = bridge.begin_module_load(&resolved).map_err(|e| throw_host_error(&ctx, e, &err_slot))?;
        let code = bridge
            .modules
            .get(&resolved)
            .cloned()
            .ok_or_else(|| throw_host_error(&ctx, ScriptError::ModuleError(format!("JS module '{resolved}' missing")), &err_slot))?;

        let child_req = build_require(&ctx, Arc::clone(&bridge), resolved.clone(), Arc::clone(&err_slot))
            .map_err(|e| throw_host_error(&ctx, e, &err_slot))?;
        let exports = eval_commonjs_module(&ctx, &resolved, &code, child_req)?;
        if let Some(cache) = cache_obj {
            cache.set(resolved.as_str(), exports.clone())?;
        }
        Ok(exports)
    })
    .map_js(ctx, &last_err)
}

fn eval_commonjs_module<'js>(
    ctx: &Ctx<'js>,
    path: &str,
    source: &str,
    require_fn: Function<'js>,
) -> rquickjs::Result<Value<'js>> {
    let exports_obj = Object::new(ctx.clone())?;
    let module_obj = Object::new(ctx.clone())?;
    module_obj.set("exports", exports_obj.clone())?;
    let fn_ctor: Constructor = ctx.globals().get("Function")?;
    let body = format!("'use strict';\n{source}\n//# sourceURL={path}\n");
    let module_fn: Function = fn_ctor.construct(("exports", "require", "module", body))?;
    let _: Value = module_fn.call((exports_obj, require_fn, module_obj.clone()))?;
    module_obj.get("exports")
}

fn has_js_fn<'js>(
    ctx: &Ctx<'js>,
    lifecycle: &Object<'js>,
    name: &str,
    last_err: &LastHostError,
) -> Result<bool, ScriptError> {
    let val: Value = lifecycle.get(name).map_js(ctx, last_err)?;
    if val.is_undefined() || val.is_null() {
        return Ok(false);
    }
    if !val.is_function() {
        return Err(ScriptError::InvocationFailed(format!(
            "JS lifecycle export '{name}' must be a function when provided"
        )));
    }
    Ok(true)
}

fn get_lifecycle_fn<'js>(
    ctx: &Ctx<'js>,
    name: &str,
    last_err: &LastHostError,
) -> Result<Option<Function<'js>>, ScriptError> {
    let Some(bag) = ctx.userdata::<Object>() else {
        return Err(ScriptError::RuntimeClosed("JS runtime userdata already released".into()));
    };
    let lifecycle: Object = bag.get(BAG_LIFECYCLE).map_js(ctx, last_err)?;
    drop(bag);
    let val: Value = lifecycle.get(name).map_js(ctx, last_err)?;
    if val.is_undefined() || val.is_null() {
        return Ok(None);
    }
    val.into_function()
        .map(Some)
        .ok_or_else(|| ScriptError::InvocationFailed(format!("JS export '{name}' is not a function")))
}

fn freeze_js_object<'js>(
    ctx: &Ctx<'js>,
    obj: &Object<'js>,
    last_err: &LastHostError,
) -> Result<(), ScriptError> {
    let obj_ctor: Object = ctx.globals().get("Object").map_js(ctx, last_err)?;
    let freeze_fn: Function = obj_ctor.get("freeze").map_js(ctx, last_err)?;
    let _: Value = freeze_fn.call((obj.clone(),)).map_js(ctx, last_err)?;
    Ok(())
}

fn parse_prop_key<'js>(
    ctx: &Ctx<'js>,
    prop: Value<'js>,
    last_err: &LastHostError,
) -> rquickjs::Result<PropertyKey> {
    if let Some(num) = prop.as_number() {
        return validate_u32_number(num)
            .map(PropertyKey::Numeric)
            .map_err(|e| throw_host_error(ctx, e, last_err));
    }
    if let Some(text) = prop.as_string().and_then(|s| s.to_string().ok()) {
        return Ok(PropertyKey::Text(text));
    }
    Err(throw_host_error(
        ctx,
        ScriptError::PropApiError("Property argument must be numeric uint32 or string alias/hex".into()),
        last_err,
    ))
}

fn throw_host_error<'js>(
    ctx: &Ctx<'js>,
    err: ScriptError,
    last_err: &LastHostError,
) -> rquickjs::Error {
    let msg = err.to_string();
    *last_err.lock() = Some((msg.clone(), err));
    Exception::throw_message(ctx, &msg)
}

fn finish_js_error(
    ctx: &Ctx<'_>,
    err: rquickjs::Error,
    guard: &BudgetGuard<'_>,
    interrupted: &AtomicBool,
    last_err: &LastHostError,
) -> ScriptError {
    let timeout = guard.into_timeout_error();
    if interrupted.swap(false, Ordering::AcqRel)
        && let Some(budget_err) = timeout
    {
        if ctx.has_exception() {
            let _ = ctx.catch();
        }
        *last_err.lock() = None;
        return budget_err;
    }
    map_ctx_error(ctx, err, timeout, last_err)
}

fn map_ctx_error(
    ctx: &Ctx<'_>,
    err: rquickjs::Error,
    timeout: Option<ScriptError>,
    last_err: &LastHostError,
) -> ScriptError {
    if let Some(budget_err) = timeout {
        if ctx.has_exception() {
            let _ = ctx.catch();
        }
        *last_err.lock() = None;
        return budget_err;
    }
    let pending = (matches!(err, rquickjs::Error::Exception) || ctx.has_exception()).then(|| {
        let caught = ctx.catch();
        if let Some(exc) = caught.as_exception() {
            exc.to_string()
        } else if let Some(s) = caught.as_string().and_then(|s| s.to_string().ok()) {
            s
        } else {
            format!("{caught:?}")
        }
    });

    if matches!(err, rquickjs::Error::Allocation)
        || pending
            .as_deref()
            .is_some_and(|m| m.to_ascii_lowercase().contains("out of memory"))
    {
        *last_err.lock() = None;
        return ScriptError::MemoryLimitExceeded(
            pending.unwrap_or_else(|| "QuickJS heap allocation limit exceeded".into()),
        );
    }

    if let Some((msg, host_err)) = last_err.lock().take()
        && pending.as_deref().is_some_and(|p| p.contains(&msg))
    {
        return host_err;
    }
    if pending
        .as_deref()
        .is_some_and(|m| m.to_ascii_lowercase().contains("interrupted"))
    {
        return ScriptError::BudgetExceeded {
            budget_ms: 0,
            elapsed_us: 0,
        };
    }

    ScriptError::InvocationFailed(pending.unwrap_or_else(|| err.to_string()))
}
