//! Shared host API backend (`HostBridge`), RAII callback/import guards, and argument validators
//! shared between the Luau (`mlua`) and JavaScript (`rquickjs`) script runtimes.

use std::collections::{BTreeMap, BTreeSet};
use std::sync::Arc;
use std::time::Instant;

use parking_lot::Mutex;
use simoder_core::{Tgi, parse_tgi};

use crate::config::{
    MAX_BUFFERED_LOGS, MAX_DIAGNOSTIC_BYTES, MAX_LOG_MESSAGE_BYTES, MAX_PROP_COMMANDS_PER_CALLBACK,
    MAX_PROP_REF_BYTES, PROP_RESOURCE_TYPE_ID,
};
use crate::types::{
    FloatObservation, ModIdentity, PropCommandAction, PropHostAccess, PropertyAlias,
    ScriptCapability, ScriptDefinition, ScriptError, ScriptLanguage, ScriptPatchCommand,
    ScriptRuntimeState, normalize_contained_posix_path,
};

/// Typed property argument accepted by `simoder.props` (`numeric u32`, hex string, or catalog alias).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PropertyKey {
    /// Exact numeric 32-bit FNV-1 property hash.
    Numeric(u32),
    /// String alias name (or `0x`-prefixed hexadecimal ID).
    Text(String),
}

pub(crate) struct HostBridgeMutableState {
    pub(crate) runtime_state: ScriptRuntimeState,
    pub(crate) logs: Vec<String>,
    pub(crate) dropped_logs: u64,
    pub(crate) commands: Vec<ScriptPatchCommand>,
    pub(crate) loading_modules: BTreeSet<String>,
    pub(crate) in_disable: bool,
}

/// Shared host API backend backing both Luau (`mlua`) and JavaScript (`rquickjs`) mod VMs.
pub struct HostBridge {
    pub(crate) identity: ModIdentity,
    pub(crate) language: ScriptLanguage,
    pub(crate) bootstrap_base: bool,
    pub(crate) default_source_origin: &'static str,
    pub(crate) host_access: PropHostAccess,
    pub(crate) budget_bytes: usize,
    pub(crate) modules: BTreeMap<String, String>,
    pub(crate) resource_aliases: BTreeMap<String, Tgi>,
    pub(crate) property_aliases: BTreeMap<String, PropertyAlias>,
    started_at: Instant,
    pub(crate) mutable: Mutex<HostBridgeMutableState>,
}

impl HostBridge {
    /// Construct a new [`HostBridge`] in [`ScriptRuntimeState::Staged`] state.
    pub fn new(
        identity: ModIdentity,
        definition: &ScriptDefinition,
        host_access: PropHostAccess,
        default_source_origin: &'static str,
    ) -> Result<Arc<Self>, ScriptError> {
        definition.validate()?;
        let mut modules = BTreeMap::from([(definition.entry.clone(), definition.entry_code.clone())]);
        for module in &definition.modules {
            if modules.insert(module.path.clone(), module.code.clone()).is_some() {
                return Err(ScriptError::ModuleError(format!(
                    "duplicate module definition '{}'",
                    module.path
                )));
            }
        }
        Ok(Arc::new(Self {
            identity,
            language: definition.language,
            bootstrap_base: definition.bootstrap_base,
            default_source_origin,
            host_access,
            budget_bytes: definition.limits.memory_bytes,
            modules,
            resource_aliases: definition.resource_aliases.clone(),
            property_aliases: definition.property_aliases.clone(),
            started_at: Instant::now(),
            mutable: Mutex::new(HostBridgeMutableState {
                runtime_state: ScriptRuntimeState::Staged,
                logs: Vec::new(),
                dropped_logs: 0,
                commands: Vec::new(),
                loading_modules: BTreeSet::new(),
                in_disable: false,
            }),
        }))
    }

    /// Elapsed monotonic wall seconds since VM creation (`simoder.time()`).
    pub fn elapsed_seconds(&self) -> f64 {
        self.started_at.elapsed().as_secs_f64()
    }

    /// Record a bounded `simoder.log(message)` entry (`<= 2048 B`, buffering up to `128`).
    pub fn log_message(&self, message: &str) -> Result<(), ScriptError> {
        if message.len() > MAX_LOG_MESSAGE_BYTES {
            return Err(ScriptError::SandboxViolation(format!(
                "Simoder log message exceeds {MAX_LOG_MESSAGE_BYTES} bytes"
            )));
        }
        let mut state = self.mutable.lock();
        if state.logs.len() < MAX_BUFFERED_LOGS {
            state.logs.push(message.to_string());
        } else {
            state.dropped_logs = state.dropped_logs.saturating_add(1);
        }
        Ok(())
    }

    /// Query `simoder.hasCapability(name)` against this VM's language and bound host.
    pub fn has_capability(&self, name: &str) -> bool {
        let Ok(cap) = name.parse::<ScriptCapability>() else {
            return false;
        };
        match cap {
            ScriptCapability::ScriptLuau => self.language == ScriptLanguage::Luau,
            ScriptCapability::ScriptJavaScript => self.language == ScriptLanguage::JavaScript,
            ScriptCapability::ScriptLifecycle | ScriptCapability::ScriptLog => true,
            ScriptCapability::GamePropsFloat => {
                matches!(
                    self.host_access,
                    PropHostAccess::Preparation | PropHostAccess::Attached(_)
                )
            }
        }
    }

    /// Read a copied historical float snapshot via `simoder.props.getFloat`.
    pub fn get_float(
        &self,
        resource: &str,
        property: PropertyKey,
    ) -> Result<Option<FloatObservation>, ScriptError> {
        if self.bootstrap_base {
            return Err(ScriptError::PropApiError(
                "bootstrap runtime does not expose mod PROP APIs".into(),
            ));
        }
        let observer = match &self.host_access {
            PropHostAccess::Detached => {
                return Err(ScriptError::PropApiError(
                    "Game PROP API is unavailable in this host".into(),
                ));
            }
            PropHostAccess::Preparation => {
                return Err(ScriptError::PropApiError(
                    "Live PROP reads are unavailable during preparation".into(),
                ));
            }
            PropHostAccess::Attached(observer) => observer,
        };
        let (target, property_id, _) = self.resolve_prop_target(resource, property)?;
        observer.read_float(target, property_id)
    }

    /// Enqueue a `setFloat` or `clearFloat` mutation for emission on successful callback completion.
    pub fn queue_prop_command(
        &self,
        resource: &str,
        property: PropertyKey,
        action: PropCommandAction,
    ) -> Result<(), ScriptError> {
        if self.bootstrap_base {
            return Err(ScriptError::PropApiError(
                "bootstrap runtime does not expose mod PROP APIs".into(),
            ));
        }
        if matches!(self.host_access, PropHostAccess::Detached) {
            return Err(ScriptError::PropApiError(
                "PROP writes require an attached or preparation host".into(),
            ));
        }
        if let PropCommandAction::SetFloat(val) = action
            && !val.is_finite()
        {
            return Err(ScriptError::PropApiError(
                "Expected finite float32 value".into(),
            ));
        }
        let (target, property_id, symbol_origin) = self.resolve_prop_target(resource, property)?;
        let mut state = self.mutable.lock();
        if state.in_disable
            || matches!(
                state.runtime_state,
                ScriptRuntimeState::Faulted(_) | ScriptRuntimeState::Stopped { .. }
            )
        {
            return Err(ScriptError::PropApiError(
                "PROP writes are forbidden during onDisable or after retirement".into(),
            ));
        }
        if state.commands.len() >= MAX_PROP_COMMANDS_PER_CALLBACK {
            return Err(ScriptError::PropApiError(format!(
                "At most {MAX_PROP_COMMANDS_PER_CALLBACK} PROP commands per callback"
            )));
        }
        state.commands.push(ScriptPatchCommand {
            target,
            property_id,
            action,
            symbol_origin,
            default_source: self.default_source_origin,
        });
        Ok(())
    }

    fn resolve_prop_target(
        &self,
        resource: &str,
        property: PropertyKey,
    ) -> Result<(Tgi, u32, Option<PropertyAlias>), ScriptError> {
        if resource.len() > MAX_PROP_REF_BYTES {
            return Err(ScriptError::PropApiError(format!(
                "PROP resource reference exceeds {MAX_PROP_REF_BYTES} bytes"
            )));
        }
        let target = match self.resource_aliases.get(resource) {
            Some(aliased) => *aliased,
            None => parse_script_tgi(resource)?,
        };
        if target.type_id != PROP_RESOURCE_TYPE_ID {
            return Err(ScriptError::PropApiError(format!(
                "Expected PROP resource type {PROP_RESOURCE_TYPE_ID:#010X}; got {target}"
            )));
        }
        match property {
            PropertyKey::Numeric(id) => Ok((target, id, None)),
            PropertyKey::Text(name) => {
                if let Some(alias) = self.property_aliases.get(&name) {
                    return Ok((target, alias.property_id, Some(alias.clone())));
                }
                let trimmed = name
                    .strip_prefix("0x")
                    .or_else(|| name.strip_prefix("0X"))
                    .unwrap_or(&name);
                if trimmed.is_empty() || trimmed.len() > 8 {
                    return Err(ScriptError::PropApiError(format!(
                        "Unknown property alias or invalid hex property ID '{name}'"
                    )));
                }
                let id = u32::from_str_radix(trimmed, 16).map_err(|_| {
                    ScriptError::PropApiError(format!(
                        "Unknown property alias or invalid hex property ID '{name}'"
                    ))
                })?;
                Ok((target, id, None))
            }
        }
    }

    /// Resolve a relative `./` or `../` module reference inside in-memory `scripts/` modules.
    pub fn resolve_module_path(
        &self,
        caller_path: &str,
        requested: &str,
        extensions: &[&str],
        default_ext: &str,
    ) -> Result<String, ScriptError> {
        resolve_relative_module_path(&self.modules, caller_path, requested, extensions, default_ext)
    }

    /// Enter cyclic import tracking for `module_path`, automatically unwinding on drop.
    pub fn begin_module_load(&self, module_path: &str) -> Result<ModuleLoadGuard<'_>, ScriptError> {
        if !self.mutable.lock().loading_modules.insert(module_path.to_string()) {
            return Err(ScriptError::ModuleError(format!(
                "cyclic module dependency involving '{module_path}'"
            )));
        }
        Ok(ModuleLoadGuard {
            bridge: self,
            path: module_path.to_string(),
        })
    }

    /// Checkpoint queued PROP commands before running a callback; rolls back automatically on error.
    pub fn begin_callback(&self) -> CallbackCommandGuard<'_> {
        CallbackCommandGuard {
            bridge: self,
            checkpoint: self.mutable.lock().commands.len(),
            committed: false,
        }
    }

    /// Transition VM from [`ScriptRuntimeState::Staged`] to [`ScriptRuntimeState::Running`].
    pub fn commit(&self) -> Result<(), ScriptError> {
        let mut state = self.mutable.lock();
        if !matches!(state.runtime_state, ScriptRuntimeState::Staged) {
            return Err(ScriptError::SchedulerError(format!(
                "mod '{}' cannot commit from non-staged state {:?}",
                self.identity.mod_id, state.runtime_state
            )));
        }
        state.runtime_state = ScriptRuntimeState::Running;
        Ok(())
    }

    /// Mark VM faulted with a bounded diagnostic and clear unsubmitted PROP commands.
    pub fn mark_faulted(&self, diagnostic: &str) {
        let mut state = self.mutable.lock();
        state.commands.clear();
        state.runtime_state = ScriptRuntimeState::Faulted(truncate_diagnostic(diagnostic));
    }

    pub(crate) fn enter_disable(&self) -> bool {
        let mut state = self.mutable.lock();
        if matches!(state.runtime_state, ScriptRuntimeState::Stopped { .. }) {
            return false;
        }
        let was_committed = matches!(
            state.runtime_state,
            ScriptRuntimeState::Running | ScriptRuntimeState::Faulted(_)
        );
        state.in_disable = true;
        state.commands.clear();
        was_committed
    }

    pub(crate) fn finish_disable(&self, error: Option<String>) {
        let mut state = self.mutable.lock();
        state.in_disable = false;
        state.commands.clear();
        let diagnostic = error.map(|msg| truncate_diagnostic(&msg)).or_else(|| {
            match &state.runtime_state {
                ScriptRuntimeState::Faulted(existing) => Some(existing.clone()),
                _ => None,
            }
        });
        state.runtime_state = ScriptRuntimeState::Stopped { diagnostic };
    }

    /// Drain unread buffered log lines once committed or stopped.
    pub fn drain_logs(&self) -> Vec<String> {
        let mut state = self.mutable.lock();
        if matches!(state.runtime_state, ScriptRuntimeState::Staged) {
            return Vec::new();
        }
        std::mem::take(&mut state.logs)
    }

    /// Take pending PROP mutation commands emitted by completed callbacks.
    pub fn take_patch_commands(&self) -> Vec<ScriptPatchCommand> {
        let mut state = self.mutable.lock();
        if matches!(state.runtime_state, ScriptRuntimeState::Staged) {
            return Vec::new();
        }
        std::mem::take(&mut state.commands)
    }
}

/// RAII guard cleaning up an active module load path even when evaluation errors.
pub struct ModuleLoadGuard<'a> {
    bridge: &'a HostBridge,
    path: String,
}

impl Drop for ModuleLoadGuard<'_> {
    fn drop(&mut self) {
        self.bridge.mutable.lock().loading_modules.remove(&self.path);
    }
}

/// RAII checkpoint guard truncating un-committed PROP commands back to entry length on error.
pub struct CallbackCommandGuard<'a> {
    bridge: &'a HostBridge,
    checkpoint: usize,
    committed: bool,
}

impl CallbackCommandGuard<'_> {
    /// Retain commands written during this callback (called only on callback success).
    pub fn keep(mut self) {
        self.committed = true;
    }
}

impl Drop for CallbackCommandGuard<'_> {
    fn drop(&mut self) {
        if !self.committed {
            self.bridge.mutable.lock().commands.truncate(self.checkpoint);
        }
    }
}

/// Shared float64-to-uint32 exact integer validator used across Luau and JS PROP bindings.
pub(crate) fn validate_u32_number(num: f64) -> Result<u32, ScriptError> {
    if !num.is_finite() || num < 0.0 || num > f64::from(u32::MAX) || num.fract() != 0.0 {
        return Err(ScriptError::PropApiError(
            "Property ID must be exact uint32".into(),
        ));
    }
    Ok(num as u32)
}

/// Shared float64-to-finite-f32 validator used across Luau and JS PROP bindings.
pub(crate) fn validate_finite_f32(num: f64) -> Result<f32, ScriptError> {
    if !num.is_finite() || num.abs() > f64::from(f32::MAX) {
        return Err(ScriptError::PropApiError(
            "Expected finite float32 value".into(),
        ));
    }
    Ok(num as f32)
}

/// Truncate diagnostics to [`MAX_DIAGNOSTIC_BYTES`] respecting UTF-8 char boundaries.
pub fn truncate_diagnostic(raw: &str) -> String {
    if raw.len() <= MAX_DIAGNOSTIC_BYTES {
        return raw.to_string();
    }
    let mut end = MAX_DIAGNOSTIC_BYTES;
    while end > 0 && !raw.is_char_boundary(end) {
        end -= 1;
    }
    raw[..end].to_string()
}

/// Parse standard 8-hex TGI or 16-hex authoring PROP instance spelling when high 32 bits are zero.
pub fn parse_script_tgi(raw: &str) -> Result<Tgi, ScriptError> {
    if let Ok(tgi) = parse_tgi(raw) {
        return Ok(tgi);
    }
    let Some(last_colon) = raw.rfind(':') else {
        return Err(ScriptError::PropApiError(format!(
            "Expected PROP TYPE:GROUP:INSTANCE, got '{raw}'"
        )));
    };
    let inst_raw = raw[last_colon + 1..].trim();
    let hex_digits = inst_raw
        .strip_prefix("0x")
        .or_else(|| inst_raw.strip_prefix("0X"))
        .unwrap_or(inst_raw);
    if hex_digits.is_empty()
        || hex_digits.len() > 16
        || !hex_digits.bytes().all(|b| b.is_ascii_hexdigit())
    {
        return Err(ScriptError::PropApiError(format!(
            "Expected PROP TYPE:GROUP:INSTANCE, got '{raw}'"
        )));
    }
    let full = u64::from_str_radix(hex_digits, 16)
        .map_err(|_| ScriptError::PropApiError(format!("Expected hexadecimal PROP instance in '{raw}'")))?;
    let instance_u32 = u32::try_from(full).map_err(|_| {
        ScriptError::PropApiError(format!(
            "Nonzero high instance bits are unsupported in PROP target '{raw}'"
        ))
    })?;
    let mut prefix = parse_tgi(&format!("{}0", &raw[..=last_colon]))?;
    prefix.instance = instance_u32;
    Ok(prefix)
}

/// Normalize relative POSIX `require("./..." | "../...")` imports via [`relative_path`] without FS access.
pub fn resolve_relative_module_path(
    modules: &BTreeMap<String, String>,
    caller_path: &str,
    requested: &str,
    extensions: &[&str],
    default_ext: &str,
) -> Result<String, ScriptError> {
    if !requested.starts_with("./") && !requested.starts_with("../") {
        return Err(ScriptError::ModuleError(format!(
            "require expects a relative POSIX path starting with './' or '../' (got '{requested}')"
        )));
    }
    let caller_dir = caller_path.rsplit_once('/').map_or("", |(dir, _)| dir);
    let candidate_base = normalize_contained_posix_path(caller_dir, requested, "scripts/")
        .map_err(ScriptError::ModuleError)?;

    let has_known_ext = extensions
        .iter()
        .any(|ext| candidate_base.ends_with(&format!(".{ext}")));
    let candidates = if has_known_ext {
        vec![candidate_base]
    } else {
        let mut out = vec![format!("{candidate_base}{default_ext}")];
        for ext in extensions {
            let suffixed = format!("{candidate_base}.{ext}");
            if !out.contains(&suffixed) {
                out.push(suffixed);
            }
        }
        out
    };

    candidates
        .into_iter()
        .find(|path| modules.contains_key(path))
        .ok_or_else(|| {
            ScriptError::ModuleError(format!(
                "local module '{requested}' not found from '{caller_path}'"
            ))
        })
}
