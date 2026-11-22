#![forbid(unsafe_code)]

//! Live `Simoder-Base` HotReload watcher (`scripts/**/*.luau` and `ui/applicator.js`) with
//! atomic Luau VM replacement and compiled-in applicator fallback.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::UNIX_EPOCH;

use walkdir::WalkDir;

use crate::base_bridge::{BaseRuntimeHost, ModsActionBackend, UiStateHost};

/// Summary emitted when [`BaseHotReloadState::check_and_reload`] swaps in a new `Simoder-Base` generation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BaseHotReloadReport {
    /// Monotonically increasing reload generation after a successful swap.
    pub reload_gen: u64,
    /// Whether `ui/applicator.js` was loaded from `Simoder-Base` rather than the compiled-in fallback.
    pub external_applicator: bool,
}

/// Polls `Simoder-Base` (`scripts/**/*.luau` and `ui/applicator.js`) for live edits and swaps
/// both the sandboxed Luau runtime and the EAWebKit DOM applicator atomically.
pub struct BaseHotReloadState {
    base_dir: PathBuf,
    last_fingerprint: BTreeMap<String, (u64, u128)>,
    applicator_script: String,
    reload_gen: u64,
}

impl BaseHotReloadState {
    /// Capture the initial file fingerprint and load `ui/applicator.js` (or the compiled-in fallback).
    pub fn new(base_dir: PathBuf) -> Self {
        let last_fingerprint = scan_base_fingerprint(&base_dir);
        let (applicator_script, _) = load_applicator_script(&base_dir);
        Self {
            base_dir,
            last_fingerprint,
            applicator_script,
            reload_gen: 0,
        }
    }

    /// Return the current hot-reload generation (`0` before the first live reload).
    pub fn reload_gen(&self) -> u64 {
        self.reload_gen
    }

    /// Return the active EAWebKit DOM applicator JS source.
    pub fn applicator_script(&self) -> &str {
        &self.applicator_script
    }

    /// Re-scan `base_dir` and atomically swap `*host` + `applicator_script` when any watched file changed.
    ///
    /// If the edited Luau scripts fail to compile or initialize, `*host` and `applicator_script`
    /// stay on the previous working generation and `Err` is returned.
    pub fn check_and_reload(
        &mut self,
        host: &mut BaseRuntimeHost,
        runtime: Arc<dyn UiStateHost>,
        backend: Arc<dyn ModsActionBackend>,
    ) -> Result<Option<BaseHotReloadReport>, String> {
        let current = scan_base_fingerprint(&self.base_dir);
        if current == self.last_fingerprint {
            return Ok(None);
        }
        // Update fingerprint immediately so a broken edit logs once until edited again.
        self.last_fingerprint = current;

        let fresh_host = BaseRuntimeHost::start_from_base_dir(&self.base_dir, runtime, backend)?;
        let (script, external_applicator) = load_applicator_script(&self.base_dir);
        *host = fresh_host;
        // Dropping the previous `BaseRuntimeHost` triggers `onDisable()` -> `revokeOwnerGeneration()`;
        // refresh the new host immediately so active composed patches remain in `IntegratedSimoderRuntime`.
        host.refresh_ui()?;
        self.applicator_script = script;
        self.reload_gen = self.reload_gen.saturating_add(1);
        Ok(Some(BaseHotReloadReport {
            reload_gen: self.reload_gen,
            external_applicator,
        }))
    }
}

/// Load `base_dir/ui/applicator.js` if present and valid UTF-8, otherwise fall back to [`crate::ui_patch_host::UI_PATCH_APPLICATOR_ES5`].
pub fn load_applicator_script(base_dir: &Path) -> (String, bool) {
    let ext_path = base_dir.join("ui").join("applicator.js");
    if let Ok(meta) = fs::symlink_metadata(&ext_path)
        && !meta.file_type().is_symlink()
        && meta.is_file()
        && let Ok(text) = fs::read_to_string(&ext_path)
        && !text.trim().is_empty()
    {
        return (text, true);
    }
    (
        crate::ui_patch_host::UI_PATCH_APPLICATOR_ES5.to_string(),
        false,
    )
}

/// Fingerprint all non-symlink `.luau` files under `base_dir/scripts` and `base_dir/ui/applicator.js`.
pub fn scan_base_fingerprint(base_dir: &Path) -> BTreeMap<String, (u64, u128)> {
    let mut out = BTreeMap::new();
    let scripts_dir = base_dir.join("scripts");
    if scripts_dir.is_dir() {
        for entry in WalkDir::new(&scripts_dir)
            .follow_links(false)
            .into_iter()
            .filter_map(Result::ok)
        {
            if entry.file_type().is_symlink() || !entry.file_type().is_file() {
                continue;
            }
            if entry.path().extension().and_then(|e| e.to_str()) != Some("luau") {
                continue;
            }
            if let Some(stamp) = file_stamp(entry.path())
                && let Ok(rel) = entry.path().strip_prefix(base_dir)
            {
                out.insert(rel.to_string_lossy().replace('\\', "/"), stamp);
            }
        }
    }
    let applicator_path = base_dir.join("ui").join("applicator.js");
    if let Some(stamp) = file_stamp(&applicator_path) {
        out.insert("ui/applicator.js".to_string(), stamp);
    }
    out
}

fn file_stamp(path: &Path) -> Option<(u64, u128)> {
    let meta = fs::symlink_metadata(path).ok()?;
    if meta.file_type().is_symlink() || !meta.is_file() {
        return None;
    }
    let modified_ns = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    Some((meta.len(), modified_ns))
}
