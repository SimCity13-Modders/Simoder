//! Decoupled mod-provided MCP tool extension registry ([`HostToolRegistry`]) and `Simoder-OpenSCP`
//! debug-tools DBPF overlay builder ([`build_retail_debug_tools_overlay_package`]).

use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;
use std::sync::Arc;

pub use sc_properties::debug_tools::{
    DEBUG_TOOLS, DEFAULT_CATEGORY, DEFAULT_UI_CATEGORY, DebugToolPatch, HASH_PARENT,
    HASH_TOOL_CATEGORY, HASH_UI_TOOL_CATEGORY, LIVE_TOOL_PARENT, TOOL_GROUP,
    build_debug_tools_overlay,
};
use serde::{Deserialize, Serialize};
use serde_json::Value as JsonValue;
use simoder_formats::{Package, write_uncompressed_overlay};

use super::SdResult;
use crate::types::{ModError, is_valid_mod_id};

/// MCP tool annotations attached to a mod-registered or built-in tool descriptor.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModToolAnnotations {
    /// Whether the tool may perform destructive state changes.
    pub destructive_hint: bool,
    /// Whether repeated calls with the same arguments have no additional effect.
    pub idempotent_hint: bool,
    /// Whether the tool interacts with external open-world systems.
    pub open_world_hint: bool,
    /// Whether the tool only reads state without mutating the game or runtime.
    pub read_only_hint: bool,
}

impl ModToolAnnotations {
    /// Construct read-only, non-destructive annotations.
    pub const fn read_only(idempotent: bool) -> Self {
        Self {
            destructive_hint: false,
            idempotent_hint: idempotent,
            open_world_hint: false,
            read_only_hint: true,
        }
    }

    /// Construct state-mutating tool annotations.
    pub const fn mutating(idempotent: bool, destructive: bool) -> Self {
        Self {
            destructive_hint: destructive,
            idempotent_hint: idempotent,
            open_world_hint: false,
            read_only_hint: false,
        }
    }
}

/// Decoupled MCP tool descriptor registered by any mod (Luau, JS, or Native `.dll`) into [`HostToolRegistry`].
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ModToolDescriptor {
    /// Host-verified owning mod identifier (for example `"author.traffic-advisor"`).
    pub owner_mod_id: String,
    /// Host-verified owning mod generation counter.
    pub owner_generation: u64,
    /// Canonical namespaced tool name (`"<owner_mod_id>.<short_name>"`).
    pub name: String,
    /// Human-readable description of the tool's purpose and constraints.
    pub description: String,
    /// JSON Schema object describing accepted tool arguments.
    pub input_schema: JsonValue,
    /// Optional JSON Schema object describing structured output.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output_schema: Option<JsonValue>,
    /// Required permission grant identifiers checked before invocation.
    pub required_grants: Vec<String>,
    /// MCP tool behavioral annotations.
    pub annotations: ModToolAnnotations,
}

/// Callback handler invoked when an authorized MCP client executes a mod-registered tool.
pub type ModToolHandler =
    Arc<dyn Fn(&JsonValue) -> Result<JsonValue, String> + Send + Sync + 'static>;

struct RegisteredModTool {
    descriptor: ModToolDescriptor,
    handler: ModToolHandler,
}

/// Host-side registry allowing mods to register optional MCP tools without taking a hard dependency
/// on the `simoder.devtools.mcp` native mod `.dll` (`ADR 0001`).
#[derive(Default)]
pub struct HostToolRegistry {
    tools: BTreeMap<String, RegisteredModTool>,
    active_generations: BTreeMap<String, u64>,
}

impl HostToolRegistry {
    /// Construct an empty [`HostToolRegistry`].
    pub fn new() -> Self {
        Self::default()
    }

    /// Record the currently active `generation` for `mod_id` in the host runtime.
    pub fn set_active_mod_generation(&mut self, mod_id: &str, generation: u64) {
        self.active_generations.insert(mod_id.to_string(), generation);
    }

    /// Register a namespaced tool owned by `(owner_mod_id, owner_generation)`.
    ///
    /// Succeeds even when no MCP DevTool mod is installed, keeping mods decoupled from DevTools.
    pub fn register_tool(
        &mut self,
        mut descriptor: ModToolDescriptor,
        handler: ModToolHandler,
    ) -> Result<(), ModError> {
        if !is_valid_mod_id(&descriptor.owner_mod_id) || descriptor.owner_generation == 0 {
            return Err(ModError::InvalidModId(descriptor.owner_mod_id));
        }
        let expected_prefix = format!("{}.", descriptor.owner_mod_id);
        let canonical_name = if descriptor.name.starts_with(&expected_prefix) {
            descriptor.name.clone()
        } else {
            format!("{expected_prefix}{}", descriptor.name.trim())
        };
        if canonical_name.len() <= expected_prefix.len() {
            return Err(ModError::ManifestValidation(
                "registered MCP tool name cannot be empty".into(),
            ));
        }
        if self.tools.contains_key(&canonical_name) {
            return Err(ModError::ManifestValidation(format!(
                "duplicate MCP tool registration '{canonical_name}'"
            )));
        }
        descriptor.name = canonical_name.clone();
        self.active_generations
            .entry(descriptor.owner_mod_id.clone())
            .or_insert(descriptor.owner_generation);
        self.tools.insert(
            canonical_name,
            RegisteredModTool {
                descriptor,
                handler,
            },
        );
        Ok(())
    }

    /// Remove all registered tools belonging to `(mod_id, generation)` on mod unload/disable/reload.
    pub fn revoke_mod_generation(&mut self, mod_id: &str, generation: u64) -> usize {
        if self.active_generations.get(mod_id).copied() == Some(generation) {
            self.active_generations.remove(mod_id);
        }
        let before = self.tools.len();
        self.tools.retain(|_, entry| {
            !(entry.descriptor.owner_mod_id == mod_id
                && entry.descriptor.owner_generation == generation)
        });
        before - self.tools.len()
    }

    /// Return all currently active mod-registered tool descriptors in deterministic alphabetical order.
    pub fn list_tools(&self) -> Vec<ModToolDescriptor> {
        self.tools
            .values()
            .filter(|entry| {
                self.active_generations
                    .get(&entry.descriptor.owner_mod_id)
                    .copied()
                    == Some(entry.descriptor.owner_generation)
            })
            .map(|entry| entry.descriptor.clone())
            .collect()
    }

    /// Resolve and authorize a mod-registered tool handler without holding the registry lock
    /// while executing the callback (preventing AB-BA deadlocks with UI-thread reloads).
    pub fn resolve_tool_handler(
        &self,
        tool_name: &str,
        granted_permissions: &BTreeSet<String>,
    ) -> Result<ModToolHandler, SdResult> {
        let Some(entry) = self.tools.get(tool_name) else {
            return Err(SdResult::Invalid);
        };
        if self
            .active_generations
            .get(&entry.descriptor.owner_mod_id)
            .copied()
            != Some(entry.descriptor.owner_generation)
        {
            return Err(SdResult::Stale);
        }
        if entry
            .descriptor
            .required_grants
            .iter()
            .any(|req| !granted_permissions.contains(req))
        {
            return Err(SdResult::Denied);
        }
        Ok(Arc::clone(&entry.handler))
    }

    /// Invoke a mod-registered tool by `tool_name` after verifying its owning `(mod_id, generation)`
    /// is still active and `granted_permissions` satisfies all `required_grants`.
    pub fn invoke_tool(
        &self,
        tool_name: &str,
        arguments: &JsonValue,
        granted_permissions: &BTreeSet<String>,
    ) -> Result<JsonValue, SdResult> {
        let handler = self.resolve_tool_handler(tool_name, granted_permissions)?;
        (handler)(arguments).map_err(|_| SdResult::OperationFailed)
    }
}

/// Build an uncompressed DBPF overlay package unlocking SimCity's three retail development tools
/// (`0x53B77423`, `0xA2AA4268`, `0xB65E1F2E`) by reusing `Simoder-OpenSCP` [`build_debug_tools_overlay`].
pub fn build_retail_debug_tools_overlay_package(
    base_package_path: &Path,
    category: Option<u32>,
    ui_category: Option<u32>,
) -> Result<(Vec<u8>, Vec<DebugToolPatch>), ModError> {
    let package = Package::open(base_package_path).map_err(|e| ModError::Format(e.into()))?;
    let (entries, patches) = build_debug_tools_overlay(
        &package,
        category.unwrap_or(DEFAULT_CATEGORY),
        ui_category.unwrap_or(DEFAULT_UI_CATEGORY),
    )
    .map_err(ModError::PackageValidation)?;
    let dbpf_bytes =
        write_uncompressed_overlay(&entries).map_err(|e| ModError::Format(e.into()))?;
    Ok((dbpf_bytes, patches))
}
