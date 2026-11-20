//! Directory scanning, symlink/cache filtering, candidate probing, and duplicate ID detection
//! for [`super::ModManager`].

use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

use crate::{assets, config, manifest, types::*};

pub(super) struct DiscoveredCandidate {
    pub(super) definition: ModDefinition,
    pub(super) fingerprint: ModFingerprint,
}

pub(super) struct DiscoveryScan {
    pub(super) unique: BTreeMap<String, DiscoveredCandidate>,
    pub(super) duplicate_ids: HashSet<String>,
    pub(super) failed_by_path: HashMap<PathBuf, (String, ModUiEntry)>,
    pub(super) duplicate_ui_entries: Vec<ModUiEntry>,
}

struct FailedDirectory {
    id: String,
    name: String,
    version: String,
    author: String,
    diagnostic: String,
}

impl FailedDirectory {
    fn from_error(path: &Path, diagnostic: impl ToString) -> Self {
        Self {
            id: String::new(),
            name: display_leaf(path),
            version: String::new(),
            author: String::new(),
            diagnostic: diagnostic.to_string(),
        }
    }

    fn from_manifest(manifest: &ModManifest, diagnostic: String) -> Self {
        Self {
            id: manifest.id.clone(),
            name: manifest.name.clone(),
            version: manifest.version.clone(),
            author: manifest.author.clone(),
            diagnostic,
        }
    }

    fn into_ui_entry(self) -> ModUiEntry {
        ModUiEntry {
            id: self.id,
            name: self.name,
            version: self.version,
            author: self.author,
            state: ModState::Failed,
            can_enable: false,
            can_disable: false,
            has_pending_changes: false,
            can_reload: false,
            pending_revision: None,
            diagnostic: Some(self.diagnostic),
            active_patches: Vec::new(),
            pending_patches: Vec::new(),
            new_asset_count: 0,
            vanilla_reference_count: 0,
        }
    }
}

pub(super) fn scan_mods_directory(
    mods_directory: &Path,
    known_vanilla_digests: &HashSet<String>,
) -> Result<DiscoveryScan, ModError> {
    fs::create_dir_all(mods_directory)?;

    let mut dir_entries: Vec<PathBuf> = fs::read_dir(mods_directory)?
        .map(|entry| entry.map(|e| e.path()))
        .collect::<Result<Vec<_>, _>>()?;
    dir_entries.sort();

    let mut failed_by_path = HashMap::new();
    let mut grouped: BTreeMap<String, Vec<DiscoveredCandidate>> = BTreeMap::new();

    for path in dir_entries {
        if is_shared_cache_dir(&path) {
            continue;
        }
        match assets::reject_reparse_or_symlink(&path) {
            Ok(meta) if !meta.is_dir() => continue,
            Ok(_) => {}
            Err(err) => {
                let fail = FailedDirectory::from_error(&path, &err);
                let msg = fail.diagnostic.clone();
                failed_by_path.insert(path, (msg, fail.into_ui_entry()));
                continue;
            }
        }
        match probe_directory(&path, known_vanilla_digests) {
            Ok(candidate) => grouped
                .entry(candidate.definition.manifest.id.clone())
                .or_default()
                .push(candidate),
            Err(fail) => {
                let msg = fail.diagnostic.clone();
                failed_by_path.insert(path, (msg, fail.into_ui_entry()));
            }
        }
    }

    let mut unique = BTreeMap::new();
    let mut duplicate_ids = HashSet::new();
    let mut duplicate_ui_entries = Vec::new();
    for (id, mut candidates) in grouped {
        if candidates.len() != 1 {
            duplicate_ids.insert(id.clone());
            let paths_joined = candidates
                .iter()
                .map(|c| c.definition.directory.display().to_string())
                .collect::<Vec<_>>()
                .join("; ");
            let diag = format!("Duplicate mod ID '{id}' paths: {paths_joined}");
            for candidate in candidates {
                duplicate_ui_entries.push(
                    FailedDirectory::from_manifest(&candidate.definition.manifest, diag.clone())
                        .into_ui_entry(),
                );
            }
            continue;
        }
        unique.insert(id, candidates.remove(0));
    }

    Ok(DiscoveryScan {
        unique,
        duplicate_ids,
        failed_by_path,
        duplicate_ui_entries,
    })
}

fn probe_directory(
    directory: &Path,
    known_vanilla_digests: &HashSet<String>,
) -> Result<DiscoveredCandidate, FailedDirectory> {
    let (definition, fingerprint) = manifest::load_stable_candidate(directory)
        .map_err(|err| FailedDirectory::from_error(directory, err))?;
    if let Some(diag) = manifest::raw_resource_runtime_diagnostic(&definition) {
        return Err(FailedDirectory::from_manifest(&definition.manifest, diag));
    }
    assets::validate_no_vanilla_copies(definition.new_assets(), known_vanilla_digests)
        .map_err(|err| FailedDirectory::from_manifest(&definition.manifest, err.to_string()))?;
    Ok(DiscoveredCandidate {
        definition,
        fingerprint,
    })
}

fn is_shared_cache_dir(path: &Path) -> bool {
    path.file_name()
        .and_then(|n| n.to_str())
        .is_some_and(|name| name.eq_ignore_ascii_case(config::SHARED_CACHE_DIR_NAME))
}

fn display_leaf(path: &Path) -> String {
    path.file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("<unrepresentable-path>")
        .to_string()
}
