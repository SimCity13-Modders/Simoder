//! Round-robin script scheduler coordinating Luau and QuickJS-NG VMs, staged atomic replacement,
//! aggregate memory accounting, `DelayedCalls`, custom events, and isolated per-mod fault policies.

use std::collections::{BTreeMap, VecDeque};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use parking_lot::Mutex;

use crate::config::{MAX_VM_SLOTS, SCHEDULER_SOFT_CYCLE_BUDGET_MS};
use crate::delayed_calls::{
    DelayedCallQueue, DelayedCallScheduleRequest, DueDelayedCall, SimulationClockSample,
};
use crate::events::{EventBus, EventEmitOutcome, EventHandler, EventKey, EventSchema, EventSubscriptionId};
use crate::hooks::{HookPolicyRegistry, HookValue};
use crate::types::{
    FaultPolicy, ModIdentity, PropHostAccess, ScriptDefinition, ScriptError, ScriptPatchCommand,
    ScriptStatusSnapshot, VmGeneration,
};
use crate::vm::{AggregateMemoryPool, ScriptVm};

/// Host sink receiving logs, PROP mutation batches, and mature delayed callbacks from [`ScriptScheduler::tick_cycle`].
pub trait SchedulerTickSink {
    /// Deliver one log line attributed to `identity`.
    fn on_log(&mut self, identity: &ModIdentity, message: &str);
    /// Submit one validated callback PROP batch outside the VM lock; return `Err` to reject & fault owner.
    fn on_patch_commands(
        &mut self,
        identity: &ModIdentity,
        commands: Vec<ScriptPatchCommand>,
    ) -> Result<(), String>;
    /// Deliver one matured [`DueDelayedCall`] scheduled by an active mod generation.
    fn on_delayed_call(&mut self, due: DueDelayedCall) -> Result<(), String>;
}

/// Summary returned when atomically committing a staged script generation.
#[derive(Debug, Clone)]
pub struct CommitOutcome {
    /// Newly activated mod identity and generation.
    pub identity: ModIdentity,
    /// Startup/enable log messages buffered while staged.
    pub startup_logs: Vec<String>,
    /// PROP commands queued during entry and `onEnable()`.
    pub patch_commands: Vec<ScriptPatchCommand>,
    /// Prior active generation retired cleanly during replacement, if any.
    pub retired_generation: Option<ModIdentity>,
}

/// Summary produced when cleanly stopping and removing an active mod generation.
#[derive(Debug, Clone)]
pub struct StopOutcome {
    /// Retired mod identity and generation.
    pub identity: ModIdentity,
    /// Final buffered log lines drained after `onDisable()`.
    pub final_logs: Vec<String>,
    /// Optional diagnostic if `onDisable()` threw an error during retirement.
    pub disable_error: Option<String>,
}

/// Telemetry returned by one round-robin [`ScriptScheduler::tick_cycle`] sweep.
#[derive(Debug, Clone, Default)]
pub struct TickCycleReport {
    /// Mod VMs invoked this turn.
    pub ticked_mods: Vec<ModIdentity>,
    /// Mod generations halted due to callback or sink failures during this turn.
    pub faulted_mods: Vec<(ModIdentity, String)>,
    /// Count of matured `DelayedCalls` dispatched this turn.
    pub dispatched_delayed_calls: usize,
}

struct SchedulerState {
    active_vms: BTreeMap<String, ScriptVm>,
    staged_vms: BTreeMap<String, ScriptVm>,
    active_generations: BTreeMap<String, VmGeneration>,
    turn_order: VecDeque<String>,
    delayed_calls: DelayedCallQueue,
    events: EventBus,
    hooks: HookPolicyRegistry,
    fault_policy: FaultPolicy,
    load_order: Vec<String>,
    shutting_down: bool,
}

/// Multi-runtime scheduler managing up to [`MAX_VM_SLOTS`] Luau and JavaScript mod VMs under
/// one shared [`AggregateMemoryPool`].
pub struct ScriptScheduler {
    pool: Arc<AggregateMemoryPool>,
    next_generation: AtomicU64,
    state: Mutex<SchedulerState>,
}

impl ScriptScheduler {
    /// Create a fresh [`ScriptScheduler`] with empty slots and explicit `fault_policy`.
    pub fn new(fault_policy: FaultPolicy) -> Self {
        Self {
            pool: AggregateMemoryPool::new(),
            next_generation: AtomicU64::new(1),
            state: Mutex::new(SchedulerState {
                active_vms: BTreeMap::new(),
                staged_vms: BTreeMap::new(),
                active_generations: BTreeMap::new(),
                turn_order: VecDeque::new(),
                delayed_calls: DelayedCallQueue::new(),
                events: EventBus::new(),
                hooks: HookPolicyRegistry::default(),
                fault_policy,
                load_order: Vec::new(),
                shutting_down: false,
            }),
        }
    }

    fn allocate_generation(&self) -> Result<VmGeneration, ScriptError> {
        let mut current = self.next_generation.load(Ordering::Acquire);
        loop {
            let next = current
                .checked_add(1)
                .ok_or_else(|| ScriptError::SchedulerError("script generation counter overflow".into()))?;
            match self
                .next_generation
                .compare_exchange_weak(current, next, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(raw) => return VmGeneration::new(raw),
                Err(actual) => current = actual,
            }
        }
    }

    /// Borrow the workspace-wide 128 MiB aggregate memory reservation pool.
    pub const fn memory_pool(&self) -> &Arc<AggregateMemoryPool> {
        &self.pool
    }

    /// Configure deterministic mod load order across active mods for event/hook precedence.
    pub fn set_mod_load_order(&self, order: Vec<String>) {
        let mut state = self.state.lock();
        state.hooks.set_mod_load_order(order.clone());
        state.load_order = order;
    }

    /// Reserve a VM slot + aggregate memory budget and stage a candidate VM without disrupting active ones.
    pub fn stage_script(
        &self,
        definition: &ScriptDefinition,
        host_access: PropHostAccess,
    ) -> Result<ModIdentity, ScriptError> {
        {
            let state = self.state.lock();
            if state.shutting_down {
                return Err(ScriptError::RuntimeClosed("scheduler is shutting down".into()));
            }
            if state.active_vms.len() + state.staged_vms.len() >= MAX_VM_SLOTS {
                return Err(ScriptError::SchedulerError(format!(
                    "script VM slot capacity exhausted ({MAX_VM_SLOTS})"
                )));
            }
            if state.staged_vms.contains_key(&definition.mod_id) {
                return Err(ScriptError::SchedulerError(format!(
                    "mod '{}' already has a staged candidate VM",
                    definition.mod_id
                )));
            }
        }

        let identity = ModIdentity::new(&definition.mod_id, self.allocate_generation()?)?;
        let vm = ScriptVm::spawn(identity.clone(), definition, host_access, &self.pool)?;

        let mut state = self.state.lock();
        if state.shutting_down {
            return Err(ScriptError::RuntimeClosed("scheduler shut down while staging".into()));
        }
        if state.active_vms.len() + state.staged_vms.len() >= MAX_VM_SLOTS
            || state.staged_vms.contains_key(&definition.mod_id)
        {
            return Err(ScriptError::SchedulerError(format!(
                "slot conflict staging candidate for '{}'",
                definition.mod_id
            )));
        }
        state.staged_vms.insert(definition.mod_id.clone(), vm);
        Ok(identity)
    }

    /// Discard a staged candidate VM without invoking `onDisable()`, releasing its memory reservation.
    pub fn discard_staged(&self, identity: &ModIdentity) -> Result<(), ScriptError> {
        let removed = {
            let mut state = self.state.lock();
            let existing = state.staged_vms.get(&identity.mod_id).ok_or_else(|| {
                ScriptError::StaleHandle(format!("no staged candidate for '{}'", identity.mod_id))
            })?;
            if existing.identity() != identity {
                return Err(ScriptError::StaleHandle(format!(
                    "staged candidate '{}' generation mismatch {}",
                    identity.mod_id, identity.generation
                )));
            }
            state.staged_vms.remove(&identity.mod_id)
        };
        drop(removed);
        Ok(())
    }

    /// Atomically publish a staged candidate VM, cleanly retiring any older generation of the same mod.
    pub fn commit_staged(&self, identity: &ModIdentity) -> Result<CommitOutcome, ScriptError> {
        let (old_vm, new_vm) = {
            let mut state = self.state.lock();
            if state.shutting_down {
                return Err(ScriptError::RuntimeClosed("scheduler is shutting down".into()));
            }
            let staged_ref = state.staged_vms.get(&identity.mod_id).ok_or_else(|| {
                ScriptError::StaleHandle(format!("no staged VM found for '{}'", identity.mod_id))
            })?;
            if staged_ref.identity() != identity {
                return Err(ScriptError::StaleHandle(format!(
                    "staged VM mismatch for '{}' generation {}",
                    identity.mod_id, identity.generation
                )));
            }
            let staged_vm = state.staged_vms.remove(&identity.mod_id).ok_or_else(|| {
                ScriptError::StaleHandle(format!("no staged VM found for '{}'", identity.mod_id))
            })?;
            let prior = state.active_vms.remove(&identity.mod_id);
            if let Some(ref old) = prior {
                retire_generation_metadata(&mut state, old.identity());
            }
            (prior, staged_vm)
        };

        let retired_generation = old_vm.map(|vm| retire_active_vm(vm).identity);

        new_vm.commit()?;
        let startup_logs = new_vm.drain_logs();
        let patch_commands = new_vm.take_patch_commands();

        let mut state = self.state.lock();
        if state.shutting_down {
            drop(state);
            let _ = retire_active_vm(new_vm);
            return Err(ScriptError::RuntimeClosed("scheduler is shutting down".into()));
        }
        state.active_generations.insert(identity.mod_id.clone(), identity.generation);
        state.hooks.activate_generation(identity);
        if !state.turn_order.iter().any(|id| id == &identity.mod_id) {
            state.turn_order.push_back(identity.mod_id.clone());
        }
        state.active_vms.insert(identity.mod_id.clone(), new_vm);

        Ok(CommitOutcome {
            identity: identity.clone(),
            startup_logs,
            patch_commands,
            retired_generation,
        })
    }

    /// Cleanly stop (`onDisable`) and retire an active mod generation.
    pub fn remove_mod(&self, mod_id: &str) -> Option<StopOutcome> {
        let vm = {
            let mut state = self.state.lock();
            state.turn_order.retain(|entry| entry != mod_id);
            if let Some(&generation) = state.active_generations.get(mod_id)
                && let Ok(identity) = ModIdentity::new(mod_id, generation)
            {
                retire_generation_metadata(&mut state, &identity);
            }
            state.active_vms.remove(mod_id)?
        };
        Some(retire_active_vm(vm))
    }

    /// Enqueue or replace a `DelayedCall` bound strictly to an active mod generation.
    pub fn schedule_delayed_call(
        &self,
        now_real: Instant,
        sim_sample: SimulationClockSample,
        req: DelayedCallScheduleRequest,
    ) -> Result<(), ScriptError> {
        let mut state = self.state.lock();
        ensure_active_gen(&state.active_generations, &req.owner)?;
        state.delayed_calls.schedule(now_real, sim_sample, req)
    }

    /// Cancel an owner-scoped delayed call.
    pub fn cancel_delayed_call(&self, owner: &ModIdentity, call_id: &str) -> bool {
        self.state.lock().delayed_calls.cancel(owner, call_id)
    }

    /// Register a namespaced public custom event on the scheduler's [`EventBus`].
    pub fn register_event(
        &self,
        owner: &ModIdentity,
        event_name: &str,
        schema: EventSchema,
    ) -> Result<EventKey, ScriptError> {
        let mut state = self.state.lock();
        ensure_active_gen(&state.active_generations, owner)?;
        state.events.register_event(owner.clone(), event_name, schema)
    }

    /// Subscribe an active mod generation to an existing registered event.
    pub fn subscribe_event(
        &self,
        subscriber: &ModIdentity,
        key: &EventKey,
        handler: EventHandler,
    ) -> Result<EventSubscriptionId, ScriptError> {
        let mut state = self.state.lock();
        ensure_active_gen(&state.active_generations, subscriber)?;
        state.events.subscribe(subscriber.clone(), key, handler)
    }

    /// Emit a namespaced custom event cleanly under the scheduler's fault policy and load order.
    pub fn emit_event(
        &self,
        emitter: &ModIdentity,
        key: &EventKey,
        payload: &HookValue,
    ) -> Result<EventEmitOutcome, ScriptError> {
        let state = self.state.lock();
        ensure_active_gen(&state.active_generations, emitter)?;
        state
            .events
            .emit(emitter, key, payload, &state.load_order, state.fault_policy)
    }

    /// Execute one fair round-robin scheduler turn bounded by [`SCHEDULER_SOFT_CYCLE_BUDGET_MS`].
    pub fn tick_cycle(
        &self,
        now_real: Instant,
        sim_sample: SimulationClockSample,
        sink: &mut dyn SchedulerTickSink,
    ) -> TickCycleReport {
        let mut report = TickCycleReport::default();
        let cycle_deadline = Instant::now() + Duration::from_millis(SCHEDULER_SOFT_CYCLE_BUDGET_MS);

        let due_calls = {
            let mut state = self.state.lock();
            if state.shutting_down {
                return report;
            }
            let gens = state.active_generations.clone();
            state.delayed_calls.poll_due(now_real, sim_sample, &gens)
        };

        for due in due_calls {
            let owner_id = due.owner.clone();
            report.dispatched_delayed_calls += 1;
            if let Err(msg) = sink.on_delayed_call(due) {
                let state = self.state.lock();
                if let Some(vm) = state.active_vms.get(&owner_id.mod_id)
                    && vm.identity() == &owner_id
                {
                    vm.fail(&format!("DelayedCall failed: {msg}"));
                }
                report.faulted_mods.push((owner_id, msg));
            }
        }

        let visit_count = self.state.lock().turn_order.len();
        for visited in 0..visit_count {
            if visited > 0 && Instant::now() >= cycle_deadline {
                break;
            }
            let maybe_vm = {
                let mut state = self.state.lock();
                if state.shutting_down {
                    break;
                }
                let Some(next_mod) = state.turn_order.pop_front() else {
                    break;
                };
                state.active_vms.remove(&next_mod).inspect(|_| {
                    state.turn_order.push_back(next_mod);
                })
            };

            let Some(mut vm) = maybe_vm else { continue };
            let identity = vm.identity().clone();
            if vm.is_running() {
                report.ticked_mods.push(identity.clone());
                if let Err(err) = vm.tick(-1.0) {
                    report.faulted_mods.push((identity.clone(), err.to_string()));
                } else {
                    let cmds = vm.take_patch_commands();
                    if !cmds.is_empty()
                        && let Err(err_msg) = sink.on_patch_commands(&identity, cmds)
                    {
                        let diag = format!("PROP transaction rejected: {err_msg}");
                        vm.fail(&diag);
                        report.faulted_mods.push((identity.clone(), diag));
                    }
                }
            }
            for line in vm.drain_logs() {
                sink.on_log(&identity, &line);
            }

            let mut state = self.state.lock();
            if state.shutting_down || state.active_generations.get(&identity.mod_id) != Some(&identity.generation) {
                drop(state);
                let _ = retire_active_vm(vm);
            } else {
                state.active_vms.insert(identity.mod_id.clone(), vm);
            }
        }

        report
    }

    /// Snapshot current runtime metrics and lifecycle state for `mod_id` if present.
    pub fn status_snapshot(&self, mod_id: &str) -> Option<ScriptStatusSnapshot> {
        let state = self.state.lock();
        state
            .active_vms
            .get(mod_id)
            .or_else(|| state.staged_vms.get(mod_id))
            .map(ScriptVm::status)
    }

    /// Snapshot status for every active and staged VM in deterministic `mod_id` order.
    pub fn all_statuses(&self) -> Vec<ScriptStatusSnapshot> {
        let state = self.state.lock();
        let mut out: Vec<ScriptStatusSnapshot> = state
            .active_vms
            .values()
            .chain(state.staged_vms.values())
            .map(ScriptVm::status)
            .collect();
        out.sort_by(|a, b| a.identity.mod_id.cmp(&b.identity.mod_id));
        out
    }

    /// Stop all active VMs cleanly (`onDisable`), discard any staged candidates, and free memory reservations.
    pub fn shutdown(&self) -> Vec<StopOutcome> {
        let (active, staged) = {
            let mut state = self.state.lock();
            state.shutting_down = true;
            state.turn_order.clear();
            let active = std::mem::take(&mut state.active_vms);
            let staged = std::mem::take(&mut state.staged_vms);
            for vm in active.values() {
                retire_generation_metadata(&mut state, vm.identity());
            }
            (active, staged)
        };
        drop(staged);
        active.into_values().map(retire_active_vm).collect()
    }
}

fn ensure_active_gen(
    active: &BTreeMap<String, VmGeneration>,
    identity: &ModIdentity,
) -> Result<(), ScriptError> {
    if active.get(&identity.mod_id) == Some(&identity.generation) {
        Ok(())
    } else {
        Err(ScriptError::StaleHandle(format!(
            "mod '{}' generation {} is not active",
            identity.mod_id, identity.generation
        )))
    }
}

fn retire_active_vm(mut vm: ScriptVm) -> StopOutcome {
    let disable_error = vm.stop();
    let identity = vm.identity().clone();
    let final_logs = vm.drain_logs();
    drop(vm);
    StopOutcome {
        identity,
        final_logs,
        disable_error,
    }
}

fn retire_generation_metadata(state: &mut SchedulerState, identity: &ModIdentity) {
    if state.active_generations.get(&identity.mod_id) == Some(&identity.generation) {
        state.active_generations.remove(&identity.mod_id);
    }
    state.hooks.retire_generation(identity);
    state.events.retire_generation(identity);
    state.delayed_calls.retire_generation(identity);
}
