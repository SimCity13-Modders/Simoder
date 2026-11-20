//! DBPF v3 mod overlay packaging, retail `SP::AddPackageIfValid` metadata policy checks,
//! mount-location precedence, and safe TGI / menu-clone reference rewriting.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::Write as _;

use simoder_core::{Tgi, hex};
use simoder_formats::{Kind, Key, Package, PropertyFile, Value, build_resource_overlay};
use tempfile::NamedTempFile;

use crate::{assets, config, types::*};

/// Inspect package metadata records (`type = 0x00B1B104`, `group = 0x40404000`) to determine
/// retail `SP::AddPackageIfValid` signature policy.
pub fn inspect_package_metadata_policy(package: &Package) -> PackageMetadataVerdict {
    let count = package
        .entries()
        .iter()
        .filter(|entry| {
            entry.id.type_id == config::PROP_TYPE_ID
                && entry.id.group == config::PACKAGE_INFO_GROUP_ID
        })
        .count();
    match count {
        0 => PackageMetadataVerdict::BypassSignature,
        1 => PackageMetadataVerdict::RequiresRsaSignature,
        n => PackageMetadataVerdict::RejectedDuplicateMetadata(n),
    }
}

/// Inspect in-memory DBPF package bytes by staging through a temporary file so [`Package::open`]
/// validates the header and index cleanly on disk.
pub fn inspect_package_bytes_metadata_policy(
    bytes: &[u8],
) -> Result<PackageMetadataVerdict, ModError> {
    let mut temp = NamedTempFile::new()?;
    temp.write_all(bytes)?;
    temp.as_file().sync_all()?;
    let temp_path = temp.into_temp_path();
    let verdict = {
        let package = Package::open(&temp_path).map_err(simoder_formats::FormatError::from)?;
        inspect_package_metadata_policy(&package)
    };
    Ok(verdict)
}

/// Report whether packages mounted in `location` override base-game resources (`true` for `SimCityData/`).
pub const fn mount_overrides_base_game(location: PackageMountLocation) -> bool {
    matches!(location, PackageMountLocation::SimCityData)
}

/// Rewrite matching [`Key`] TGI references across scalar and array properties in `prop`.
///
/// Returns the number of [`Key`] values rewritten.
pub fn rewrite_tgi_references(
    prop: &mut PropertyFile,
    replacements: &HashMap<Tgi, Tgi>,
) -> usize {
    let mut rewritten = 0usize;
    for property in &mut prop.values {
        match &mut property.kind {
            Kind::Scalar(Value::Key(key)) => {
                if rewrite_key(key, replacements) {
                    rewritten += 1;
                }
            }
            Kind::Array(values) => {
                for value in values {
                    if let Value::Key(key) = value
                        && rewrite_key(key, replacements)
                    {
                        rewritten += 1;
                    }
                }
            }
            Kind::Scalar(_) | Kind::Empty => {}
        }
    }
    rewritten
}

/// Target TGI overrides applied by [`rewrite_menu_clone_references`] when cloning menu items.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct MenuCloneOverrides {
    /// Replacement parent-menu TGI for property `0x0DB9FC63`.
    pub parent_menu: Option<Tgi>,
    /// Replacement icon image TGI for property `0x0977AA8F`.
    pub menu_icon: Option<Tgi>,
    /// Replacement marquee image TGI for property `0x0DDEEE56`.
    pub menu_marquee_image: Option<Tgi>,
    /// Replacement building/unit target TGI for property `0x0975695F`.
    pub menu_unit: Option<Tgi>,
}

/// Rewrite the four retail menu-clone [`Key`] properties (`0x0DB9FC63` parent menu, `0x0977AA8F`
/// `menuIcon`, `0x0DDEEE56` `menuMarqueeImage`, `0x0975695F` `menuUnit`) when present in `prop`.
///
/// Returns the count of updated [`Key`] entries.
pub fn rewrite_menu_clone_references(
    prop: &mut PropertyFile,
    overrides: MenuCloneOverrides,
) -> usize {
    let mut rewritten = 0usize;
    for property in &mut prop.values {
        let replacement = match property.hash {
            config::MENU_PARENT_PROP_HASH => overrides.parent_menu,
            config::MENU_ICON_PROP_HASH => overrides.menu_icon,
            config::MENU_MARQUEE_IMAGE_PROP_HASH => overrides.menu_marquee_image,
            config::MENU_UNIT_PROP_HASH => overrides.menu_unit,
            _ => None,
        };
        let Some(target) = replacement else {
            continue;
        };
        match &mut property.kind {
            Kind::Scalar(Value::Key(key)) => {
                apply_tgi_to_key(key, target);
                rewritten += 1;
            }
            Kind::Array(values) => {
                for value in values {
                    if let Value::Key(key) = value {
                        apply_tgi_to_key(key, target);
                        rewritten += 1;
                    }
                }
            }
            Kind::Scalar(_) | Kind::Empty => {}
        }
    }
    rewritten
}

/// Assemble a signature-bypass DBPF v3 overlay package from `definition.new_assets` and `extra_entries`.
///
/// Guarantees that no unchanged vanilla assets are included, rejects any package-info metadata TGI
/// `(0x00B1B104, 0x40404000, *)`, rejects duplicate TGIs, and verifies round-trip DBPF parsing with
/// [`PackageMetadataVerdict::BypassSignature`].
pub fn build_mod_overlay_package(
    definition: &ModDefinition,
    extra_entries: &[(Tgi, Vec<u8>)],
    known_vanilla_digests: &HashSet<String>,
) -> Result<Vec<u8>, ModError> {
    let new_assets = definition.new_assets();
    assets::validate_no_vanilla_copies(new_assets, known_vanilla_digests)?;

    let normalized_vanilla: HashSet<String> = known_vanilla_digests
        .iter()
        .map(|d| d.to_ascii_lowercase())
        .collect();
    let reference_digests: HashSet<String> = definition
        .vanilla_references()
        .iter()
        .filter_map(|r| r.expected_sha256.as_ref())
        .map(|d| d.to_ascii_lowercase())
        .collect();

    let mut entries = Vec::with_capacity(new_assets.len() + extra_entries.len());
    let mut seen_tgis = HashSet::with_capacity(new_assets.len() + extra_entries.len());

    for asset in new_assets {
        let tgi = asset.target.unwrap_or(Tgi {
            type_id: asset.kind.default_type_id(),
            group: config::MOD_ASSET_GROUP_ID,
            instance: assets::allocate_mod_instance_id(&definition.manifest.id, &asset.id),
        });
        let bytes = fs::read(&asset.resolved_path)?;
        let digest = hex::encode(assets::sha256_bytes(&bytes));
        if digest != asset.sha256 {
            return Err(ModError::AssetValidation(format!(
                "new asset '{}' modified on disk prior to packaging",
                asset.id
            )));
        }
        if normalized_vanilla.contains(&digest) || reference_digests.contains(&digest) {
            return Err(ModError::VanillaCopyViolation(format!(
                "refusing to pack vanilla asset digest {digest} from '{}'",
                asset.relative_path
            )));
        }
        push_checked_entry(&mut entries, &mut seen_tgis, tgi, bytes)?;
    }

    for (tgi, data) in extra_entries {
        if !normalized_vanilla.is_empty() {
            let digest = hex::encode(assets::sha256_bytes(data));
            if normalized_vanilla.contains(&digest) {
                return Err(ModError::VanillaCopyViolation(format!(
                    "extra entry {tgi} matches known vanilla SHA-256 {digest}"
                )));
            }
        }
        push_checked_entry(&mut entries, &mut seen_tgis, *tgi, data.clone())?;
    }

    let bytes = build_resource_overlay(&entries)?;
    let verdict = inspect_package_bytes_metadata_policy(&bytes)?;
    if verdict != PackageMetadataVerdict::BypassSignature {
        return Err(ModError::PackageValidation(format!(
            "built overlay package returned unexpected metadata policy verdict: {verdict:?}"
        )));
    }
    Ok(bytes)
}

fn push_checked_entry(
    entries: &mut Vec<(Tgi, Vec<u8>)>,
    seen_tgis: &mut HashSet<Tgi>,
    tgi: Tgi,
    bytes: Vec<u8>,
) -> Result<(), ModError> {
    if tgi.type_id == config::PROP_TYPE_ID && tgi.group == config::PACKAGE_INFO_GROUP_ID {
        return Err(ModError::PackageValidation(format!(
            "overlay packages must not include package-info metadata TGI {tgi}"
        )));
    }
    if !seen_tgis.insert(tgi) {
        return Err(ModError::PackageValidation(format!(
            "duplicate TGI {tgi} in overlay package"
        )));
    }
    entries.push((tgi, bytes));
    Ok(())
}

fn rewrite_key(key: &mut Key, replacements: &HashMap<Tgi, Tgi>) -> bool {
    let current = Tgi {
        type_id: key.type_id,
        group: key.group,
        instance: key.instance,
    };
    if let Some(&replacement) = replacements.get(&current) {
        apply_tgi_to_key(key, replacement);
        true
    } else {
        false
    }
}

fn apply_tgi_to_key(key: &mut Key, replacement: Tgi) {
    key.type_id = replacement.type_id;
    key.group = replacement.group;
    key.instance = replacement.instance;
}
