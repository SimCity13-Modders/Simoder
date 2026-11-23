//! Unified isolated script VM abstraction (`ScriptVm`), workspace 128 MiB memory pool,
//! shared host API backend (`HostBridge`), and lock-free callback timeout tracking.

pub mod cross_vm;
pub mod host_bridge;
pub mod js;
pub mod luau;

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::time::Instant;

use crate::config::MAX_AGGREGATE_SCRIPT_MEMORY_BYTES;
use crate::types::{
    ModIdentity, PropHostAccess, ScriptDefinition, ScriptError, ScriptLanguage,
    ScriptPatchCommand, ScriptStatusSnapshot,
};

pub use host_bridge::{
    CallbackCommandGuard, HostBridge, ModuleLoadGuard, PropertyKey, parse_script_tgi,
    resolve_relative_module_path, truncate_diagnostic,
};
pub(crate) use host_bridge::{validate_finite_f32, validate_u32_number};

/// Shared 128 MiB aggregate declared heap reservation pool preventing x86 address-space exhaustion.
#[derive(Debug, Default)]
pub struct AggregateMemoryPool {
    reserved_bytes: AtomicUsize,
}

impl AggregateMemoryPool {
    /// Allocate a new [`AggregateMemoryPool`].
    pub fn new() -> Arc<Self> {
        Arc::new(Self::default())
    }

    /// Return the currently reserved aggregate memory across active and staged VMs in bytes.
    pub fn reserved_bytes(&self) -> usize {
        self.reserved_bytes.load(Ordering::Acquire)
    }

    /// Atomically reserve `bytes` from the pool, releasing automatically when the guard drops.
    pub fn reserve(self: &Arc<Self>, bytes: usize) -> Result<MemoryReservation, ScriptError> {
        let mut current = self.reserved_bytes.load(Ordering::Acquire);
        loop {
            let next = current.checked_add(bytes).ok_or_else(|| {
                ScriptError::MemoryLimitExceeded("aggregate memory reservation overflow".into())
            })?;
            if next > MAX_AGGREGATE_SCRIPT_MEMORY_BYTES {
                return Err(ScriptError::MemoryLimitExceeded(format!(
                    "combined script memory budgets ({next} bytes) exceed {} MiB host limit",
                    MAX_AGGREGATE_SCRIPT_MEMORY_BYTES / (1024 * 1024)
                )));
            }
            match self.reserved_bytes.compare_exchange_weak(
                current,
                next,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => {
                    return Ok(MemoryReservation {
                        pool: Arc::clone(self),
                        bytes,
                    });
                }
                Err(actual) => current = actual,
            }
        }
    }
}

/// RAII lease holding a VM's reserved heap budget inside [`AggregateMemoryPool`].
#[derive(Debug)]
pub struct MemoryReservation {
    pool: Arc<AggregateMemoryPool>,
    bytes: usize,
}

impl MemoryReservation {
    /// Return the reserved byte capacity held by this lease.
    pub const fn bytes(&self) -> usize {
        self.bytes
    }
}

impl Drop for MemoryReservation {
    fn drop(&mut self) {
        self.pool
            .reserved_bytes
            .fetch_sub(self.bytes, Ordering::AcqRel);
    }
}

/// Lock-free monotonic wall-clock execution budget tracker shared across interrupt callbacks.
#[derive(Debug)]
pub struct ExecutionBudgetTracker {
    origin: Instant,
    deadline_us: AtomicU64,
    active_budget_ms: AtomicU32,
    exceeded: AtomicBool,
    exceeded_elapsed_us: AtomicU64,
}

impl ExecutionBudgetTracker {
    /// Create a disarmed tracker anchored at current monotonic instant.
    pub fn new() -> Arc<Self> {
        Arc::new(Self {
            origin: Instant::now(),
            deadline_us: AtomicU64::new(0),
            active_budget_ms: AtomicU32::new(0),
            exceeded: AtomicBool::new(false),
            exceeded_elapsed_us: AtomicU64::new(0),
        })
    }

    /// Arm an execution window of `budget_ms` starting immediately; returns an RAII restore guard.
    pub fn arm<'a>(self: &'a Arc<Self>, budget_ms: u32) -> BudgetGuard<'a> {
        let window_us = u64::from(budget_ms).saturating_mul(1000).max(1);
        self.arm_until_us(window_us, budget_ms)
    }

    /// Arm until an absolute `deadline` monotonic instant without relaxing an active outer deadline.
    pub fn arm_until(self: &Arc<Self>, deadline: Instant, budget_ms: u32) -> BudgetGuard<'_> {
        let remaining_us = deadline.saturating_duration_since(Instant::now()).as_micros();
        let clamped_us = remaining_us
            .min(u128::from(budget_ms).saturating_mul(1000))
            .max(1)
            .min(u128::from(u64::MAX - 1)) as u64;
        self.arm_until_us(clamped_us, budget_ms)
    }

    fn arm_until_us<'a>(self: &'a Arc<Self>, window_us: u64, budget_ms: u32) -> BudgetGuard<'a> {
        let start_us = self.elapsed_origin_us();
        let prev_deadline_us = self.deadline_us.load(Ordering::Acquire);
        let prev_budget_ms = self.active_budget_ms.load(Ordering::Acquire);
        let prev_exceeded = self.exceeded.swap(false, Ordering::AcqRel);
        let prev_exceeded_elapsed_us = self.exceeded_elapsed_us.swap(0, Ordering::AcqRel);

        let proposed_deadline_us = start_us.saturating_add(window_us).max(1);
        let next_deadline_us = if prev_deadline_us == 0 {
            proposed_deadline_us
        } else {
            proposed_deadline_us.min(prev_deadline_us)
        };

        self.active_budget_ms.store(budget_ms, Ordering::Release);
        self.deadline_us.store(next_deadline_us, Ordering::Release);
        BudgetGuard {
            tracker: self,
            start_us,
            prev_deadline_us,
            prev_budget_ms,
            prev_exceeded,
            prev_exceeded_elapsed_us,
        }
    }

    /// Poll whether the currently armed deadline has expired; latches elapsed duration when exceeded.
    pub fn poll_exceeded(&self) -> Option<(u32, u64)> {
        let deadline_us = self.deadline_us.load(Ordering::Acquire);
        if deadline_us == 0 {
            return None;
        }
        let now_us = self.elapsed_origin_us();
        if now_us >= deadline_us {
            let budget_ms = self.active_budget_ms.load(Ordering::Acquire);
            let window_us = u64::from(budget_ms).saturating_mul(1000);
            let approx_elapsed = now_us.saturating_sub(deadline_us.saturating_sub(window_us));
            self.exceeded_elapsed_us
                .store(approx_elapsed.max(window_us), Ordering::Release);
            self.exceeded.store(true, Ordering::Release);
            return Some((budget_ms, approx_elapsed));
        }
        None
    }

    /// Consume any latched budget-exceeded state recorded during the active invocation.
    pub fn take_exceeded(&self) -> Option<(u32, u64)> {
        self.exceeded.swap(false, Ordering::AcqRel).then(|| {
            (
                self.active_budget_ms.load(Ordering::Acquire),
                self.exceeded_elapsed_us.load(Ordering::Acquire),
            )
        })
    }

    fn elapsed_origin_us(&self) -> u64 {
        self.origin
            .elapsed()
            .as_micros()
            .min(u128::from(u64::MAX - 1)) as u64
            + 1
    }
}

/// RAII scope restoring any outer [`ExecutionBudgetTracker`] deadline even on panic or early return.
pub struct BudgetGuard<'a> {
    tracker: &'a ExecutionBudgetTracker,
    start_us: u64,
    prev_deadline_us: u64,
    prev_budget_ms: u32,
    prev_exceeded: bool,
    prev_exceeded_elapsed_us: u64,
}

impl BudgetGuard<'_> {
    /// Consume latched budget violation as [`ScriptError::BudgetExceeded`] if triggered.
    pub fn into_timeout_error(&self) -> Option<ScriptError> {
        self.tracker
            .take_exceeded()
            .map(|(budget_ms, recorded_us)| ScriptError::BudgetExceeded {
                budget_ms,
                elapsed_us: recorded_us.max(
                    self.tracker
                        .elapsed_origin_us()
                        .saturating_sub(self.start_us),
                ),
            })
    }
}

impl Drop for BudgetGuard<'_> {
    fn drop(&mut self) {
        self.tracker
            .active_budget_ms
            .store(self.prev_budget_ms, Ordering::Release);
        self.tracker
            .deadline_us
            .store(self.prev_deadline_us, Ordering::Release);
        if self.prev_exceeded && !self.tracker.exceeded.load(Ordering::Acquire) {
            self.tracker
                .exceeded_elapsed_us
                .store(self.prev_exceeded_elapsed_us, Ordering::Release);
            self.tracker.exceeded.store(true, Ordering::Release);
        }
    }
}

pub(crate) enum EngineBackend {
    Luau(luau::LuauBackend),
    JavaScript(js::JsBackend),
}

/// Isolated single-mod script VM hosting either Luau (`mlua`) or QuickJS-NG (`rquickjs`) with
/// shared lifecycle accounting, timeouts, log buffering, and PROP command staging.
pub struct ScriptVm {
    bridge: Arc<HostBridge>,
    budget: Arc<ExecutionBudgetTracker>,
    budget_ms: u32,
    last_tick: Option<Instant>,
    backend: EngineBackend,
    _reservation: MemoryReservation,
}

impl ScriptVm {
    /// Allocate, sandbox, and stage an isolated [`ScriptVm`], running entry and `onEnable()`.
    pub fn spawn(
        identity: ModIdentity,
        definition: &ScriptDefinition,
        host_access: PropHostAccess,
        pool: &Arc<AggregateMemoryPool>,
    ) -> Result<Self, ScriptError> {
        let reservation = pool.reserve(definition.limits.memory_bytes)?;
        let budget = ExecutionBudgetTracker::new();
        let (bridge, backend) = match definition.language {
            ScriptLanguage::Luau => {
                let bridge = HostBridge::new(identity, definition, host_access, crate::config::LUAU_DEFAULT_SOURCE_ORIGIN)?;
                let b = luau::LuauBackend::spawn(&bridge, definition, &budget)?;
                (bridge, EngineBackend::Luau(b))
            }
            ScriptLanguage::JavaScript => {
                let bridge = HostBridge::new(identity, definition, host_access, crate::config::JS_DEFAULT_SOURCE_ORIGIN)?;
                let b = js::JsBackend::spawn(&bridge, definition, &budget)?;
                (bridge, EngineBackend::JavaScript(b))
            }
        };
        Ok(Self {
            bridge,
            budget,
            budget_ms: definition.limits.budget_ms,
            last_tick: None,
            backend,
            _reservation: reservation,
        })
    }

    /// Borrow owning mod identity and generation.
    pub fn identity(&self) -> &ModIdentity {
        &self.bridge.identity
    }

    /// Return the underlying script runtime language.
    pub fn language(&self) -> ScriptLanguage {
        self.bridge.language
    }

    /// Commit a staged VM so it begins receiving scheduled ticks and exposing logs/PROP patches.
    pub fn commit(&self) -> Result<(), ScriptError> {
        self.bridge.commit()
    }

    /// Execute one bounded `onTick(deltaSeconds)` invocation; isolates callback failures cleanly.
    pub fn tick(&mut self, delta_seconds: f64) -> Result<(), ScriptError> {
        if !self.is_running() {
            return Ok(());
        }
        let now = Instant::now();
        let dt = if delta_seconds < 0.0 {
            self.last_tick
                .map_or(0.0, |prev| now.saturating_duration_since(prev).as_secs_f64())
        } else {
            delta_seconds
        };
        self.last_tick = Some(now);

        let guard = self.budget.arm(self.budget_ms);
        let cmd_guard = self.bridge.begin_callback();
        let res = match &mut self.backend {
            EngineBackend::Luau(b) => b.call_tick(dt, &guard),
            EngineBackend::JavaScript(b) => b.call_tick(dt, &guard),
        };
        match res {
            Ok(()) => {
                cmd_guard.keep();
                Ok(())
            }
            Err(err) => {
                self.bridge.mark_faulted(&err.to_string());
                Err(err)
            }
        }
    }

    /// Invoke optional read-only `onDisable()` once (if committed) and permanently stop execution.
    pub fn stop(&mut self) -> Option<String> {
        let should_run = self.bridge.enter_disable();
        let mut disable_error = None;
        if should_run {
            let guard = self.budget.arm(self.budget_ms);
            let res = match &mut self.backend {
                EngineBackend::Luau(b) => b.call_disable(&guard),
                EngineBackend::JavaScript(b) => b.call_disable(&guard),
            };
            if let Err(err) = res {
                disable_error = Some(err.to_string());
            }
        }
        if let EngineBackend::Luau(b) = &mut self.backend {
            b.release_keys();
        } else if let EngineBackend::JavaScript(b) = &mut self.backend {
            b.release_userdata();
        }
        self.bridge.finish_disable(disable_error.clone());
        disable_error
    }

    /// Force-fault VM with a bounded diagnostic.
    pub fn fail(&self, diagnostic: &str) {
        self.bridge.mark_faulted(diagnostic);
    }

    /// Check whether VM is committed and actively running.
    pub fn is_running(&self) -> bool {
        self.bridge.mutable.lock().runtime_state.is_running()
    }

    /// Drain unread buffered log lines.
    pub fn drain_logs(&self) -> Vec<String> {
        self.bridge.drain_logs()
    }

    /// Take pending PROP mutation commands emitted by successful callbacks.
    pub fn take_patch_commands(&self) -> Vec<ScriptPatchCommand> {
        self.bridge.take_patch_commands()
    }

    /// Report current live heap consumption reported by the engine in bytes.
    pub fn memory_bytes(&self) -> usize {
        match &self.backend {
            EngineBackend::Luau(b) => b.memory_bytes(),
            EngineBackend::JavaScript(b) => b.memory_bytes(),
        }
    }

    /// Capture a non-blocking diagnostic status snapshot.
    pub fn status(&self) -> ScriptStatusSnapshot {
        let guard = self.bridge.mutable.lock();
        ScriptStatusSnapshot {
            identity: self.bridge.identity.clone(),
            language: self.bridge.language,
            state: guard.runtime_state.clone(),
            memory_bytes: self.memory_bytes(),
            budget_bytes: self.bridge.budget_bytes,
            dropped_logs: guard.dropped_logs,
        }
    }
}
