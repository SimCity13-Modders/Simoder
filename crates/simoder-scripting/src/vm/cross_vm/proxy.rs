//! Host-owned generational proxy arena, alias-preserving Lua table projection, cycle detection,
//! and strict VM-boundary value serialization (`mlua::serde`) for synchronous cross-VM hooks.

use std::collections::{BTreeMap, HashMap};
use std::sync::Arc;

use mlua::{
    Lua, LuaSerdeExt as _, MetaMethod, MultiValue, RegistryKey, Table, UserData, UserDataMethods,
    Value, serde::SerializeOptions as LuaSerdeOptions,
};
use parking_lot::Mutex;
use serde_json::{Map as JsonMap, Number as JsonNumber, Value as JsonValue};
use slotmap::SlotMap;

use crate::config::MAX_PROXY_GRAPH_NODES;
use crate::hooks::{
    HookPermissionScope, HookReturnTuple, HookValue, ProxyHandle, ProxyObjectKey,
};
use crate::types::{ModIdentity, ScriptError};
use crate::vm::cross_vm::CrossVmInner;
use crate::vm::luau::LuauResultExt as _;

/// Live host-owned object node backing cross-VM mutable [`ForeignObjectProxy`] references.
#[derive(Debug, Clone)]
pub(crate) struct ProxyObjectNode {
    pub(crate) owner: ModIdentity,
    pub(crate) dispatch_frame_id: u64,
    pub(crate) fields: BTreeMap<String, HookValue>,
    pub(crate) frozen: bool,
}

/// Generational host proxy arena preserving identity, aliases, and cycles within a hook frame.
#[derive(Debug, Default)]
pub struct ProxyArena {
    nodes: SlotMap<ProxyObjectKey, ProxyObjectNode>,
}

impl ProxyArena {
    pub(crate) fn alloc_node(
        &mut self,
        owner: ModIdentity,
        dispatch_frame_id: u64,
        frozen: bool,
    ) -> Result<ProxyObjectKey, ScriptError> {
        if self.nodes.len() >= MAX_PROXY_GRAPH_NODES {
            return Err(ScriptError::UnsupportedValue(format!(
                "cross-VM proxy graph exceeds max {MAX_PROXY_GRAPH_NODES} live nodes"
            )));
        }
        Ok(self.nodes.insert(ProxyObjectNode {
            owner,
            dispatch_frame_id,
            fields: BTreeMap::new(),
            frozen,
        }))
    }

    pub(crate) fn get(&self, key: ProxyObjectKey) -> Option<&ProxyObjectNode> {
        self.nodes.get(key)
    }

    pub(crate) fn get_mut(&mut self, key: ProxyObjectKey) -> Option<&mut ProxyObjectNode> {
        self.nodes.get_mut(key)
    }

    pub(crate) fn retire_generation(&mut self, identity: &ModIdentity) {
        self.nodes.retain(|_, node| &node.owner != identity);
    }

    pub(crate) fn retire_frame(&mut self, frame_id: u64) {
        self.nodes
            .retain(|_, node| node.dispatch_frame_id != frame_id);
    }
}

/// Controlled mutable proxy userdata exposed to foreign VMs instead of raw pointers or copies.
#[derive(Clone)]
pub(crate) struct ForeignObjectProxy {
    pub(crate) key: ProxyObjectKey,
    pub(crate) owner: ModIdentity,
    pub(crate) dispatch_frame_id: u64,
    pub(crate) state: Arc<Mutex<CrossVmInner>>,
}

impl ForeignObjectProxy {
    fn authorize_access(
        inner: &CrossVmInner,
        dispatch_frame_id: u64,
        node_owner: &ModIdentity,
        required: HookPermissionScope,
    ) -> Result<(), ScriptError> {
        let frame = inner
            .frames
            .current()
            .ok_or_else(|| ScriptError::StaleHandle("proxy accessed outside active hook frame".into()))?;
        if frame.frame_id != dispatch_frame_id {
            return Err(ScriptError::StaleHandle(format!(
                "stale proxy handle from frame #{dispatch_frame_id} (active frame #{})",
                frame.frame_id
            )));
        }
        if !inner.policies.is_generation_active(node_owner) {
            return Err(ScriptError::StaleHandle(format!(
                "proxy target '{}' generation {} is no longer active",
                node_owner.mod_id, node_owner.generation
            )));
        }
        inner
            .policies
            .check_approval(&frame.active_owner, &frame.target, required)
    }
}

fn require_string_key(key: &Value, context: &str) -> Result<String, mlua::Error> {
    match key {
        Value::String(s) => s.to_str().map(|txt| txt.to_string()),
        _ => Err(mlua::Error::external(ScriptError::UnsupportedValue(format!(
            "{context} requires string keys"
        )))),
    }
}

impl UserData for ForeignObjectProxy {
    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_meta_method(MetaMethod::Index, |lua, this, key: Value| {
            let key_str = require_string_key(&key, "proxy indexing")?;
            let (val, dispatch_frame_id) = {
                let inner = this.state.lock();
                let node = inner.arena.get(this.key).ok_or_else(|| {
                    mlua::Error::external(ScriptError::StaleHandle(format!(
                        "proxy object {:?} retired",
                        this.key
                    )))
                })?;
                Self::authorize_access(
                    &inner,
                    this.dispatch_frame_id,
                    &node.owner,
                    HookPermissionScope::ObserveOnly,
                )
                .map_err(mlua::Error::external)?;
                let val = node
                    .fields
                    .get(&key_str)
                    .cloned()
                    .unwrap_or_else(HookValue::nil);
                (val, this.dispatch_frame_id)
            };
            hook_value_to_lua_for_caller(lua, &val, dispatch_frame_id, &this.state)
                .map_err(mlua::Error::external)
        });

        methods.add_meta_method(
            MetaMethod::NewIndex,
            |_, this, (key, value): (Value, Value)| {
                let key_str = require_string_key(&key, "proxy assignment")?;
                let mut inner = this.state.lock();
                let (node_owner, is_frozen) = inner
                    .arena
                    .get(this.key)
                    .map(|n| (n.owner.clone(), n.frozen))
                    .ok_or_else(|| {
                        mlua::Error::external(ScriptError::StaleHandle(format!(
                            "proxy object {:?} retired",
                            this.key
                        )))
                    })?;
                if is_frozen {
                    return Err(mlua::Error::external(ScriptError::SandboxViolation(
                        "cannot mutate frozen proxy target object".into(),
                    )));
                }
                Self::authorize_access(
                    &inner,
                    this.dispatch_frame_id,
                    &node_owner,
                    HookPermissionScope::MutateArgs,
                )
                .map_err(mlua::Error::external)?;

                let converted = match &value {
                    Value::Table(_) => {
                        return Err(mlua::Error::external(ScriptError::UnsupportedValue(
                            "foreign Lua tables cannot be assigned directly to cross-VM proxy fields"
                                .into(),
                        )));
                    }
                    Value::UserData(ud) => {
                        let rhs = ud.borrow::<ForeignObjectProxy>().map_err(|_| {
                            mlua::Error::external(ScriptError::UnsupportedValue(
                                "native/foreign userdata cannot cross VM boundaries".into(),
                            ))
                        })?;
                        if rhs.dispatch_frame_id != this.dispatch_frame_id || rhs.owner != node_owner
                        {
                            return Err(mlua::Error::external(ScriptError::UnsupportedValue(
                                "cannot attach proxy handle from another owner or invocation frame"
                                    .into(),
                            )));
                        }
                        if inner.arena.get(rhs.key).is_none() {
                            return Err(mlua::Error::external(ScriptError::StaleHandle(
                                "assigned proxy object has retired".into(),
                            )));
                        }
                        HookValue::Proxy(ProxyHandle {
                            object_key: rhs.key,
                            owner: rhs.owner.clone(),
                        })
                    }
                    other => lua_value_to_hook(other).map_err(mlua::Error::external)?,
                };

                let node = inner.arena.get_mut(this.key).ok_or_else(|| {
                    mlua::Error::external(ScriptError::StaleHandle("proxy disappeared".into()))
                })?;
                node.fields.insert(key_str, converted);
                Ok(())
            },
        );

        methods.add_meta_method(MetaMethod::Eq, |_, this, other: mlua::AnyUserData| {
            let rhs = other.borrow::<ForeignObjectProxy>()?;
            Ok(this.key == rhs.key && this.owner == rhs.owner)
        });
    }
}

pub(crate) fn project_lua_table_to_proxy(
    lua: &Lua,
    table: &Table,
    owner: &ModIdentity,
    dispatch_frame_id: u64,
    arena: &mut ProxyArena,
    visited: &mut HashMap<usize, ProxyObjectKey>,
    bindings: &mut Vec<(ProxyObjectKey, Arc<RegistryKey>)>,
) -> Result<HookValue, ScriptError> {
    lua_table_into_arena(
        Some(lua),
        table,
        owner,
        dispatch_frame_id,
        arena,
        visited,
        Some(bindings),
    )
}

fn lua_value_into_arena(
    lua: Option<&Lua>,
    value: &Value,
    owner: &ModIdentity,
    dispatch_frame_id: u64,
    arena: &mut ProxyArena,
    visited: &mut HashMap<usize, ProxyObjectKey>,
    bindings: Option<&mut Vec<(ProxyObjectKey, Arc<RegistryKey>)>>,
) -> Result<HookValue, ScriptError> {
    match value {
        Value::UserData(ud) => {
            if let Ok(proxy) = ud.borrow::<ForeignObjectProxy>() {
                return Ok(HookValue::Proxy(ProxyHandle {
                    object_key: proxy.key,
                    owner: proxy.owner.clone(),
                }));
            }
            Err(ScriptError::UnsupportedValue(
                "native/foreign userdata cannot cross VM boundaries".into(),
            ))
        }
        Value::Table(tbl) => lua_table_into_arena(
            lua,
            tbl,
            owner,
            dispatch_frame_id,
            arena,
            visited,
            bindings,
        ),
        other => lua_value_to_hook(other),
    }
}

fn lua_table_into_arena(
    lua: Option<&Lua>,
    table: &Table,
    owner: &ModIdentity,
    dispatch_frame_id: u64,
    arena: &mut ProxyArena,
    visited: &mut HashMap<usize, ProxyObjectKey>,
    mut bindings: Option<&mut Vec<(ProxyObjectKey, Arc<RegistryKey>)>>,
) -> Result<HookValue, ScriptError> {
    let ptr = table.to_pointer() as usize;
    if let Some(&existing_key) = visited.get(&ptr) {
        return Ok(HookValue::Proxy(ProxyHandle {
            object_key: existing_key,
            owner: owner.clone(),
        }));
    }

    let key = arena.alloc_node(owner.clone(), dispatch_frame_id, table.is_readonly())?;
    visited.insert(ptr, key);
    if let (Some(lua_vm), Some(bind_vec)) = (lua, bindings.as_deref_mut()) {
        let reg = Arc::new(lua_vm.create_registry_value(table.clone()).map_luau(None)?);
        bind_vec.push((key, reg));
    }

    let mut entries = Vec::new();
    for pair in table.pairs::<Value, Value>() {
        let (k, v) = pair.map_luau(None)?;
        let Value::String(ks) = k else {
            return Err(ScriptError::UnsupportedValue(
                "cross-VM table proxy requires string keys".into(),
            ));
        };
        let k_str = ks.to_str().map_luau(None)?.to_string();
        let child = lua_value_into_arena(
            lua,
            &v,
            owner,
            dispatch_frame_id,
            arena,
            visited,
            bindings.as_deref_mut(),
        )?;
        entries.push((k_str, child));
    }

    let node = arena
        .get_mut(key)
        .ok_or_else(|| ScriptError::SchedulerError("missing newly allocated proxy node".into()))?;
    for (field, val) in entries {
        node.fields.insert(field, val);
    }

    Ok(HookValue::Proxy(ProxyHandle {
        object_key: key,
        owner: owner.clone(),
    }))
}

pub(crate) fn sync_arena_to_lua_tables(
    lua: &Lua,
    arena: &ProxyArena,
    bindings: &[(ProxyObjectKey, Arc<RegistryKey>)],
) -> Result<(), ScriptError> {
    for (key, tbl_key) in bindings {
        let Some(node) = arena.get(*key) else {
            continue;
        };
        if node.frozen {
            continue;
        }
        let tbl: Table = lua.registry_value(tbl_key).map_luau(None)?;
        for (field, val) in &node.fields {
            let lua_val = hook_value_to_lua_in_owning_vm(val, bindings, lua)?;
            tbl.set(field.as_str(), lua_val).map_luau(None)?;
        }
    }
    Ok(())
}

pub(crate) fn sync_lua_tables_to_arena(
    lua: &Lua,
    owner: &ModIdentity,
    dispatch_frame_id: u64,
    arena: &mut ProxyArena,
    bindings: &[(ProxyObjectKey, Arc<RegistryKey>)],
) -> Result<(), ScriptError> {
    let mut visited = HashMap::new();
    for (key, tbl_key) in bindings {
        let tbl: Table = lua.registry_value(tbl_key).map_luau(None)?;
        visited.insert(tbl.to_pointer() as usize, *key);
    }
    for (key, tbl_key) in bindings {
        let tbl: Table = lua.registry_value(tbl_key).map_luau(None)?;
        let mut updates = BTreeMap::new();
        for pair in tbl.pairs::<Value, Value>() {
            let (k, v) = pair.map_luau(None)?;
            if let Value::String(ks) = k {
                let k_str = ks.to_str().map_luau(None)?.to_string();
                let converted = lua_value_into_arena(
                    None,
                    &v,
                    owner,
                    dispatch_frame_id,
                    arena,
                    &mut visited,
                    None,
                )?;
                updates.insert(k_str, converted);
            }
        }
        if let Some(node) = arena.get_mut(*key) {
            node.fields = updates;
        }
    }
    Ok(())
}

pub(crate) fn hook_value_to_lua_for_caller(
    lua: &Lua,
    val: &HookValue,
    dispatch_frame_id: u64,
    state: &Arc<Mutex<CrossVmInner>>,
) -> Result<Value, ScriptError> {
    match val {
        HookValue::Proxy(handle) => {
            let proxy = ForeignObjectProxy {
                key: handle.object_key,
                owner: handle.owner.clone(),
                dispatch_frame_id,
                state: Arc::clone(state),
            };
            let ud = lua.create_userdata(proxy).map_luau(None)?;
            Ok(Value::UserData(ud))
        }
        HookValue::Data(json) => hook_data_to_lua(lua, json).map_luau(None),
    }
}

pub(crate) fn hook_value_to_lua_in_owning_vm(
    val: &HookValue,
    bindings: &[(ProxyObjectKey, Arc<RegistryKey>)],
    lua: &Lua,
) -> Result<Value, ScriptError> {
    match val {
        HookValue::Proxy(handle) => {
            if let Some((_, reg)) = bindings.iter().find(|(k, _)| *k == handle.object_key) {
                let tbl: Table = lua.registry_value(reg).map_luau(None)?;
                return Ok(Value::Table(tbl));
            }
            Err(ScriptError::StaleHandle(format!(
                "missing owning table binding for proxy {:?}",
                handle.object_key
            )))
        }
        HookValue::Data(json) => hook_data_to_lua(lua, json).map_luau(None),
    }
}

pub(crate) fn hook_value_to_plain_lua(lua: &Lua, val: &HookValue) -> mlua::Result<Value> {
    match val {
        HookValue::Data(json) => hook_data_to_lua(lua, json),
        HookValue::Proxy(_) => Err(mlua::Error::external(ScriptError::UnsupportedValue(
            "proxy handle cannot be flattened without VM context".into(),
        ))),
    }
}

fn plain_lua_serde_options() -> LuaSerdeOptions {
    // Preserve JSON null as real Lua nil instead of mlua's LightUserData null sentinel.
    LuaSerdeOptions::new()
        .serialize_none_to_null(false)
        .serialize_unit_to_null(false)
}

fn hook_data_to_lua(lua: &Lua, json: &JsonValue) -> mlua::Result<Value> {
    if json.is_null() {
        return Ok(Value::Nil);
    }
    lua.to_value_with(json, plain_lua_serde_options())
}

pub(crate) fn lua_value_to_hook(val: &Value) -> Result<HookValue, ScriptError> {
    let mut visited_nodes = 0usize;
    lua_value_to_json(val, &mut visited_nodes).map(HookValue::Data)
}

fn lua_value_to_json(val: &Value, nodes: &mut usize) -> Result<JsonValue, ScriptError> {
    *nodes = nodes.saturating_add(1);
    if *nodes > MAX_PROXY_GRAPH_NODES {
        return Err(ScriptError::UnsupportedValue(format!(
            "cross-VM table payload exceeds max {MAX_PROXY_GRAPH_NODES} nodes"
        )));
    }
    match val {
        Value::Nil => Ok(JsonValue::Null),
        Value::Boolean(b) => Ok(JsonValue::Bool(*b)),
        Value::Integer(i) => Ok(JsonValue::Number((*i).into())),
        Value::Number(f) => JsonNumber::from_f64(*f)
            .map(JsonValue::Number)
            .ok_or_else(|| {
                ScriptError::UnsupportedValue("non-finite float cannot cross VM boundaries".into())
            }),
        Value::String(s) => Ok(JsonValue::String(s.to_str().map_luau(None)?.to_string())),
        Value::Function(_) => Err(ScriptError::UnsupportedValue(
            "arbitrary Luau functions/closures cannot cross VM boundaries".into(),
        )),
        Value::Thread(_) => Err(ScriptError::UnsupportedValue(
            "Luau coroutines/threads cannot cross VM boundaries".into(),
        )),
        Value::UserData(_) | Value::LightUserData(_) => Err(ScriptError::UnsupportedValue(
            "native userdata cannot cross VM boundaries".into(),
        )),
        Value::Table(tbl) => {
            let mut map = JsonMap::new();
            for pair in tbl.pairs::<Value, Value>() {
                let (k, v) = pair.map_luau(None)?;
                let Value::String(ks) = k else {
                    return Err(ScriptError::UnsupportedValue(
                        "table copy across VM requires string keys".into(),
                    ));
                };
                let key_str = ks.to_str().map_luau(None)?.to_string();
                map.insert(key_str, lua_value_to_json(&v, nodes)?);
            }
            Ok(JsonValue::Object(map))
        }
        other => Err(ScriptError::UnsupportedValue(format!(
            "unsupported Luau value type {:?} across VM boundary",
            other.type_name()
        ))),
    }
}

pub(crate) fn lua_multivalue_to_tuple(mv: &MultiValue) -> Result<HookReturnTuple, ScriptError> {
    let vals = mv
        .iter()
        .map(lua_value_to_hook)
        .collect::<Result<Vec<_>, _>>()?;
    Ok(HookReturnTuple::from_values(vals))
}
