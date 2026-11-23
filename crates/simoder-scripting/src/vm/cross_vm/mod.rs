//! Real multi-VM Luau synchronous cross-VM hook coordinator (`PreHook`, `OverrideFunction`,
//! `PostHook`, `GetFunction`, `GetReturn`), provenance tracking, and chain-deadline enforcement.

pub mod proxy;

use std::collections::{BTreeMap, HashMap};
use std::sync::Arc;
use std::time::{Duration, Instant};

use mlua::{Function, Lua, MultiValue, RegistryKey, Table, Value};
use parking_lot::Mutex;
use slotmap::SlotMap;

use crate::config::MAX_CROSS_VM_FANOUT;
use crate::hooks::{
    FunctionHandleKey, GetFunctionHandle, HookApproval, HookPermissionScope,
    HookPolicyRegistry, HookReturnTuple, HookTargetId, HookValue, InvocationFrameStack,
    OverrideConflictReport, OverrideWinner, PostHookOutcome, ProxyObjectKey,
};
use crate::types::{ModIdentity, ScriptError, ScriptLimits, VmGeneration};
use crate::vm::ExecutionBudgetTracker;
use crate::vm::luau::{LuauResultExt as _, install_luau_interrupt, strip_luau_unsafe_globals};

pub use proxy::ProxyArena;

use proxy::{
    hook_value_to_lua_for_caller, hook_value_to_lua_in_owning_vm, hook_value_to_plain_lua,
    lua_multivalue_to_tuple, lua_value_to_hook, project_lua_table_to_proxy,
    sync_arena_to_lua_tables, sync_lua_tables_to_arena,
};

struct ManagedHookVm {
    identity: ModIdentity,
    lua: Lua,
    budget: Arc<ExecutionBudgetTracker>,
    budget_ms: u32,
    exports: BTreeMap<String, RegistryKey>,
    live_tables: BTreeMap<String, RegistryKey>,
}

#[derive(Clone)]
struct RegisteredCallback {
    owner: ModIdentity,
    fn_key: Arc<RegistryKey>,
}

#[derive(Clone, Copy)]
enum HookPhase {
    Pre,
    Override,
    Post,
}

impl HookPhase {
    const fn required_scope(self) -> HookPermissionScope {
        match self {
            Self::Pre | Self::Post => HookPermissionScope::ObserveOnly,
            Self::Override => HookPermissionScope::OverrideBody,
        }
    }

    const fn label(self) -> &'static str {
        match self {
            Self::Pre => "PreHook",
            Self::Override => "Override",
            Self::Post => "PostHook",
        }
    }
}

pub(crate) struct CrossVmInner {
    vms: BTreeMap<String, ManagedHookVm>,
    pub(crate) policies: HookPolicyRegistry,
    pub(crate) arena: ProxyArena,
    pub(crate) frames: InvocationFrameStack,
    pre_hooks: BTreeMap<HookTargetId, Vec<RegisteredCallback>>,
    overrides: BTreeMap<HookTargetId, Vec<RegisteredCallback>>,
    post_hooks: BTreeMap<HookTargetId, Vec<RegisteredCallback>>,
    get_fn_handles: SlotMap<FunctionHandleKey, RegisteredCallback>,
}

/// Argument specification passed into synchronous cross-VM invocation.
#[derive(Debug, Clone, PartialEq)]
pub enum CrossVmCallArg {
    /// Pass a plain scalar/JSON value directly.
    Value(HookValue),
    /// Pass the live exported Lua table `export_name` from the target mod's own VM via controlled
    /// alias-preserving mutable [`ForeignObjectProxy`](proxy) across foreign hook VMs.
    TargetExportedTable(String),
}

struct FrameGuard<'a> {
    state: &'a Arc<Mutex<CrossVmInner>>,
    frame_id: u64,
}

impl Drop for FrameGuard<'_> {
    fn drop(&mut self) {
        let mut inner = self.state.lock();
        inner.arena.retire_frame(self.frame_id);
        inner.frames.pop_frame();
    }
}

/// Isolated multi-VM Luau synchronous hook coordinator backing Beta cross-VM contracts.
pub struct CrossVmHookHub {
    state: Arc<Mutex<CrossVmInner>>,
}

impl Default for CrossVmHookHub {
    fn default() -> Self {
        Self::new()
    }
}

impl CrossVmHookHub {
    /// Construct a fresh isolated multi-VM cross-hook coordinator.
    pub fn new() -> Self {
        Self {
            state: Arc::new(Mutex::new(CrossVmInner {
                vms: BTreeMap::new(),
                policies: HookPolicyRegistry::default(),
                arena: ProxyArena::default(),
                frames: InvocationFrameStack::default(),
                pre_hooks: BTreeMap::new(),
                overrides: BTreeMap::new(),
                post_hooks: BTreeMap::new(),
                get_fn_handles: SlotMap::with_key(),
            })),
        }
    }

    /// Configure deterministic mod load order for override conflict resolution and hook ordering.
    pub fn set_mod_load_order(&self, order: Vec<String>) -> Result<(), ScriptError> {
        self.state.lock().policies.set_mod_load_order(order);
        Ok(())
    }

    /// Grant explicit user hook approval for `approval.caller_mod_id`.
    pub fn grant_approval(&self, approval: HookApproval) -> Result<(), ScriptError> {
        self.state.lock().policies.grant_approval(approval);
        Ok(())
    }

    /// Revoke user hook approval for `caller_mod_id` on `target` (enforced immediately on live use).
    pub fn revoke_approval(&self, caller_mod_id: &str, target: &HookTargetId) -> Result<(), ScriptError> {
        self.state.lock().policies.revoke_approval(caller_mod_id, target);
        Ok(())
    }

    /// Create an isolated real Luau VM for `mod_id` at `generation`, compiling its exported module table.
    pub fn spawn_vm(
        &self,
        mod_id: &str,
        generation: VmGeneration,
        limits: ScriptLimits,
        module_source: &str,
    ) -> Result<ModIdentity, ScriptError> {
        limits.validate()?;
        let identity = ModIdentity::new(mod_id, generation)?;
        let lua = Lua::new();
        let budget = ExecutionBudgetTracker::new();

        install_luau_interrupt(&lua, &budget);
        strip_luau_unsafe_globals(&lua)?;
        install_cross_vm_host_api(&lua, &identity, &self.state)?;
        lua.sandbox(true).map_luau(None)?;
        lua.set_memory_limit(limits.memory_bytes).map_luau(None)?;

        let exported_val: Value = {
            let guard = budget.arm(limits.budget_ms);
            lua.load(module_source)
                .set_name(format!("@{mod_id}/main.luau"))
                .eval()
                .map_luau(guard.into_timeout_error())?
        };
        let Some(exported_tbl) = exported_val.as_table() else {
            return Err(ScriptError::InvocationFailed(format!(
                "mod '{mod_id}' must return a module table"
            )));
        };

        let mut exports = BTreeMap::new();
        let mut live_tables = BTreeMap::new();
        for pair in exported_tbl.pairs::<Value, Value>() {
            let (k, v) = pair.map_luau(None)?;
            let Value::String(key_str) = k else {
                continue;
            };
            let name = key_str.to_str().map_luau(None)?.to_string();
            match v {
                Value::Function(func) => {
                    exports.insert(name, lua.create_registry_value(func).map_luau(None)?);
                }
                Value::Table(tbl) => {
                    live_tables.insert(name, lua.create_registry_value(tbl).map_luau(None)?);
                }
                _ => {}
            }
        }

        let mut inner = self.state.lock();
        if let Some(old) = inner.vms.remove(mod_id) {
            retire_vm_records_locked(&mut inner, &old.identity);
        }
        inner.policies.activate_generation(&identity);
        inner.vms.insert(
            mod_id.to_string(),
            ManagedHookVm {
                identity: identity.clone(),
                lua,
                budget,
                budget_ms: limits.budget_ms,
                exports,
                live_tables,
            },
        );
        Ok(identity)
    }

    /// Retire a mod VM generation, invalidating all owned handles, proxies, hooks, and overrides.
    pub fn retire_vm(&self, identity: &ModIdentity) -> Result<(), ScriptError> {
        let mut inner = self.state.lock();
        if inner
            .vms
            .get(&identity.mod_id)
            .is_some_and(|vm| &vm.identity == identity)
        {
            inner.vms.remove(&identity.mod_id);
        }
        retire_vm_records_locked(&mut inner, identity);
        Ok(())
    }

    /// Register a `PreHook` compiled inside `caller`'s own isolated Luau VM.
    pub fn register_pre_hook(
        &self,
        caller: &ModIdentity,
        target: &HookTargetId,
        hook_fn_src: &str,
    ) -> Result<(), ScriptError> {
        self.register_phase_callback(caller, target, HookPhase::Pre, hook_fn_src)
    }

    /// Register an `OverrideFunction` candidate compiled inside `caller`'s own isolated Luau VM.
    pub fn register_override(
        &self,
        caller: &ModIdentity,
        target: &HookTargetId,
        override_fn_src: &str,
    ) -> Result<(), ScriptError> {
        self.register_phase_callback(caller, target, HookPhase::Override, override_fn_src)
    }

    /// Register a `PostHook` compiled inside `caller`'s own isolated Luau VM.
    pub fn register_post_hook(
        &self,
        caller: &ModIdentity,
        target: &HookTargetId,
        post_fn_src: &str,
    ) -> Result<(), ScriptError> {
        self.register_phase_callback(caller, target, HookPhase::Post, post_fn_src)
    }

    fn register_phase_callback(
        &self,
        caller: &ModIdentity,
        target: &HookTargetId,
        phase: HookPhase,
        fn_src: &str,
    ) -> Result<(), ScriptError> {
        let (lua, budget, budget_ms) = {
            let inner = self.state.lock();
            inner
                .policies
                .check_approval(caller, target, phase.required_scope())?;
            let vm = inner
                .vms
                .get(&caller.mod_id)
                .filter(|vm| &vm.identity == caller)
                .ok_or_else(|| {
                    ScriptError::StaleHandle(format!(
                        "caller '{}' gen {} VM unavailable",
                        caller.mod_id, caller.generation
                    ))
                })?;
            (vm.lua.clone(), Arc::clone(&vm.budget), vm.budget_ms)
        };
        let guard = budget.arm(budget_ms);
        let func: Function = lua
            .load(fn_src)
            .set_name(format!("@{}/hook.luau", caller.mod_id))
            .eval()
            .map_luau(guard.into_timeout_error())?;
        let key = lua.create_registry_value(func).map_luau(None)?;
        let cb = RegisteredCallback {
            owner: caller.clone(),
            fn_key: Arc::new(key),
        };

        let mut inner = self.state.lock();
        let list = match phase {
            HookPhase::Pre => inner.pre_hooks.entry(target.clone()).or_default(),
            HookPhase::Override => inner.overrides.entry(target.clone()).or_default(),
            HookPhase::Post => inner.post_hooks.entry(target.clone()).or_default(),
        };
        if !matches!(phase, HookPhase::Override) && list.len() >= MAX_CROSS_VM_FANOUT {
            return Err(ScriptError::InvocationFailed(format!(
                "{} fan-out exceeds max {MAX_CROSS_VM_FANOUT}",
                phase.label()
            )));
        }
        list.push(cb);
        Ok(())
    }

    /// Inspect override precedence and shadowed contenders for `target` (H-004).
    pub fn inspect_override_conflicts(
        &self,
        target: &HookTargetId,
    ) -> Result<OverrideConflictReport, ScriptError> {
        let inner = self.state.lock();
        let contenders = collect_contenders(&inner.overrides, target);
        Ok(inner.policies.resolve_override_winner(target, &contenders))
    }

    /// Read a named exported live Lua table field inside `mod_id`'s own VM (used to verify
    /// real object mutations performed via cross-VM proxies).
    pub fn read_target_table_field(
        &self,
        mod_id: &str,
        table_export_name: &str,
        field: &str,
    ) -> Result<HookValue, ScriptError> {
        let inner = self.state.lock();
        let vm = inner
            .vms
            .get(mod_id)
            .ok_or_else(|| ScriptError::StaleHandle(format!("unknown VM '{mod_id}'")))?;
        let tbl_key = vm.live_tables.get(table_export_name).ok_or_else(|| {
            ScriptError::InvocationFailed(format!(
                "mod '{mod_id}' exports no table '{table_export_name}'"
            ))
        })?;
        let tbl: Table = vm.lua.registry_value(tbl_key).map_luau(None)?;
        let raw: Value = tbl.get(field).map_luau(None)?;
        lua_value_to_hook(&raw)
    }

    /// Issue a generation-pinned [`GetFunctionHandle`] bypassing surrounding hooks (H-013).
    pub fn get_function(
        &self,
        caller: &ModIdentity,
        target: &HookTargetId,
    ) -> Result<GetFunctionHandle, ScriptError> {
        let mut inner = self.state.lock();
        inner
            .policies
            .check_approval(caller, target, HookPermissionScope::OverrideBody)?;
        let contenders = collect_contenders(&inner.overrides, target);
        let selected_cb = match inner.policies.resolve_override_winner(target, &contenders).winner {
            OverrideWinner::Override { owner } => inner
                .overrides
                .get(target)
                .and_then(|list| list.iter().rfind(|cb| cb.owner == owner).cloned())
                .ok_or_else(|| ScriptError::StaleHandle("override function vanished".into()))?,
            OverrideWinner::Original { .. } => {
                let vm = inner
                    .vms
                    .get(&target.target_mod_id)
                    .filter(|vm| inner.policies.is_generation_active(&vm.identity))
                    .ok_or_else(|| {
                        ScriptError::StaleHandle(format!(
                            "target mod '{}' retired",
                            target.target_mod_id
                        ))
                    })?;
                let fn_key = vm.exports.get(&target.symbol_path).ok_or_else(|| {
                    ScriptError::InvocationFailed(format!(
                        "exported function '{}.{}' not found",
                        target.target_mod_id, target.symbol_path
                    ))
                })?;
                let func: Function = vm.lua.registry_value(fn_key).map_luau(None)?;
                RegisteredCallback {
                    owner: vm.identity.clone(),
                    fn_key: Arc::new(vm.lua.create_registry_value(func).map_luau(None)?),
                }
            }
        };
        let handle_key = inner.get_fn_handles.insert(selected_cb);
        match inner
            .policies
            .authorize_get_function_handle(caller, target, &contenders, handle_key)
        {
            Ok(handle) => Ok(handle),
            Err(err) => {
                inner.get_fn_handles.remove(handle_key);
                Err(err)
            }
        }
    }

    /// Invoke a previously acquired [`GetFunctionHandle`] directly without pre/post hooks (H-013).
    pub fn invoke_get_function(
        &self,
        handle: &GetFunctionHandle,
        args: Vec<HookValue>,
        budget_ms: u32,
    ) -> Result<HookReturnTuple, ScriptError> {
        let (cb, lua, budget, body_budget_ms) = {
            let inner = self.state.lock();
            inner.policies.validate_get_function_handle(handle)?;
            let cb = inner
                .get_fn_handles
                .get(handle.handle_key)
                .cloned()
                .ok_or_else(|| ScriptError::StaleHandle("GetFunction handle expired".into()))?;
            let vm = inner
                .vms
                .get(&cb.owner.mod_id)
                .filter(|vm| vm.identity == cb.owner)
                .ok_or_else(|| {
                    ScriptError::StaleHandle(format!(
                        "bound VM '{}' gen {} closed",
                        cb.owner.mod_id, cb.owner.generation
                    ))
                })?;
            (
                cb,
                vm.lua.clone(),
                Arc::clone(&vm.budget),
                budget_ms.min(vm.budget_ms),
            )
        };

        let guard = budget.arm(body_budget_ms);
        let func: Function = lua.registry_value(&cb.fn_key).map_luau(None)?;
        let mut mv = MultiValue::new();
        for arg in args {
            mv.push_back(hook_value_to_plain_lua(&lua, &arg).map_luau(None)?);
        }
        let ret: MultiValue = func.call(mv).map_luau(guard.into_timeout_error())?;
        lua_multivalue_to_tuple(&ret)
    }

    /// Synchronously execute `target` with named live exported table args or scalar args:
    /// `PreHooks -> active Override (or Original) -> PostHooks`.
    pub fn invoke_target(
        &self,
        caller: &ModIdentity,
        target: &HookTargetId,
        args: &[CrossVmCallArg],
        chain_budget_ms: u32,
    ) -> Result<HookReturnTuple, ScriptError> {
        invoke_target_shared(&self.state, caller, target, args, chain_budget_ms)
    }
}

fn collect_contenders(
    overrides: &BTreeMap<HookTargetId, Vec<RegisteredCallback>>,
    target: &HookTargetId,
) -> Vec<ModIdentity> {
    overrides
        .get(target)
        .map(|list| list.iter().map(|entry| entry.owner.clone()).collect())
        .unwrap_or_default()
}

fn collect_sorted_hooks(
    hooks_map: &BTreeMap<HookTargetId, Vec<RegisteredCallback>>,
    policies: &HookPolicyRegistry,
    target: &HookTargetId,
) -> Vec<RegisteredCallback> {
    let mut list: Vec<RegisteredCallback> = hooks_map
        .get(target)
        .cloned()
        .unwrap_or_default()
        .into_iter()
        .filter(|cb| policies.is_generation_active(&cb.owner))
        .collect();
    policies.sort_by_owner_rank(&mut list, |cb| &cb.owner.mod_id);
    list
}

fn retire_vm_records_locked(inner: &mut CrossVmInner, identity: &ModIdentity) {
    inner.policies.retire_generation(identity);
    inner.arena.retire_generation(identity);
    inner.get_fn_handles.retain(|_, cb| &cb.owner != identity);
    for list in [&mut inner.pre_hooks, &mut inner.overrides, &mut inner.post_hooks] {
        for callbacks in list.values_mut() {
            callbacks.retain(|cb| &cb.owner != identity);
        }
    }
}

fn invoke_target_shared(
    state: &Arc<Mutex<CrossVmInner>>,
    caller: &ModIdentity,
    target: &HookTargetId,
    args: &[CrossVmCallArg],
    chain_budget_ms: u32,
) -> Result<HookReturnTuple, ScriptError> {
    let outermost_deadline = Instant::now() + Duration::from_millis(u64::from(chain_budget_ms));

    let (
        frame_id,
        effective_deadline,
        target_lua,
        target_budget,
        target_owner,
        target_fn_key,
        pre_hooks,
        winner_override,
        post_hooks,
        exported_proxies,
        proxy_bindings,
    ) = {
        let mut inner = state.lock();
        if !inner.policies.is_generation_active(caller) {
            return Err(ScriptError::StaleHandle(format!(
                "caller '{}' gen {} is no longer active",
                caller.mod_id, caller.generation
            )));
        }
        let target_vm = inner.vms.get(&target.target_mod_id).ok_or_else(|| {
            ScriptError::StaleHandle(format!(
                "target mod '{}' has no active VM",
                target.target_mod_id
            ))
        })?;
        let target_owner = target_vm.identity.clone();
        let target_lua = target_vm.lua.clone();
        let target_budget = Arc::clone(&target_vm.budget);
        let target_fn_key = target_vm.exports.get(&target.symbol_path).ok_or_else(|| {
            ScriptError::InvocationFailed(format!(
                "target exported symbol '{}.{}' not found (closures cannot be hooked)",
                target.target_mod_id, target.symbol_path
            ))
        })?;
        let func: Function = target_lua.registry_value(target_fn_key).map_luau(None)?;
        let cloned_fn_key = Arc::new(target_lua.create_registry_value(func).map_luau(None)?);
        let requested_tables = args
            .iter()
            .filter_map(|arg| match arg {
                CrossVmCallArg::TargetExportedTable(export_name) => Some(export_name.as_str()),
                CrossVmCallArg::Value(_) => None,
            })
            .map(|export_name| {
                let key = target_vm.live_tables.get(export_name).ok_or_else(|| {
                    ScriptError::InvocationFailed(format!(
                        "mod '{}' does not export table '{export_name}'",
                        target.target_mod_id
                    ))
                })?;
                let tbl = target_lua.registry_value::<Table>(key).map_luau(None)?;
                Ok((export_name, tbl))
            })
            .collect::<Result<Vec<_>, ScriptError>>()?;

        let frame_id = inner
            .frames
            .push_frame(target.clone(), caller.clone(), Some(outermost_deadline))?
            .frame_id;
        let effective_deadline = inner.frames.chain_deadline().unwrap_or(outermost_deadline);

        let pre_hooks = collect_sorted_hooks(&inner.pre_hooks, &inner.policies, target);
        let post_hooks = collect_sorted_hooks(&inner.post_hooks, &inner.policies, target);
        let contenders = collect_contenders(&inner.overrides, target);
        let winner_override = match inner.policies.resolve_override_winner(target, &contenders).winner {
            OverrideWinner::Original { .. } => None,
            OverrideWinner::Override { owner } => inner
                .overrides
                .get(target)
                .and_then(|list| list.iter().rfind(|cb| cb.owner == owner).cloned()),
        };

        let mut ptr_map: HashMap<usize, ProxyObjectKey> = HashMap::new();
        let mut proxy_bindings: Vec<(ProxyObjectKey, Arc<RegistryKey>)> = Vec::new();
        let mut exported_proxies = Vec::with_capacity(args.len());
        let mut table_iter = requested_tables.into_iter();
        for arg in args {
            match arg {
                CrossVmCallArg::Value(v) => exported_proxies.push(v.clone()),
                CrossVmCallArg::TargetExportedTable(_) => {
                    let (_, tbl) = table_iter.next().ok_or_else(|| {
                        ScriptError::SchedulerError("missing resolved target exported table".into())
                    })?;
                    let proxy_val = project_lua_table_to_proxy(
                        &target_lua,
                        &tbl,
                        &target_owner,
                        frame_id,
                        &mut inner.arena,
                        &mut ptr_map,
                        &mut proxy_bindings,
                    )?;
                    exported_proxies.push(proxy_val);
                }
            }
        }

        (
            frame_id,
            effective_deadline,
            target_lua,
            target_budget,
            target_owner,
            cloned_fn_key,
            pre_hooks,
            winner_override,
            post_hooks,
            exported_proxies,
            proxy_bindings,
        )
    };

    let _frame_guard = FrameGuard { state, frame_id };

    for pre in &pre_hooks {
        run_hook_callback(
            state,
            target,
            pre,
            HookPermissionScope::ObserveOnly,
            &exported_proxies,
            frame_id,
            effective_deadline,
        )?;
    }

    sync_arena_if_target_active(state, &target_owner, &target_lua, &proxy_bindings)?;

    // Execute winning Override or Original body (never fall back on Override failure — H-006).
    let body_tuple = if let Some(override_cb) = &winner_override {
        let ret = run_hook_callback(
            state,
            target,
            override_cb,
            HookPermissionScope::OverrideBody,
            &exported_proxies,
            frame_id,
            effective_deadline,
        )?;
        sync_arena_if_target_active(state, &target_owner, &target_lua, &proxy_bindings)?;
        HookReturnTuple::from_values(ret)
    } else {
        let vm_budget_ms = {
            let mut inner = state.lock();
            inner.frames.check_deadline()?;
            inner.frames.set_active_owner(target_owner.clone())?;
            inner
                .vms
                .get(&target_owner.mod_id)
                .filter(|vm| vm.identity == target_owner)
                .ok_or_else(|| {
                    ScriptError::StaleHandle(format!(
                        "target VM '{}' gen {} retired",
                        target_owner.mod_id, target_owner.generation
                    ))
                })?
                .budget_ms
        };
        let guard = target_budget.arm_until(effective_deadline, vm_budget_ms);
        let func: Function = target_lua.registry_value(&target_fn_key).map_luau(None)?;
        let mut mv = MultiValue::new();
        for arg in &exported_proxies {
            mv.push_back(hook_value_to_lua_in_owning_vm(arg, &proxy_bindings, &target_lua)?);
        }
        let raw_ret: MultiValue = func.call(mv).map_luau(guard.into_timeout_error())?;
        sync_lua_tables_to_arena(
            &target_lua,
            &target_owner,
            frame_id,
            &mut state.lock().arena,
            &proxy_bindings,
        )?;
        lua_multivalue_to_tuple(&raw_ret)?
    };

    state.lock().frames.set_frame_return(body_tuple)?;

    // Execute PostHooks only after body succeeded (H-007).
    for post in &post_hooks {
        let ret_vals = run_hook_callback(
            state,
            target,
            post,
            HookPermissionScope::ObserveOnly,
            &exported_proxies,
            frame_id,
            effective_deadline,
        )?;
        let mut inner = state.lock();
        let outcome = if ret_vals.is_empty() {
            PostHookOutcome::Unchanged
        } else {
            inner.policies.check_approval(
                &post.owner,
                target,
                HookPermissionScope::MutateArgs,
            )?;
            PostHookOutcome::Replace(HookReturnTuple::from_values(ret_vals))
        };
        inner.frames.apply_post_outcome(outcome)?;
    }

    sync_arena_if_target_active(state, &target_owner, &target_lua, &proxy_bindings)?;

    let final_ret = state.lock().frames.get_return()?.clone();
    Ok(final_ret)
}

fn sync_arena_if_target_active(
    state: &Arc<Mutex<CrossVmInner>>,
    target_owner: &ModIdentity,
    target_lua: &Lua,
    proxy_bindings: &[(ProxyObjectKey, Arc<RegistryKey>)],
) -> Result<(), ScriptError> {
    if proxy_bindings.is_empty() {
        return Ok(());
    }
    let inner = state.lock();
    if inner.policies.is_generation_active(target_owner) {
        sync_arena_to_lua_tables(target_lua, &inner.arena, proxy_bindings)?;
    }
    Ok(())
}

fn run_hook_callback(
    state: &Arc<Mutex<CrossVmInner>>,
    target: &HookTargetId,
    cb: &RegisteredCallback,
    required_scope: HookPermissionScope,
    args: &[HookValue],
    frame_id: u64,
    effective_deadline: Instant,
) -> Result<Vec<HookValue>, ScriptError> {
    let (lua, budget, vm_budget_ms) = {
        let mut inner = state.lock();
        inner.frames.check_deadline()?;
        inner
            .policies
            .check_approval(&cb.owner, target, required_scope)?;
        inner.frames.set_active_owner(cb.owner.clone())?;
        let vm = inner
            .vms
            .get(&cb.owner.mod_id)
            .filter(|vm| vm.identity == cb.owner)
            .ok_or_else(|| {
                ScriptError::StaleHandle(format!(
                    "hook owner '{}' gen {} no longer active",
                    cb.owner.mod_id, cb.owner.generation
                ))
            })?;
        (vm.lua.clone(), Arc::clone(&vm.budget), vm.budget_ms)
    };

    let guard = budget.arm_until(effective_deadline, vm_budget_ms);
    let func: Function = lua.registry_value(&cb.fn_key).map_luau(None)?;
    let mut mv = MultiValue::new();
    for arg in args {
        mv.push_back(
            hook_value_to_lua_for_caller(&lua, arg, frame_id, state)
                .map_err(|err| ScriptError::UnsupportedValue(err.to_string()))?,
        );
    }
    let raw_ret: MultiValue = func.call(mv).map_luau(guard.into_timeout_error())?;
    Ok(lua_multivalue_to_tuple(&raw_ret)?.values().to_vec())
}

fn install_cross_vm_host_api(
    lua: &Lua,
    owner: &ModIdentity,
    state: &Arc<Mutex<CrossVmInner>>,
) -> Result<(), ScriptError> {
    let simoder_tbl = lua.create_table().map_luau(None)?;
    let hooks_tbl = lua.create_table().map_luau(None)?;

    let state_ret = Arc::clone(state);
    hooks_tbl
        .set(
            "getReturn",
            lua.create_function(move |lua, ()| {
                let (tuple, frame_id) = {
                    let inner = state_ret.lock();
                    let tuple = inner.frames.get_return().map_err(mlua::Error::external)?.clone();
                    let frame_id = inner.frames.current().map_or(0, |f| f.frame_id);
                    (tuple, frame_id)
                };
                let mut mv = MultiValue::new();
                for val in tuple.values() {
                    mv.push_back(
                        hook_value_to_lua_for_caller(lua, val, frame_id, &state_ret)
                            .map_err(mlua::Error::external)?,
                    );
                }
                Ok(mv)
            })
            .map_luau(None)?,
        )
        .map_luau(None)?;

    let state_inv = Arc::clone(state);
    let owner_inv = owner.clone();
    hooks_tbl
        .set(
            "invoke",
            lua.create_function(
                move |lua, (target_mod, symbol, rest): (mlua::LuaString, mlua::LuaString, mlua::Variadic<Value>)| {
                    let target_id = HookTargetId::new(
                        target_mod.to_str()?.to_string(),
                        symbol.to_str()?.to_string(),
                    )
                    .map_err(mlua::Error::external)?;
                    let args = rest
                        .iter()
                        .map(|val| lua_value_to_hook(val).map(CrossVmCallArg::Value))
                        .collect::<Result<Vec<_>, _>>()
                        .map_err(mlua::Error::external)?;
                    let tuple = invoke_target_shared(&state_inv, &owner_inv, &target_id, &args, 10)
                        .map_err(mlua::Error::external)?;
                    let mut mv = MultiValue::new();
                    for val in tuple.values() {
                        mv.push_back(hook_value_to_plain_lua(lua, val)?);
                    }
                    Ok(mv)
                },
            )
            .map_luau(None)?,
        )
        .map_luau(None)?;

    let state_prov = Arc::clone(state);
    hooks_tbl
        .set(
            "provenance",
            lua.create_function(move |_, ()| {
                let inner = state_prov.lock();
                let frame = inner.frames.current().ok_or_else(|| {
                    mlua::Error::external(ScriptError::InvocationFailed("no active hook frame".into()))
                })?;
                Ok((
                    frame.initiating_provenance.mod_id.clone(),
                    frame.active_owner.mod_id.clone(),
                ))
            })
            .map_luau(None)?,
        )
        .map_luau(None)?;

    hooks_tbl.set_readonly(true);
    simoder_tbl.set("hooks", hooks_tbl).map_luau(None)?;
    simoder_tbl.set_readonly(true);
    lua.globals().set("simoder", simoder_tbl).map_luau(None)
}
