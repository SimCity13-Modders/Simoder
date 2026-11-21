//! Scheduler-driven `DelayedCalls` supporting monotonic real time and pause-aware simulation clocks.

use std::collections::{BTreeMap, VecDeque};
use std::time::{Duration, Instant};

use serde::Serialize;
use serde_with::DeserializeFromStr;
use strum::{EnumString, IntoStaticStr, VariantArray};

use crate::config::{MAX_DELAYED_CALLS_PER_OWNER, MAX_DELAYED_CALLS_PER_TURN};
use crate::hooks::HookValue;
use crate::types::{ModIdentity, ScriptError, VmGeneration};

/// Clock domain advancing a scheduled delayed callback (Beta rule H-008).
#[derive(
    Debug,
    Clone,
    Copy,
    Default,
    PartialEq,
    Eq,
    Hash,
    Serialize,
    DeserializeFromStr,
    EnumString,
    IntoStaticStr,
    VariantArray,
)]
#[strum(serialize_all = "lowercase", ascii_case_insensitive)]
#[serde(rename_all = "lowercase")]
pub enum DelayClock {
    /// Monotonic real wall time (default when unspecified).
    #[default]
    Real,
    /// Verified game simulation elapsed clock; halts cleanly when simulation is paused or unloaded.
    Simulation,
}

/// Authoritative simulation clock sample provided to the scheduler each turn.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SimulationClockSample {
    /// Main menu or unloaded city; simulation timers never advance.
    NoCity,
    /// City loaded with simulation paused at monotonic `sim_elapsed_ms`; simulation timers freeze.
    Paused {
        /// Cumulative elapsed simulation milliseconds prior to pause.
        sim_elapsed_ms: u64,
    },
    /// Active running simulation advancing monotonically in `sim_elapsed_ms`.
    Running {
        /// Cumulative elapsed simulation milliseconds.
        sim_elapsed_ms: u64,
    },
}

/// Disposition applied to a [`DelayClock::Simulation`] timer when transitioning to [`SimulationClockSample::NoCity`].
#[derive(
    Debug,
    Clone,
    Copy,
    Default,
    PartialEq,
    Eq,
    Hash,
    Serialize,
    DeserializeFromStr,
    EnumString,
    IntoStaticStr,
    VariantArray,
)]
#[strum(serialize_all = "kebab-case", ascii_case_insensitive)]
#[serde(rename_all = "kebab-case")]
pub enum DelayedCallPolicyOnNoCity {
    /// Preserve remaining simulation time until a simulation is running again.
    #[default]
    Pause,
    /// Cancel pending simulation timers immediately when leaving a loaded city.
    CancelOnCityUnload,
}

/// Validated request to enqueue or atomically replace one delayed callback owned by `owner`.
#[derive(Debug, Clone, PartialEq)]
pub struct DelayedCallScheduleRequest {
    /// Owning mod identity (mod ID + active generation).
    pub owner: ModIdentity,
    /// Caller-chosen timer identifier scoped to `owner.mod_id`.
    pub call_id: String,
    /// Non-negative finite delay in seconds (`>= 0.0`).
    pub delay_seconds: f64,
    /// Real or simulation clock domain.
    pub clock: DelayClock,
    /// Behavior when transitioning to [`SimulationClockSample::NoCity`].
    pub on_no_city: DelayedCallPolicyOnNoCity,
    /// Optional structured callback payload copied cleanly into scheduler storage.
    pub payload: HookValue,
}

/// Due callback popped during [`DelayedCallQueue::poll_due`] for scheduler delivery.
#[derive(Debug, Clone, PartialEq)]
pub struct DueDelayedCall {
    /// Owning mod identity whose active generation will execute the callback.
    pub owner: ModIdentity,
    /// Timer identifier unique within `owner.mod_id`.
    pub call_id: String,
    /// Clock domain that matured this call.
    pub clock: DelayClock,
    /// Payload supplied at schedule time.
    pub payload: HookValue,
}

#[derive(Debug, Clone)]
enum TimerDeadline {
    Real { due_at: Instant },
    Simulation { remaining_ms: u64, on_no_city: DelayedCallPolicyOnNoCity },
}

#[derive(Debug, Clone)]
struct PendingDelayedCall {
    owner: ModIdentity,
    call_id: String,
    deadline: TimerDeadline,
    payload: HookValue,
}

/// Pure scheduler-driven queue for real and simulation delayed calls (no background timer thread).
#[derive(Debug, Default)]
pub struct DelayedCallQueue {
    entries: BTreeMap<(String, String), PendingDelayedCall>,
    owner_ring: VecDeque<String>,
    last_sim_elapsed_ms: Option<u64>,
}

impl DelayedCallQueue {
    /// Create an empty [`DelayedCallQueue`].
    pub fn new() -> Self {
        Self::default()
    }

    /// Return the total count of currently armed delayed calls across all owners.
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Return `true` when zero delayed calls are currently queued.
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Enqueue or atomically replace a delayed call keyed by `(owner.mod_id, call_id)`.
    pub fn schedule(
        &mut self,
        now_real: Instant,
        sim_sample: SimulationClockSample,
        req: DelayedCallScheduleRequest,
    ) -> Result<(), ScriptError> {
        if req.call_id.is_empty() {
            return Err(ScriptError::DelayedCallError("delayed call ID must not be empty".into()));
        }
        if !req.delay_seconds.is_finite() || req.delay_seconds < 0.0 {
            return Err(ScriptError::DelayedCallError(format!(
                "delayed call '{}' delay must be finite and >= 0.0 (got {})",
                req.call_id, req.delay_seconds
            )));
        }
        let delay_ms_f64 = (req.delay_seconds * 1000.0).round();
        if !delay_ms_f64.is_finite() || delay_ms_f64 > u64::MAX as f64 {
            return Err(ScriptError::DelayedCallError(format!(
                "delayed call '{}' delay overflows millisecond range",
                req.call_id
            )));
        }
        let delay_ms = delay_ms_f64 as u64;
        let _ = req.payload.estimated_bytes()?;

        let deadline = match req.clock {
            DelayClock::Real => TimerDeadline::Real {
                due_at: now_real
                    .checked_add(Duration::from_millis(delay_ms))
                    .ok_or_else(|| ScriptError::DelayedCallError("real-time deadline overflow".into()))?,
            },
            DelayClock::Simulation => {
                if matches!(sim_sample, SimulationClockSample::NoCity)
                    && req.on_no_city == DelayedCallPolicyOnNoCity::CancelOnCityUnload
                {
                    return Err(ScriptError::DelayedCallError(format!(
                        "simulation delayed call '{}' with CancelOnCityUnload requires an active city",
                        req.call_id
                    )));
                }
                self.advance_simulation_timers(sim_sample);
                TimerDeadline::Simulation {
                    remaining_ms: delay_ms,
                    on_no_city: req.on_no_city,
                }
            }
        };

        let key = (req.owner.mod_id.clone(), req.call_id.clone());
        if !self.entries.contains_key(&key) {
            let owned = self.entries.keys().filter(|(o, _)| o == &req.owner.mod_id).count();
            if owned >= MAX_DELAYED_CALLS_PER_OWNER {
                return Err(ScriptError::DelayedCallError(format!(
                    "owner '{}' reached maximum {} concurrent delayed calls",
                    req.owner.mod_id, MAX_DELAYED_CALLS_PER_OWNER
                )));
            }
            if !self.owner_ring.iter().any(|o| o == &req.owner.mod_id) {
                self.owner_ring.push_back(req.owner.mod_id.clone());
            }
        }

        self.entries.insert(
            key,
            PendingDelayedCall {
                owner: req.owner,
                call_id: req.call_id,
                deadline,
                payload: req.payload,
            },
        );
        Ok(())
    }

    /// Cancel an existing delayed call only when owned by `owner` (verifying `generation`).
    pub fn cancel(&mut self, owner: &ModIdentity, call_id: &str) -> bool {
        let key = (owner.mod_id.clone(), call_id.to_string());
        if self.entries.get(&key).is_some_and(|e| &e.owner == owner) {
            self.entries.remove(&key);
            true
        } else {
            false
        }
    }

    /// Retire all queued delayed calls belonging to `identity`, returning how many were dropped.
    pub fn retire_generation(&mut self, identity: &ModIdentity) -> usize {
        let before = self.entries.len();
        self.entries.retain(|_, entry| &entry.owner != identity);
        before - self.entries.len()
    }

    /// Advance simulation timers according to `sim_sample` and pop up to [`MAX_DELAYED_CALLS_PER_TURN`]
    /// eligible due calls in round-robin owner fairness order without catch-up bursts.
    pub fn poll_due(
        &mut self,
        now_real: Instant,
        sim_sample: SimulationClockSample,
        active_generations: &BTreeMap<String, VmGeneration>,
    ) -> Vec<DueDelayedCall> {
        self.entries.retain(|_, e| {
            active_generations.get(&e.owner.mod_id) == Some(&e.owner.generation)
        });
        self.advance_simulation_timers(sim_sample);

        let mut due = Vec::new();
        self.owner_ring
            .retain(|owner| self.entries.keys().any(|(mod_id, _)| mod_id == owner));
        let mut silent_passes = 0usize;

        while due.len() < MAX_DELAYED_CALLS_PER_TURN && silent_passes < self.owner_ring.len() {
            let Some(owner) = self.owner_ring.pop_front() else {
                break;
            };
            let due_key = self
                .entries
                .iter()
                .find(|((mod_id, _), e)| mod_id == &owner && Self::is_due(e, now_real))
                .map(|(k, _)| k.clone());

            if let Some(key) = due_key {
                if let Some(popped) = self.entries.remove(&key) {
                    due.push(DueDelayedCall {
                        owner: popped.owner,
                        call_id: popped.call_id,
                        clock: match popped.deadline {
                            TimerDeadline::Real { .. } => DelayClock::Real,
                            TimerDeadline::Simulation { .. } => DelayClock::Simulation,
                        },
                        payload: popped.payload,
                    });
                }
                silent_passes = 0;
            } else {
                silent_passes += 1;
            }

            if self.entries.keys().any(|(mod_id, _)| mod_id == &owner) {
                self.owner_ring.push_back(owner);
            }
        }
        due
    }

    fn advance_simulation_timers(&mut self, sim_sample: SimulationClockSample) {
        match sim_sample {
            SimulationClockSample::NoCity => {
                self.last_sim_elapsed_ms = None;
                self.entries.retain(|_, entry| {
                    !matches!(
                        entry.deadline,
                        TimerDeadline::Simulation {
                            on_no_city: DelayedCallPolicyOnNoCity::CancelOnCityUnload,
                            ..
                        }
                    )
                });
            }
            SimulationClockSample::Paused { sim_elapsed_ms } => {
                self.last_sim_elapsed_ms = Some(sim_elapsed_ms);
            }
            SimulationClockSample::Running { sim_elapsed_ms } => {
                let delta_ms = self
                    .last_sim_elapsed_ms
                    .filter(|&prev| sim_elapsed_ms >= prev)
                    .map_or(0, |prev| sim_elapsed_ms - prev);
                self.last_sim_elapsed_ms = Some(sim_elapsed_ms);
                if delta_ms > 0 {
                    for entry in self.entries.values_mut() {
                        if let TimerDeadline::Simulation { remaining_ms, .. } = &mut entry.deadline {
                            *remaining_ms = remaining_ms.saturating_sub(delta_ms);
                        }
                    }
                }
            }
        }
    }

    fn is_due(entry: &PendingDelayedCall, now_real: Instant) -> bool {
        match entry.deadline {
            TimerDeadline::Real { due_at } => now_real >= due_at,
            TimerDeadline::Simulation { remaining_ms, .. } => remaining_ms == 0,
        }
    }
}
