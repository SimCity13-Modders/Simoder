#![forbid(unsafe_code)]

//! Controlled Beta crash diagnostics, build-verified stack symbolication, full-dump consent gating,
//! and `Simoder-OpenSCP` Backend <-> Frontend (`bridge_observer`) & GlassBox (`engine_telemetry`) observers.
//!
//! General central logging (`LogLevel`, `LogSourceType`, `LogRecord`, `DiagnosticLogPipeline`) and
//! producer-side text redaction (`redact_sensitive_text`) live in [`simoder_core`] and are re-exported
//! here for convenience without duplication.

pub mod bridge_observer;
pub mod config;
pub mod engine_telemetry;
pub mod redaction;
pub mod report;
pub mod types;

pub use simoder_core::{
    DiagnosticLogPipeline, DrainedLogBatch, LogConsoleFilter, LogLevel, LogRecord, LogSourceType,
};
pub use {
    bridge_observer::*, config::*, engine_telemetry::*, redaction::*, report::*, types::*,
};
