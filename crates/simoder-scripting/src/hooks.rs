//! Beta function interception (`PreHook`, `PostHook`, `OverrideFunction`, `GetFunction`, `GetReturn`)
//! and deterministic cross-VM hook policy tracking.

use std::collections::{BTreeMap, BTreeSet};
use std::time::Instant;

use serde::{Deserialize, Serialize};
use serde_json::{Number as JsonNumber, Value as JsonValue};
use serde_with::DeserializeFromStr;
use slotmap::new_key_type;
use strum::{EnumString, IntoStaticStr, VariantArray};

use crate::config::{MAX_CROSS_VM_REENTRY_DEPTH, MAX_PROXY_GRAPH_NODES, RESERVED_BASE_MOD_ID};
use crate::types::{ModIdentity, ScriptError, VmGeneration};

/// Callback type executed before the active body inside [`HookChain`].
pub type PreHookFn<A> = Box<dyn FnMut(&mut A) -> Result<(), ScriptError>>;

/// Callback type for an original function body or override inside [`HookChain`].
pub type BodyFn<A, R> = Box<dyn FnMut(&mut A) -> Result<R, ScriptError>>;

/// Callback type executed after a successful body inside [`HookChain`].
pub type PostHookFn<A, R> = Box<dyn FnMut(&mut A, R) -> Result<R, ScriptError>>;

/// Ordered synchronous hook chain enforcing Beta rules H-003, H-004, H-006, and H-007.
pub struct HookChain<A, R> {
    pre_hooks: Vec<(String, PreHookFn<A>)>,
    overrides: Vec<(String, BodyFn<A, R>)>,
    post_hooks: Vec<(String, PostHookFn<A, R>)>,
    original: BodyFn<A, R>,
}

impl<A, R> HookChain<A, R> {
    /// Create a [`HookChain`] wrapping an `original` function body with no hooks or overrides.
    pub fn new(original: BodyFn<A, R>) -> Self {
        Self {
            pre_hooks: Vec::new(),
            overrides: Vec::new(),
            post_hooks: Vec::new(),
            original,
        }
    }

    /// Append a `PreHook` executed before the selected body.
    pub fn add_pre_hook(&mut self, owner: impl Into<String>, hook: PreHookFn<A>) {
        self.pre_hooks.push((owner.into(), hook));
    }

    /// Append an `OverrideFunction` candidate; the last registered override wins deterministically.
    pub fn add_override(&mut self, owner: impl Into<String>, body: BodyFn<A, R>) {
        self.overrides.push((owner.into(), body));
    }

    /// Append a `PostHook` executed only on body success that may inspect or transform the return value.
    pub fn add_post_hook(&mut self, owner: impl Into<String>, hook: PostHookFn<A, R>) {
        self.post_hooks.push((owner.into(), hook));
    }

    /// Return the active override owner, or `None` when the original body executes.
    pub fn active_override_owner(&self) -> Option<&str> {
        self.overrides.last().map(|(owner, _)| owner.as_str())
    }

    /// Execute `PreHooks -> last Override (or original, never falling back on error) -> PostHooks`.
    pub fn invoke(&mut self, args: &mut A) -> Result<R, ScriptError> {
        for (_, pre) in &mut self.pre_hooks {
            pre(args)?;
        }
        let mut result = if let Some((_, override_body)) = self.overrides.last_mut() {
            override_body(args)?
        } else {
            (self.original)(args)?
        };
        for (_, post) in &mut self.post_hooks {
            result = post(args, result)?;
        }
        Ok(result)
    }
}

new_key_type! {
    /// Generational key addressing one live mutable object node in [`crate::vm::cross_vm::ProxyArena`].
    pub struct ProxyObjectKey;
    /// Generational key addressing a `GetFunction` body binding inside [`crate::CrossVmHookHub`].
    pub struct FunctionHandleKey;
}

/// Verified cross-VM proxy descriptor bound to an owning [`ModIdentity`].
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct ProxyHandle {
    /// Generational key inside the host proxy arena.
    pub object_key: ProxyObjectKey,
    /// Owning mod identity whose generation must remain active on dereference.
    pub owner: ModIdentity,
}

/// Value dialect safe to cross isolated VM boundaries under Beta rule H-014.
#[derive(Debug, Clone, PartialEq)]
pub enum HookValue {
    /// Copyable JSON data snapshot (`Null`, `Bool`, finite `Number`, `String`, `Array`, `Object`).
    Data(JsonValue),
    /// Controlled mutable proxy referencing a live foreign object node.
    Proxy(ProxyHandle),
}

impl HookValue {
    /// Construct a `nil` / `null` value.
    pub const fn nil() -> Self {
        Self::Data(JsonValue::Null)
    }

    /// Construct a boolean value.
    pub const fn bool(value: bool) -> Self {
        Self::Data(JsonValue::Bool(value))
    }

    /// Construct an integer value.
    pub fn int(value: i64) -> Self {
        Self::Data(JsonValue::from(value))
    }

    /// Construct a finite IEEE-754 float value.
    pub fn float(value: f64) -> Result<Self, ScriptError> {
        JsonNumber::from_f64(value)
            .map(|n| Self::Data(JsonValue::Number(n)))
            .ok_or_else(|| {
                ScriptError::UnsupportedValue("non-finite float cannot cross VM boundaries".into())
            })
    }

    /// Construct a UTF-8 string value.
    pub fn text(value: impl Into<String>) -> Self {
        Self::Data(JsonValue::String(value.into()))
    }

    /// Estimate structural weight in bytes while enforcing [`MAX_PROXY_GRAPH_NODES`].
    pub fn estimated_bytes(&self) -> Result<usize, ScriptError> {
        let mut nodes = 0usize;
        match self {
            Self::Proxy(handle) => Ok(24 + handle.owner.mod_id.len()),
            Self::Data(json) => measure_json_bytes(json, &mut nodes),
        }
    }
}

fn measure_json_bytes(value: &JsonValue, nodes: &mut usize) -> Result<usize, ScriptError> {
    *nodes = nodes.saturating_add(1);
    if *nodes > MAX_PROXY_GRAPH_NODES {
        return Err(ScriptError::UnsupportedValue(format!(
            "cross-VM value graph exceeds max {MAX_PROXY_GRAPH_NODES} nodes"
        )));
    }
    Ok(match value {
        JsonValue::Null | JsonValue::Bool(_) => 1,
        JsonValue::Number(_) => 8,
        JsonValue::String(s) => s.len(),
        JsonValue::Array(arr) => {
            let mut sum = 8usize;
            for item in arr {
                sum = sum.saturating_add(measure_json_bytes(item, nodes)?);
            }
            sum
        }
        JsonValue::Object(map) => {
            let mut sum = 8usize;
            for (k, v) in map {
                sum = sum
                    .saturating_add(k.len())
                    .saturating_add(measure_json_bytes(v, nodes)?);
            }
            sum
        }
    })
}

/// Ordered invocation result tuple preserving arity as well as interior and trailing `Nil`s (H-005).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct HookReturnTuple(Vec<HookValue>);

impl HookReturnTuple {
    /// Empty zero-value return tuple.
    pub const fn empty() -> Self {
        Self(Vec::new())
    }

    /// Construct from explicit ordered values (preserving interior/trailing [`HookValue::nil`]).
    pub fn from_values(values: Vec<HookValue>) -> Self {
        Self(values)
    }

    /// Borrow all return tuple slots in exact return order.
    pub fn values(&self) -> &[HookValue] {
        &self.0
    }

    /// Return the exact tuple arity (distinct from Lua `#` array length).
    pub fn len(&self) -> usize {
        self.0.len()
    }

    /// Return `true` when zero return values were produced.
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
}

/// Result disposition returned by a `PostHook` differentiating no-op from explicit `nil` replacement.
#[derive(Debug, Clone, PartialEq)]
pub enum PostHookOutcome {
    /// PostHook returned zero values: preserve the preceding [`HookReturnTuple`].
    Unchanged,
    /// PostHook returned 1+ values: replace the invocation's active [`HookReturnTuple`].
    Replace(HookReturnTuple),
}

/// Verified identifier naming a public mod module/object function eligible for interception.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct HookTargetId {
    /// Mod owning the hooked symbol.
    pub target_mod_id: String,
    /// Dotted symbol path exposed through that mod's public module/table export.
    pub symbol_path: String,
}

impl HookTargetId {
    /// Validate a hookable target mod and public exported symbol path (blocking privileged internals).
    pub fn new(
        target_mod_id: impl Into<String>,
        symbol_path: impl Into<String>,
    ) -> Result<Self, ScriptError> {
        let target_mod_id = target_mod_id.into();
        let symbol_path = symbol_path.into();
        if target_mod_id.is_empty() || symbol_path.is_empty() {
            return Err(ScriptError::InvalidConfig(
                "hook target mod ID and symbol path must not be empty".into(),
            ));
        }
        if target_mod_id == RESERVED_BASE_MOD_ID
            || target_mod_id == "simoder"
            || target_mod_id.starts_with("simoder.")
        {
            return Err(ScriptError::PermissionDenied(format!(
                "privileged namespace '{target_mod_id}' cannot be hooked"
            )));
        }
        if symbol_path.starts_with('.')
            || symbol_path.ends_with('.')
            || symbol_path.contains("..")
            || symbol_path.chars().any(char::is_whitespace)
        {
            return Err(ScriptError::InvalidConfig(format!(
                "invalid hook symbol path '{symbol_path}'"
            )));
        }
        Ok(Self {
            target_mod_id,
            symbol_path,
        })
    }
}

/// Authorized cross-VM interception grant granted by user approval (Beta rule H-002).
#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    PartialOrd,
    Ord,
    Hash,
    Serialize,
    DeserializeFromStr,
    EnumString,
    IntoStaticStr,
    VariantArray,
)]
#[strum(serialize_all = "kebab-case", ascii_case_insensitive)]
#[serde(rename_all = "kebab-case")]
pub enum HookPermissionScope {
    /// Observe arguments and return values without mutating proxies or replacing the body.
    ObserveOnly,
    /// Read/write mutable foreign proxy fields and inspect/replace return values in Pre/Post hooks.
    MutateArgs,
    /// Full permission including `OverrideFunction` replacement and `GetFunction` invocation.
    OverrideBody,
}

impl HookPermissionScope {
    /// Return `true` when this scope satisfies `required`.
    pub const fn satisfies(self, required: Self) -> bool {
        (self as u8) >= (required as u8)
    }
}

/// User approval grant authorizing `caller_mod_id` to intercept `target` up to `scope`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct HookApproval {
    /// Registering mod retaining its own identity and permissions during callback execution.
    pub caller_mod_id: String,
    /// Intercepted mod and symbol scope.
    pub target: HookTargetId,
    /// Maximum authorized operation level.
    pub scope: HookPermissionScope,
}

/// Selected body disposition reported by override conflict resolution.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum OverrideWinner {
    /// No active override exists; original body executes.
    Original {
        /// Mod owning the original implementation.
        target_mod_id: String,
    },
    /// Deterministic last-wins override executes in place of the original body.
    Override {
        /// Winning override owner and active generation.
        owner: ModIdentity,
    },
}

/// Diagnostic inspection report detailing the active body winner and shadowed contenders (H-004).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OverrideConflictReport {
    /// Inspected hook target.
    pub target: HookTargetId,
    /// Currently selected body winner.
    pub winner: OverrideWinner,
    /// Contending overrides overridden by later configured load order entries.
    pub inactive_contenders: Vec<ModIdentity>,
}

/// Which function implementation a checked [`GetFunctionHandle`] references (H-013).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GetFunctionTarget {
    /// Original target body without surrounding pre/post hooks.
    Original,
    /// Specific override candidate generation winning at retrieval linearization.
    Override(ModIdentity),
}

/// Checked `GetFunction` handle bypassing pre/post hooks without implicit rebinding across reloads (H-013).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GetFunctionHandle {
    /// Generational body-entry key inside [`crate::CrossVmHookHub`].
    pub handle_key: FunctionHandleKey,
    /// Target symbol bound when `GetFunction` was resolved.
    pub target: HookTargetId,
    /// Calling mod generation granted this handle.
    pub caller: ModIdentity,
    /// Target mod generation alive when the handle was retrieved.
    pub target_generation: VmGeneration,
    /// Specific body revision bound at retrieval time.
    pub body_kind: GetFunctionTarget,
}

/// Private per-invocation execution frame isolating `GetReturn` and proving provenance (H-005 / H-012).
#[derive(Debug, Clone, PartialEq)]
pub struct InvocationFrame {
    /// Unique frame sequence number.
    pub frame_id: u64,
    /// Hook target being invoked in this frame.
    pub target: HookTargetId,
    /// Original initiating mod identity that triggered the outermost call.
    pub initiating_provenance: ModIdentity,
    /// Currently executing hook/body owner mod identity (cannot impersonate another owner).
    pub active_owner: ModIdentity,
    /// Private return tuple scoped strictly to this frame.
    pub current_return: HookReturnTuple,
    /// Nesting depth (`1` for outermost frame).
    pub depth: usize,
}

/// Synchronous cross-VM invocation frame stack with a shared unresettable chain deadline.
#[derive(Debug, Default)]
pub struct InvocationFrameStack {
    frames: Vec<InvocationFrame>,
    next_frame_id: u64,
    chain_deadline: Option<Instant>,
}

impl InvocationFrameStack {
    /// Enter a synchronous invocation frame, inheriting `initiating_provenance` and chain deadline.
    pub fn push_frame(
        &mut self,
        target: HookTargetId,
        caller: ModIdentity,
        deadline: Option<Instant>,
    ) -> Result<&InvocationFrame, ScriptError> {
        let next_depth = self.frames.len() + 1;
        if next_depth > MAX_CROSS_VM_REENTRY_DEPTH {
            return Err(ScriptError::ReentryLimitExceeded {
                max_depth: MAX_CROSS_VM_REENTRY_DEPTH,
            });
        }
        if self.frames.is_empty() {
            self.chain_deadline = deadline;
        } else {
            self.check_deadline()?;
        }

        self.next_frame_id = self.next_frame_id.wrapping_add(1).max(1);
        let initiating_provenance = self
            .frames
            .first()
            .map_or_else(|| caller.clone(), |root| root.initiating_provenance.clone());

        self.frames.push(InvocationFrame {
            frame_id: self.next_frame_id,
            target,
            initiating_provenance,
            active_owner: caller,
            current_return: HookReturnTuple::empty(),
            depth: next_depth,
        });
        Ok(self.frames.last().expect("frame just pushed"))
    }

    /// Update the active executing owner identity inside the current top frame.
    pub fn set_active_owner(&mut self, owner: ModIdentity) -> Result<(), ScriptError> {
        let top = self
            .frames
            .last_mut()
            .ok_or_else(|| ScriptError::InvocationFailed("no active hook invocation frame".into()))?;
        top.active_owner = owner;
        Ok(())
    }

    /// Store the body result tuple on the current private invocation frame.
    pub fn set_frame_return(&mut self, value: HookReturnTuple) -> Result<(), ScriptError> {
        let top = self
            .frames
            .last_mut()
            .ok_or_else(|| ScriptError::InvocationFailed("no active hook invocation frame".into()))?;
        top.current_return = value;
        Ok(())
    }

    /// Read the active return tuple of the innermost frame (`GetReturn`, Beta rule H-005).
    pub fn get_return(&self) -> Result<&HookReturnTuple, ScriptError> {
        self.frames
            .last()
            .map(|frame| &frame.current_return)
            .ok_or_else(|| {
                ScriptError::InvocationFailed("GetReturn called outside active hook frame".into())
            })
    }

    /// Apply a `PostHookOutcome` to the innermost frame, preserving prior result on `Unchanged`.
    pub fn apply_post_outcome(&mut self, outcome: PostHookOutcome) -> Result<(), ScriptError> {
        match outcome {
            PostHookOutcome::Unchanged => Ok(()),
            PostHookOutcome::Replace(next_tuple) => self.set_frame_return(next_tuple),
        }
    }

    /// Borrow the innermost active frame.
    pub fn current(&self) -> Option<&InvocationFrame> {
        self.frames.last()
    }

    /// Return the current synchronous re-entry nesting depth.
    pub fn depth(&self) -> usize {
        self.frames.len()
    }

    /// Return the active outermost chain deadline if currently dispatching.
    pub const fn chain_deadline(&self) -> Option<Instant> {
        self.chain_deadline
    }

    /// Verify that the outermost chain-wide deadline has not expired.
    pub fn check_deadline(&self) -> Result<(), ScriptError> {
        if let Some(deadline) = self.chain_deadline
            && Instant::now() >= deadline
        {
            return Err(ScriptError::BudgetExceeded {
                budget_ms: 0,
                elapsed_us: 0,
            });
        }
        Ok(())
    }

    /// Exit the innermost synchronous invocation frame cleanly, clearing chain deadline when empty.
    pub fn pop_frame(&mut self) -> Option<InvocationFrame> {
        let popped = self.frames.pop();
        if self.frames.is_empty() {
            self.chain_deadline = None;
        }
        popped
    }
}

/// Centralized deterministic hook policy store governing load order, approvals, generation lifetimes,
/// conflict reports, and checked `GetFunction` handle validation.
#[derive(Debug, Default)]
pub struct HookPolicyRegistry {
    load_order: Vec<String>,
    active_generations: BTreeMap<String, VmGeneration>,
    approvals: BTreeMap<(String, HookTargetId), HookPermissionScope>,
}

impl HookPolicyRegistry {
    /// Set the configured deterministic mod load order used for hook and override precedence.
    pub fn set_mod_load_order(&mut self, order: Vec<String>) {
        self.load_order = order;
    }

    /// Deterministically sort items by configured mod load order, tie-breaking lexicographically by mod ID.
    pub fn sort_by_owner_rank<T>(&self, items: &mut [T], owner_id: impl Fn(&T) -> &str) {
        sort_by_load_order(&self.load_order, items, owner_id);
    }

    /// Mark `identity` as the active committed generation for `identity.mod_id`.
    pub fn activate_generation(&mut self, identity: &ModIdentity) {
        self.active_generations
            .insert(identity.mod_id.clone(), identity.generation);
    }

    /// Retire `identity` if it matches the active generation.
    pub fn retire_generation(&mut self, identity: &ModIdentity) -> bool {
        if self.active_generations.get(&identity.mod_id) == Some(&identity.generation) {
            self.active_generations.remove(&identity.mod_id);
            true
        } else {
            false
        }
    }

    /// Check whether `identity` matches the live active generation for its `mod_id`.
    pub fn is_generation_active(&self, identity: &ModIdentity) -> bool {
        self.active_generations.get(&identity.mod_id) == Some(&identity.generation)
    }

    /// Return active generation for `mod_id`, if any.
    pub fn active_generation_for(&self, mod_id: &str) -> Option<VmGeneration> {
        self.active_generations.get(mod_id).copied()
    }

    /// Grant or update an explicit user hook approval for `approval.caller_mod_id`.
    pub fn grant_approval(&mut self, approval: HookApproval) {
        self.approvals
            .insert((approval.caller_mod_id, approval.target), approval.scope);
    }

    /// Revoke any hook approval held by `caller_mod_id` for `target`.
    pub fn revoke_approval(&mut self, caller_mod_id: &str, target: &HookTargetId) {
        self.approvals
            .remove(&(caller_mod_id.to_string(), target.clone()));
    }

    /// Verify live caller generation, active target generation, and authorized [`HookPermissionScope`].
    pub fn check_approval(
        &self,
        caller: &ModIdentity,
        target: &HookTargetId,
        required: HookPermissionScope,
    ) -> Result<(), ScriptError> {
        if !self.is_generation_active(caller) {
            return Err(ScriptError::StaleHandle(format!(
                "caller '{}' generation {} is no longer active",
                caller.mod_id, caller.generation
            )));
        }
        if !self.active_generations.contains_key(&target.target_mod_id) {
            return Err(ScriptError::StaleHandle(format!(
                "target mod '{}' has no active script generation",
                target.target_mod_id
            )));
        }
        if caller.mod_id == target.target_mod_id {
            return Ok(());
        }
        let Some(granted) = self.approvals.get(&(caller.mod_id.clone(), target.clone())) else {
            return Err(ScriptError::PermissionDenied(format!(
                "mod '{}' lacks approval to hook '{}.{}'",
                caller.mod_id, target.target_mod_id, target.symbol_path
            )));
        };
        if !granted.satisfies(required) {
            return Err(ScriptError::PermissionDenied(format!(
                "mod '{}' approval {:?} is insufficient for {:?} on '{}.{}'",
                caller.mod_id, granted, required, target.target_mod_id, target.symbol_path
            )));
        }
        Ok(())
    }

    /// Compute the deterministic last-wins override conflict report (H-004).
    pub fn resolve_override_winner(
        &self,
        target: &HookTargetId,
        contenders: &[ModIdentity],
    ) -> OverrideConflictReport {
        let mut sorted: Vec<ModIdentity> = contenders
            .iter()
            .filter(|id| self.is_generation_active(id))
            .cloned()
            .collect();
        self.sort_by_owner_rank(&mut sorted, |id| &id.mod_id);
        let mut seen_mods = BTreeSet::new();
        let mut deduped = Vec::with_capacity(sorted.len());
        for entry in sorted.into_iter().rev() {
            if seen_mods.insert(entry.mod_id.clone()) {
                deduped.push(entry);
            }
        }
        deduped.reverse();

        if let Some(winner) = deduped.pop() {
            OverrideConflictReport {
                target: target.clone(),
                winner: OverrideWinner::Override { owner: winner },
                inactive_contenders: deduped,
            }
        } else {
            OverrideConflictReport {
                target: target.clone(),
                winner: OverrideWinner::Original {
                    target_mod_id: target.target_mod_id.clone(),
                },
                inactive_contenders: Vec::new(),
            }
        }
    }

    /// Construct a checked [`GetFunctionHandle`] once its target body has been registered.
    pub fn authorize_get_function_handle(
        &self,
        caller: &ModIdentity,
        target: &HookTargetId,
        contenders: &[ModIdentity],
        handle_key: FunctionHandleKey,
    ) -> Result<GetFunctionHandle, ScriptError> {
        self.check_approval(caller, target, HookPermissionScope::OverrideBody)?;
        let Some(target_generation) = self.active_generation_for(&target.target_mod_id) else {
            return Err(ScriptError::StaleHandle(format!(
                "target mod '{}' is not active",
                target.target_mod_id
            )));
        };
        let body_kind = match self.resolve_override_winner(target, contenders).winner {
            OverrideWinner::Original { .. } => GetFunctionTarget::Original,
            OverrideWinner::Override { owner } => GetFunctionTarget::Override(owner),
        };
        Ok(GetFunctionHandle {
            handle_key,
            target: target.clone(),
            caller: caller.clone(),
            target_generation,
            body_kind,
        })
    }

    /// Validate an issued [`GetFunctionHandle`], failing explicitly on expiration, reload, or revocation.
    pub fn validate_get_function_handle(&self, handle: &GetFunctionHandle) -> Result<(), ScriptError> {
        self.check_approval(
            &handle.caller,
            &handle.target,
            HookPermissionScope::OverrideBody,
        )?;
        let target_identity = ModIdentity {
            mod_id: handle.target.target_mod_id.clone(),
            generation: handle.target_generation,
        };
        if !self.is_generation_active(&target_identity) {
            return Err(ScriptError::StaleHandle(format!(
                "GetFunction handle expired after '{}' generation {} reloaded",
                handle.target.target_mod_id, handle.target_generation
            )));
        }
        if let GetFunctionTarget::Override(owner) = &handle.body_kind
            && !self.is_generation_active(owner)
        {
            return Err(ScriptError::StaleHandle(format!(
                "GetFunction bound override '{}' generation {} expired",
                owner.mod_id, owner.generation
            )));
        }
        Ok(())
    }
}

/// Shared deterministic rank helper sorting items by configured mod load order, then `mod_id`.
pub(crate) fn rank_in_load_order(load_order: &[String], mod_id: &str) -> usize {
    load_order
        .iter()
        .position(|entry| entry == mod_id)
        .unwrap_or(usize::MAX)
}

/// Sort `items` deterministically using `load_order` rank and lexicographic tie-breaking.
pub(crate) fn sort_by_load_order<T>(
    load_order: &[String],
    items: &mut [T],
    owner_id: impl Fn(&T) -> &str,
) {
    items.sort_by(|a, b| {
        let id_a = owner_id(a);
        let id_b = owner_id(b);
        rank_in_load_order(load_order, id_a)
            .cmp(&rank_in_load_order(load_order, id_b))
            .then_with(|| id_a.cmp(id_b))
    });
}
