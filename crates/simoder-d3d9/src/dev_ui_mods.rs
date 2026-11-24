#![forbid(unsafe_code)]

//! Generic Dev UI script mod discovery, SHA-256 trust verification, and [`HostToolRegistry`]
//! registration (`native_trust.toon` + `Mods UI` live consent).
//!
//! Discovers optional developer UI script mods (such as `simoder.ui_inspector`) under `<mods_root>`
//! without requiring a native `.dll` or editing `config.toon`. Because `dev_ui/*.js` scripts have
//! direct access to the `EAWebKit` DOM outside the declarative `UiKit` sandbox, they are gated by
//! explicit SHA-256-pinned user approval in `Mods UI` (`native_trust.toon`) and activate live upon
//! approval without restarting the game.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::UNIX_EPOCH;

use serde::Deserialize;
use serde_json::{Value as JsonValue, json};
use simoder_core::{Digest, Sha256, hex};
use simoder_mods::{
    HostToolRegistry, KeyChord, KeybindContext, KeybindDescriptor, KeybindRegistry,
    ModToolAnnotations, ModToolDescriptor, ModToolHandler, load_mod_definition,
    reject_reparse_or_symlink, resolve_safe_relative_path,
};

const MAX_DEV_UI_TOOLS_PER_MOD: usize = 32;
const MAX_DEV_UI_KEYBINDS_PER_MOD: usize = 32;
const MAX_DEV_UI_TOOL_JSON_BYTES: usize = 64 * 1024;
const MAX_DEV_UI_SCRIPT_BYTES: usize = 1024 * 1024;
const DEV_UI_DIR_NAME: &str = "dev_ui";
const DEV_UI_TOOLS_FILE_NAME: &str = "mcp_tools.json";
const DEV_UI_KEYBINDS_FILE_NAME: &str = "keybinds.json";

/// One optional MCP tool entry declared in a Dev UI mod's `dev_ui/mcp_tools.json`.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DevUiToolManifestEntry {
    /// Short tool name (automatically namespaced as `"<mod_id>.<name>"`).
    pub name: String,
    /// Human-readable description shown in `tools/list`.
    pub description: String,
    /// Operation name dispatched inside the JS packet (`{"op": op, "args": args}`).
    pub op: String,
    /// JSON Schema object describing accepted tool arguments.
    pub input_schema: JsonValue,
    /// Required permission grant identifiers enforced by `simoder-mcp`.
    #[serde(default)]
    pub required_grants: Vec<String>,
    /// MCP tool behavioral annotations.
    pub annotations: ModToolAnnotations,
}

/// One optional keybind entry declared in a Dev UI mod's `dev_ui/keybinds.json`.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DevUiKeybindManifestEntry {
    /// Short action identifier within the mod (e.g. `"toggle_panel"`).
    pub id: String,
    /// Human-readable title displayed in the KeyBinds UI.
    pub title: String,
    /// Description of what the keybind triggers.
    #[serde(default)]
    pub description: String,
    /// Context in which the shortcut is active (defaults to `"global"`).
    #[serde(default = "default_keybind_context")]
    pub context: String,
    /// Optional default chord string (e.g. `"Ctrl+Alt+I"`).
    #[serde(default)]
    pub default_key: Option<String>,
}

fn default_keybind_context() -> String {
    "global".to_string()
}

/// Discovered Dev UI script mod eligible for `Mods UI` trust approval and hot-reload.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DevUiModDescriptor {
    /// Canonical mod identifier from `mod.toml`.
    pub mod_id: String,
    /// Human-readable mod name from `mod.toml`.
    pub name: String,
    /// Mod version string from `mod.toml`.
    pub version: String,
    /// Relative POSIX script path declared in `mod.toml` (e.g. `"dev_ui/inspector.js"`).
    pub entry: String,
    /// Absolute path to the validated ES5 UI script (`dev_ui/*.js`).
    pub script_path: PathBuf,
    /// Optional path to `dev_ui/mcp_tools.json`.
    pub tools_path: Option<PathBuf>,
    /// Optional path to `dev_ui/keybinds.json`.
    pub keybinds_path: Option<PathBuf>,
}

/// Parse and validate `dev_ui/keybinds.json` entries.
pub fn load_dev_ui_keybind_manifest(
    raw_json: &str,
) -> Result<Vec<DevUiKeybindManifestEntry>, String> {
    if raw_json.len() > MAX_DEV_UI_TOOL_JSON_BYTES {
        return Err("dev_ui/keybinds.json exceeds 64 KiB limit".to_string());
    }
    let entries: Vec<DevUiKeybindManifestEntry> =
        serde_json::from_str(raw_json).map_err(|e| format!("invalid keybinds.json: {e}"))?;
    if entries.len() > MAX_DEV_UI_KEYBINDS_PER_MOD {
        return Err(format!(
            "dev_ui/keybinds.json exceeds {MAX_DEV_UI_KEYBINDS_PER_MOD} keybind limit"
        ));
    }
    for entry in &entries {
        if entry.id.trim().is_empty() || entry.title.trim().is_empty() {
            return Err("dev_ui keybind entry requires non-empty id and title".to_string());
        }
    }
    Ok(entries)
}

/// Register a Dev UI mod's declared keybinds in [`KeybindRegistry`].
pub fn register_dev_ui_keybinds(
    registry: &mut KeybindRegistry,
    mod_id: &str,
    entries: &[DevUiKeybindManifestEntry],
) -> Result<usize, String> {
    let mut count = 0usize;
    for entry in entries {
        let default_chord = match entry
            .default_key
            .as_deref()
            .map(str::trim)
            .filter(|s| !s.is_empty())
        {
            Some(raw) => Some(KeyChord::parse(raw)?),
            None => None,
        };
        let desc = KeybindDescriptor {
            owner_mod_id: mod_id.to_string(),
            action_id: entry.id.clone(),
            title: entry.title.clone(),
            description: entry.description.clone(),
            context: KeybindContext::parse(&entry.context)?,
            default_chord,
        };
        registry.register(desc)?;
        count += 1;
    }
    Ok(count)
}

/// Parse and validate `dev_ui/mcp_tools.json` entries.
pub fn load_dev_ui_tool_manifest(raw_json: &str) -> Result<Vec<DevUiToolManifestEntry>, String> {
    if raw_json.len() > MAX_DEV_UI_TOOL_JSON_BYTES {
        return Err("dev_ui/mcp_tools.json exceeds 64 KiB limit".to_string());
    }
    let entries: Vec<DevUiToolManifestEntry> =
        serde_json::from_str(raw_json).map_err(|e| format!("invalid mcp_tools.json: {e}"))?;
    if entries.len() > MAX_DEV_UI_TOOLS_PER_MOD {
        return Err(format!(
            "dev_ui/mcp_tools.json exceeds {MAX_DEV_UI_TOOLS_PER_MOD} tool limit"
        ));
    }
    for entry in &entries {
        if entry.name.trim().is_empty()
            || entry.op.trim().is_empty()
            || entry.description.trim().is_empty()
        {
            return Err("dev_ui tool entry requires non-empty name, op, and description".to_string());
        }
    }
    Ok(entries)
}

/// Discover all valid Dev UI script mods under `mods_root` (`dev_ui/*.js` + `mod.toml`).
pub fn discover_dev_ui_mods(mods_root: &Path) -> Vec<DevUiModDescriptor> {
    let Ok(read_dir) = fs::read_dir(mods_root) else {
        return Vec::new();
    };
    let mut found = Vec::new();
    for entry in read_dir.flatten() {
        let mod_dir = entry.path();
        if !mod_dir.is_dir() || reject_reparse_or_symlink(&mod_dir).is_err() {
            continue;
        }
        let manifest_path = mod_dir.join("mod.toml");
        if !manifest_path.is_file() {
            continue;
        }
        let Ok(def) = load_mod_definition(&mod_dir) else {
            continue;
        };
        let Some(script_rel) = def
            .scripts()
            .map(|s| s.entry.trim())
            .filter(|p| p.ends_with(".js"))
        else {
            continue;
        };
        let Ok(script_path) = resolve_safe_relative_path(&mod_dir, script_rel, Some("dev_ui/"))
        else {
            continue;
        };
        let tools_candidate = mod_dir.join(DEV_UI_DIR_NAME).join(DEV_UI_TOOLS_FILE_NAME);
        let tools_path = (tools_candidate.is_file()
            && reject_reparse_or_symlink(&tools_candidate).is_ok())
        .then_some(tools_candidate);
        let keybinds_candidate = mod_dir.join(DEV_UI_DIR_NAME).join(DEV_UI_KEYBINDS_FILE_NAME);
        let keybinds_path = (keybinds_candidate.is_file()
            && reject_reparse_or_symlink(&keybinds_candidate).is_ok())
        .then_some(keybinds_candidate);

        let entry = script_rel.to_string();
        found.push(DevUiModDescriptor {
            mod_id: def.manifest.id,
            name: def.manifest.name,
            version: def.manifest.version,
            entry,
            script_path,
            tools_path,
            keybinds_path,
        });
    }
    found.sort_by(|a, b| a.mod_id.cmp(&b.mod_id));
    found
}

/// Register a Dev UI mod's tools in [`HostToolRegistry`] under `(mod_id, generation)`.
pub fn register_dev_ui_tools<F>(
    registry: &mut HostToolRegistry,
    mod_id: &str,
    generation: u64,
    entries: &[DevUiToolManifestEntry],
    dispatch: F,
) -> Result<usize, String>
where
    F: Fn(&str, &JsonValue) -> Result<JsonValue, String> + Send + Sync + 'static,
{
    registry.set_active_mod_generation(mod_id, generation);
    let shared_dispatch = Arc::new(dispatch);
    let mut count = 0usize;
    for entry in entries {
        let op = entry.op.clone();
        let d = Arc::clone(&shared_dispatch);
        let handler: ModToolHandler = Arc::new(move |args: &JsonValue| (d)(&op, args));
        let descriptor = ModToolDescriptor {
            owner_mod_id: mod_id.to_string(),
            owner_generation: generation,
            name: entry.name.clone(),
            description: entry.description.clone(),
            input_schema: entry.input_schema.clone(),
            output_schema: None,
            required_grants: entry.required_grants.clone(),
            annotations: entry.annotations,
        };
        registry
            .register_tool(descriptor, handler)
            .map_err(|e| format!("failed registering dev_ui tool '{}': {e}", entry.name))?;
        count += 1;
    }
    Ok(count)
}

/// Read the modification timestamp in milliseconds for hot-reload change detection.
pub fn file_mtime_ms(path: &Path) -> u64 {
    fs::metadata(path)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .and_then(|d| u64::try_from(d.as_millis()).ok())
        .unwrap_or(0)
}

/// Read a validated `dev_ui/*.js` script (bounded to `1 MiB`, no symlinks/reparse points)
/// and compute its lowercase hex SHA-256 digest for `native_trust.toon` pinning.
pub fn read_dev_ui_script_with_sha256(path: &Path) -> Result<(String, String), String> {
    let meta = reject_reparse_or_symlink(path).map_err(|e| e.to_string())?;
    if !meta.is_file() {
        return Err("dev_ui script target is not a regular file".to_string());
    }
    let bytes = fs::read(path).map_err(|e| format!("failed reading dev_ui script: {e}"))?;
    if bytes.is_empty() || bytes.len() > MAX_DEV_UI_SCRIPT_BYTES {
        return Err("dev_ui script size out of bounds (1..=1 MiB)".to_string());
    }
    let sha256 = hex::encode(Sha256::digest(&bytes));
    let script =
        String::from_utf8(bytes).map_err(|e| format!("dev_ui script is not valid UTF-8: {e}"))?;
    Ok((script, sha256))
}

/// Build the JSON packet sent to a Dev UI script's entrypoint.
pub fn build_dev_ui_packet(op: &str, args: &JsonValue) -> Result<String, String> {
    serde_json::to_string(&json!({ "op": op, "args": args }))
        .map_err(|e| format!("failed serializing dev_ui packet: {e}"))
}
