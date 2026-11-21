#![forbid(unsafe_code)]

//! Embedded Luau (`mlua`) and QuickJS-NG (`rquickjs`) mod scripting VMs, scheduler, PROP reducer,
//! and Beta script extensions (synchronous cross-VM hooks, events, delayed calls, localization,
//! and authoritative city/UI/cache bridges).

pub mod bridge;
pub mod config;
pub mod delayed_calls;
pub mod events;
pub mod hooks;
pub mod localization;
pub mod patch_reducer;
pub mod scheduler;
pub mod types;
pub mod vm;

pub use {
    bridge::*, delayed_calls::*, events::*, hooks::*, localization::*, patch_reducer::*,
    scheduler::*, types::*,
};
pub use vm::cross_vm::{CrossVmCallArg, CrossVmHookHub};
pub use vm::{AggregateMemoryPool, ExecutionBudgetTracker, HostBridge, MemoryReservation, ScriptVm};
