//! Strongly typed domain errors, single-source `strum` enums, script limits, and runtime state snapshots.

use std::collections::BTreeMap;
use std::fmt;
use std::num::NonZeroU64;
use std::sync::Arc;

use relative_path::RelativePath;
use serde::{Deserialize, Serialize};
use serde_with::DeserializeFromStr;
use simoder_core::{CoreError, Tgi};
use simoder_formats::FormatError;
use strum::{EnumString, IntoStaticStr, VariantArray};

use crate::config::{
    DEFAULT_CALLBACK_BUDGET_MS, DEFAULT_JS_MAX_STACK_BYTES, DEFAULT_VM_MEMORY_BYTES,
    MAX_CALLBACK_BUDGET_MS, MAX_SCRIPT_SOURCE_BYTES, MAX_VM_MEMORY_BYTES, MIN_CALLBACK_BUDGET_MS,
    MIN_VM_MEMORY_BYTES, RESERVED_BASE_MOD_ID,
};

/// Normalize a POSIX relative path with `relative-path` and enforce strict containment under `required_prefix`.
pub(crate) fn normalize_contained_posix_path(
    base_dir: &str,
    requested: &str,
    required_prefix: &str,
) -> Result<String, String> {
    if requested.is_empty()
        || requested.ends_with('/')
        || requested.starts_with('/')
        || requested.contains('\\')
        || requested.contains(':')
        || requested.as_bytes().contains(&0)
    {
        return Err(format!(
            "path '{requested}' must be a relative POSIX path without '\\', ':', leading '/', or NUL"
        ));
    }
    let normalized = if base_dir.is_empty() {
        RelativePath::new(requested).normalize()
    } else {
        RelativePath::new(base_dir).join_normalized(requested)
    };
    let text = normalized.as_str();
    if text == ".." || text.starts_with("../") || !text.starts_with(required_prefix) {
        return Err(format!(
            "normalized path '{text}' escapes required prefix '{required_prefix}'"
        ));
    }
    Ok(text.to_string())
}

/// Errors raised across script VM initialization, sandbox enforcement, host APIs, scheduling,
/// and Beta script extensions.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum ScriptError {
    /// The invoked script callback, hook, or override body raised an explicit execution error.
    #[error("hook or body execution failed: {0}")]
    InvocationFailed(String),
    /// Invalid script manifest limit, mod identity, generation, or entry definition.
    #[error("invalid script configuration: {0}")]
    InvalidConfig(String),
    /// Per-VM heap budget or workspace-wide aggregate memory ceiling was exceeded.
    #[error("script memory limit exceeded: {0}")]
    MemoryLimitExceeded(String),
    /// Callback execution exceeded its monotonic wall-clock interrupt budget.
    #[error("script callback execution budget exceeded (budget_ms={budget_ms}, elapsed_us={elapsed_us})")]
    BudgetExceeded {
        /// Configured callback timeout budget in milliseconds.
        budget_ms: u32,
        /// Elapsed execution duration in microseconds when interrupted.
        elapsed_us: u64,
    },
    /// Script attempted a forbidden operation or violated host sandbox invariants.
    #[error("script sandbox violation: {0}")]
    SandboxViolation(String),
    /// Relative module `require` failed resolution, containment, cycle, or export checks.
    #[error("script module error: {0}")]
    ModuleError(String),
    /// Host `simoder.props` query, alias lookup, type validation, or command write was rejected.
    #[error("script PROP API error: {0}")]
    PropApiError(String),
    /// Script patch reducer exceeded [`crate::config::MAX_PROP_OVERRIDES_PER_SCRIPT`].
    #[error("persistent script override limit exceeded: {actual} overrides exceed max {limit}")]
    PatchLimitExceeded {
        /// Configured persistent override ceiling.
        limit: usize,
        /// Attempted persistent override count.
        actual: usize,
    },
    /// Caller lacks required approval, live grant, or namespace ownership.
    #[error("permission denied: {0}")]
    PermissionDenied(String),
    /// Handle, proxy, or subscription outlived its owning mod generation or target context.
    #[error("stale handle or expired generation: {0}")]
    StaleHandle(String),
    /// Synchronous cross-VM invocation exceeded [`crate::config::MAX_CROSS_VM_REENTRY_DEPTH`].
    #[error("cross-VM re-entry depth exceeded max {max_depth}")]
    ReentryLimitExceeded {
        /// Maximum allowed nested hook hops.
        max_depth: usize,
    },
    /// Value shape (for example closure, coroutine, or native userdata) cannot cross VM boundaries.
    #[error("unsupported cross-VM value shape: {0}")]
    UnsupportedValue(String),
    /// Namespaced custom event registration, schema, or emission contract error.
    #[error("script event error: {0}")]
    EventError(String),
    /// DelayedCall clock, scheduling delay, owner quota, or cancellation failure.
    #[error("delayed call error: {0}")]
    DelayedCallError(String),
    /// Mod localization dictionary validation, fallback graph, or codec failure.
    #[error("script localization error: {0}")]
    LocalizationError(String),
    /// Scheduler capacity, slot state, or cross-thread lifecycle coordination failure.
    #[error("script scheduler error: {0}")]
    SchedulerError(String),
    /// Target VM has already stopped or closed.
    #[error("script runtime is closed: {0}")]
    RuntimeClosed(String),
}

impl From<CoreError> for ScriptError {
    fn from(err: CoreError) -> Self {
        Self::PropApiError(err.to_string())
    }
}

impl From<FormatError> for ScriptError {
    fn from(err: FormatError) -> Self {
        Self::LocalizationError(err.to_string())
    }
}

/// Scripting runtime language hosted inside an isolated [`crate::vm::ScriptVm`].
#[derive(
    Debug,
    Clone,
    Copy,
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
pub enum ScriptLanguage {
    /// Embedded Luau VM (`mlua`).
    Luau,
    /// Embedded QuickJS-NG ECMAScript runtime (`rquickjs`).
    #[strum(serialize = "js", to_string = "javascript")]
    JavaScript,
}

impl ScriptLanguage {
    /// Return the canonical lowercase language identifier.
    pub fn as_str(self) -> &'static str {
        self.into()
    }
}

/// Capabilities reported by `simoder.hasCapability(name)` inside sandboxed scripts.
#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    Hash,
    Serialize,
    DeserializeFromStr,
    EnumString,
    IntoStaticStr,
    VariantArray,
)]
#[strum(ascii_case_insensitive)]
pub enum ScriptCapability {
    /// Luau script execution (`"script.luau"`).
    #[strum(to_string = "script.luau")]
    #[serde(rename = "script.luau")]
    ScriptLuau,
    /// JavaScript / ES module execution (`"script.javascript"`).
    #[strum(to_string = "script.javascript", serialize = "script.js")]
    #[serde(rename = "script.javascript")]
    ScriptJavaScript,
    /// Lifecycle hooks (`onEnable`, `onTick`, `onDisable`) (`"script.lifecycle"`).
    #[strum(to_string = "script.lifecycle")]
    #[serde(rename = "script.lifecycle")]
    ScriptLifecycle,
    /// Attributed host logging via `simoder.log` (`"script.log"`).
    #[strum(to_string = "script.log")]
    #[serde(rename = "script.log")]
    ScriptLog,
    /// Scalar float32 property overrides and observations (`"game.props.float"`).
    #[strum(to_string = "game.props.float")]
    #[serde(rename = "game.props.float")]
    GamePropsFloat,
}

impl ScriptCapability {
    /// Return the canonical dot-delimited capability identifier.
    pub fn as_str(self) -> &'static str {
        self.into()
    }
}

/// Lifecycle execution phase emitted to host observers and guarded during `simoder.props` writes.
#[derive(
    Debug,
    Clone,
    Copy,
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
pub enum ScriptPhase {
    /// VM state allocation and host library installation.
    Create,
    /// Local module evaluation inside `require`.
    Require,
    /// Top-level entry chunk evaluation returning the lifecycle exports table/object.
    Entry,
    /// Initial `onEnable()` invocation inside a staged candidate VM.
    Enable,
    /// Periodic scheduler `onTick(deltaSeconds)` invocation.
    Tick,
    /// Explicit synchronous cross-VM hook or event callback execution.
    Callback,
    /// Final read-only `onDisable()` retirement invocation.
    Disable,
    /// VM teardown and memory reservation release.
    Close,
}

/// Error propagation mode governing uncaught hook/event faults.
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
pub enum FaultPolicy {
    /// Isolate failing mod listeners/ticks with diagnostics while keeping host stability (`"regular"`).
    #[default]
    Regular,
    /// Propagate faults loudly for strict developer diagnostics (`"dev"`).
    Dev,
}

/// Strictly non-zero host-assigned monotonic VM generation identity.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct VmGeneration(NonZeroU64);

impl VmGeneration {
    /// Construct a validated [`VmGeneration`], rejecting zero.
    pub fn new(raw: u64) -> Result<Self, ScriptError> {
        NonZeroU64::new(raw)
            .map(Self)
            .ok_or_else(|| ScriptError::InvalidConfig("VM generation must be non-zero".into()))
    }

    /// Return the underlying non-zero 64-bit generation number.
    pub const fn get(self) -> u64 {
        self.0.get()
    }
}

impl fmt::Display for VmGeneration {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0.get())
    }
}

/// Verified runtime owner identity pairing a canonical mod ID with its active [`VmGeneration`].
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct ModIdentity {
    /// Verified canonical mod identifier (for example `"author.better-roads"`).
    pub mod_id: String,
    /// Host-issued generation token distinguishing reloads of the same mod.
    pub generation: VmGeneration,
}

impl ModIdentity {
    /// Validate non-empty `mod_id` and bind `generation`.
    pub fn new(mod_id: impl Into<String>, generation: VmGeneration) -> Result<Self, ScriptError> {
        let mod_id = mod_id.into();
        if mod_id.is_empty() {
            return Err(ScriptError::InvalidConfig("mod_id must not be empty".into()));
        }
        Ok(Self { mod_id, generation })
    }
}

/// Resource, stack, and interrupt execution bounds configured for one script VM.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct ScriptLimits {
    /// Maximum heap allocation budget in bytes (`2 MiB..=64 MiB`, default `8 MiB`).
    pub memory_bytes: usize,
    /// Per-callback monotonic wall-clock timeout in milliseconds (`1..=20`, default `2`).
    pub budget_ms: u32,
    /// Maximum call-stack bytes for QuickJS VMs (`>= 64 KiB`, default `256 KiB`).
    pub js_max_stack_bytes: usize,
}

impl Default for ScriptLimits {
    fn default() -> Self {
        Self {
            memory_bytes: DEFAULT_VM_MEMORY_BYTES,
            budget_ms: DEFAULT_CALLBACK_BUDGET_MS,
            js_max_stack_bytes: DEFAULT_JS_MAX_STACK_BYTES,
        }
    }
}

impl ScriptLimits {
    /// Validate memory, callback timeout, and stack bounds against workspace safety ceilings.
    pub fn validate(&self) -> Result<(), ScriptError> {
        if !(MIN_VM_MEMORY_BYTES..=MAX_VM_MEMORY_BYTES).contains(&self.memory_bytes) {
            return Err(ScriptError::InvalidConfig(format!(
                "VM memory budget {} must be within {}..={} bytes",
                self.memory_bytes, MIN_VM_MEMORY_BYTES, MAX_VM_MEMORY_BYTES
            )));
        }
        if !(MIN_CALLBACK_BUDGET_MS..=MAX_CALLBACK_BUDGET_MS).contains(&self.budget_ms) {
            return Err(ScriptError::InvalidConfig(format!(
                "callback budget {} ms must be within {}..={} ms",
                self.budget_ms, MIN_CALLBACK_BUDGET_MS, MAX_CALLBACK_BUDGET_MS
            )));
        }
        if self.js_max_stack_bytes < 64 * 1024 {
            return Err(ScriptError::InvalidConfig(format!(
                "JS stack budget {} must be at least 65536 bytes",
                self.js_max_stack_bytes
            )));
        }
        Ok(())
    }
}

/// In-memory module payload bundled alongside an entry script under `scripts/`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ScriptModuleSource {
    /// Normalized POSIX module path relative to mod root (for example `"scripts/helper.luau"`).
    pub path: String,
    /// UTF-8 script source text.
    pub code: String,
}

/// Validated manifest property symbol alias exposed to `simoder.props`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PropertyAlias {
    /// Alias key declared in `mod.toml` `[symbols.properties]`.
    pub name: String,
    /// Resolved 32-bit FNV-1 PROP property hash.
    pub property_id: u32,
    /// Author-declared provenance source tag.
    pub source: String,
}

/// Self-contained script generation staged into a VM runtime.
#[derive(Debug, Clone)]
pub struct ScriptDefinition {
    /// Verified mod identifier owning this script generation.
    pub mod_id: String,
    /// Script engine language (`luau` or `javascript`).
    pub language: ScriptLanguage,
    /// Normalized relative POSIX path of the entry module (under `"scripts/"`).
    pub entry: String,
    /// UTF-8 entry script source text.
    pub entry_code: String,
    /// Additional in-memory modules resolvable via relative `require("./...")`.
    pub modules: Vec<ScriptModuleSource>,
    /// Memory, stack, and callback time limits.
    pub limits: ScriptLimits,
    /// Named resource target aliases mapping `"sanitizer"` -> [`Tgi`].
    pub resource_aliases: BTreeMap<String, Tgi>,
    /// Named property aliases mapping `"operatingCost"` -> [`PropertyAlias`].
    pub property_aliases: BTreeMap<String, PropertyAlias>,
    /// Distinguishes internal `simoder.base` bootstrap scripts from ordinary mod scripts.
    pub bootstrap_base: bool,
}

impl ScriptDefinition {
    /// Validate definition limits, non-empty entry code, and reserved base identity constraints.
    pub fn validate(&self) -> Result<(), ScriptError> {
        if self.mod_id.is_empty() {
            return Err(ScriptError::InvalidConfig("mod_id must not be empty".into()));
        }
        if self.mod_id == RESERVED_BASE_MOD_ID && !self.bootstrap_base {
            return Err(ScriptError::InvalidConfig(format!(
                "mod ID '{RESERVED_BASE_MOD_ID}' is reserved for bootstrap base runtimes"
            )));
        }
        self.limits.validate()?;
        normalize_contained_posix_path("", &self.entry, "scripts/")
            .map_err(ScriptError::InvalidConfig)?;
        if self.entry == "scripts/" {
            return Err(ScriptError::InvalidConfig(
                "entry module path must name a file under 'scripts/'".into(),
            ));
        }
        if self.entry_code.is_empty() || self.entry_code.len() > MAX_SCRIPT_SOURCE_BYTES {
            return Err(ScriptError::InvalidConfig(format!(
                "entry source length {} must be within 1..={} bytes",
                self.entry_code.len(),
                MAX_SCRIPT_SOURCE_BYTES
            )));
        }
        for module in &self.modules {
            normalize_contained_posix_path("", &module.path, "scripts/")
                .map_err(ScriptError::ModuleError)?;
            if module.path == "scripts/" {
                return Err(ScriptError::ModuleError(
                    "module path must name a file under 'scripts/'".into(),
                ));
            }
            if module.code.len() > MAX_SCRIPT_SOURCE_BYTES {
                return Err(ScriptError::ModuleError(format!(
                    "module '{}' exceeds {} bytes",
                    module.path, MAX_SCRIPT_SOURCE_BYTES
                )));
            }
        }
        Ok(())
    }
}

/// Discriminated mutation action queued by `simoder.props.setFloat` or `clearFloat`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum PropCommandAction {
    /// Queue an absolute finite IEEE-754 `f32` override.
    SetFloat(f32),
    /// Queue removal of this script's previous dynamic override on `(target, property_id)`.
    ClearFloat,
}

/// Buffered PROP mutation emitted by one successful script callback before transaction commit.
#[derive(Debug, Clone, PartialEq)]
pub struct ScriptPatchCommand {
    /// Target `0x00B1B104` PROP resource TGI.
    pub target: Tgi,
    /// Target 32-bit FNV-1 property identifier.
    pub property_id: u32,
    /// Queued mutation action (`SetFloat` or `ClearFloat`).
    pub action: PropCommandAction,
    /// Resolved symbol alias metadata when addressed through `[symbols.properties]`.
    pub symbol_origin: Option<PropertyAlias>,
    /// Default provenance source when no explicit alias source is bound.
    pub default_source: &'static str,
}

/// Immutable copied observation snapshot returned by `simoder.props.getFloat`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FloatObservation {
    /// Effective observed float value.
    pub value: f32,
    /// Original base-game float value before active mod patches.
    pub vanilla: f32,
    /// Observation timestamp in milliseconds since Unix epoch.
    pub observed_at_unix_ms: u64,
    /// Host registry generation when the snapshot was captured.
    pub generation: u64,
    /// Count of consistent game instances sharing this observed field value.
    pub instances: u32,
    /// Human-readable host observation status label (for example `"active"`).
    pub status: String,
}

/// Thread-safe host observation bridge supplying retained historical `PROP` values.
pub trait PropObserver: Send + Sync {
    /// Read one retained float observation for `(target, property_id)`, or `None` if unobserved.
    fn read_float(&self, target: Tgi, property_id: u32) -> Result<Option<FloatObservation>, ScriptError>;
}

/// Strongly typed host binding determining which `simoder.props` operations are admitted.
#[derive(Clone, Default)]
pub enum PropHostAccess {
    /// Standalone mode with no game PROP host; `hasCapability("game.props.float")` is `false`.
    #[default]
    Detached,
    /// Preparation staging mode; writes validate aliases/limits offline, reads are rejected.
    Preparation,
    /// Full attached runtime host backed by an authoritative [`PropObserver`].
    Attached(Arc<dyn PropObserver>),
}

impl fmt::Debug for PropHostAccess {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Detached => f.write_str("Detached"),
            Self::Preparation => f.write_str("Preparation"),
            Self::Attached(_) => f.write_str("Attached(<PropObserver>)"),
        }
    }
}

/// Lifecycle state machine of an isolated script VM without impossible flag combinations.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ScriptRuntimeState {
    /// Initialized and `onEnable()` succeeded; awaiting atomic scheduler commit.
    Staged,
    /// Committed and eligible for periodic `onTick` and hook/event dispatch.
    Running,
    /// Halted due to runtime error or resource violation; retains fault diagnostic.
    Faulted(String),
    /// Explicitly stopped via clean retirement (`onDisable` completed without error or fault diagnostic).
    Stopped {
        /// Optional diagnostic if `onDisable()` raised an error during shutdown.
        diagnostic: Option<String>,
    },
}

impl ScriptRuntimeState {
    /// Return `true` only when the VM is committed and running.
    pub const fn is_running(&self) -> bool {
        matches!(self, Self::Running)
    }

    /// Return any recorded fault or shutdown diagnostic message.
    pub fn diagnostic(&self) -> Option<&str> {
        match self {
            Self::Faulted(msg) => Some(msg.as_str()),
            Self::Stopped { diagnostic } => diagnostic.as_deref(),
            Self::Staged | Self::Running => None,
        }
    }
}

/// Non-blocking status snapshot published by a script VM for overlay and MCP diagnostics.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScriptStatusSnapshot {
    /// Owner mod identity (`mod_id` + non-zero generation).
    pub identity: ModIdentity,
    /// Underlying engine language.
    pub language: ScriptLanguage,
    /// Strongly typed current lifecycle state.
    pub state: ScriptRuntimeState,
    /// Live heap allocation reported by the VM in bytes.
    pub memory_bytes: usize,
    /// Declared heap budget reserved against the workspace aggregate pool.
    pub budget_bytes: usize,
    /// Saturating count of dropped log messages beyond [`crate::config::MAX_BUFFERED_LOGS`].
    pub dropped_logs: u64,
}
