//! Schema 2 authoring and Schema 3 raw-resources `mod.toml` deserialization, validation,
//! and candidate stability fingerprinting.

mod authoring;
mod raw;

use std::fs;
use std::path::Path;

use serde::{Deserialize, Deserializer};
use simoder_core::{Digest, Sha256, Tgi, hex, parse_tgi};

use crate::{assets, config, types::*};

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct ManifestProbe {
    schema_version: Option<i64>,
    contract: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct HexOrU32(pub(super) u32);

impl<'de> Deserialize<'de> for HexOrU32 {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        #[derive(Deserialize)]
        #[serde(untagged)]
        enum Raw {
            Int(i64),
            Str(String),
        }
        match Raw::deserialize(deserializer)? {
            Raw::Int(num) => u32::try_from(num)
                .map(Self)
                .map_err(|_| serde::de::Error::custom(format!("integer {num} out of u32 range"))),
            Raw::Str(text) => {
                let trimmed = text.trim();
                let digits = trimmed
                    .strip_prefix("0x")
                    .or_else(|| trimmed.strip_prefix("0X"))
                    .ok_or_else(|| {
                        serde::de::Error::custom(format!("hex string '{text}' must start with 0x"))
                    })?;
                if digits.is_empty() || digits.len() > 8 {
                    return Err(serde::de::Error::custom(format!(
                        "hex string '{text}' has invalid width"
                    )));
                }
                u32::from_str_radix(digits, 16)
                    .map(Self)
                    .map_err(|_| serde::de::Error::custom(format!("invalid hex '{text}'")))
            }
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(untagged)]
pub(super) enum TgiSpec {
    Text(String),
    Table {
        #[serde(rename = "type")]
        type_id: HexOrU32,
        group: HexOrU32,
        instance: HexOrU32,
    },
}

impl TgiSpec {
    pub(super) fn resolve(self) -> Result<Tgi, ModError> {
        match self {
            Self::Text(raw) => Ok(parse_tgi(&raw)?),
            Self::Table {
                type_id,
                group,
                instance,
            } => Ok(Tgi {
                type_id: type_id.0,
                group: group.0,
                instance: instance.0,
            }),
        }
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub(super) struct MetadataDto {
    id: String,
    name: String,
    version: String,
    #[serde(default)]
    author: String,
    #[serde(default)]
    description: String,
}

impl MetadataDto {
    pub(super) fn into_manifest(self) -> Result<ModManifest, ModError> {
        check_bounded_non_empty(&self.id, "metadata.id", config::MAX_MOD_ID_LEN)?;
        if !is_valid_mod_id(&self.id) {
            return Err(ModError::InvalidModId(self.id));
        }
        if self.id == config::RESERVED_BASE_MOD_ID {
            return Err(ModError::ManifestValidation(format!(
                "reserved mod ID '{}' cannot be claimed by third-party mods",
                config::RESERVED_BASE_MOD_ID
            )));
        }
        check_bounded_non_empty(&self.name, "metadata.name", config::MAX_FIELD_LEN)?;
        check_bounded_non_empty(&self.version, "metadata.version", config::MAX_FIELD_LEN)?;
        check_bounded(&self.author, "metadata.author", config::MAX_FIELD_LEN)?;
        check_bounded(
            &self.description,
            "metadata.description",
            config::MAX_DESCRIPTION_LEN,
        )?;
        Ok(ModManifest {
            id: self.id,
            name: self.name,
            version: self.version,
            author: self.author,
            description: self.description,
        })
    }
}

pub(super) fn check_bounded_non_empty(
    value: &str,
    field: &str,
    max_len: usize,
) -> Result<(), ModError> {
    if value.is_empty() || value.len() > max_len {
        return Err(ModError::ManifestValidation(format!(
            "field '{field}' length must be within 1..={max_len}"
        )));
    }
    Ok(())
}

pub(super) fn check_bounded(value: &str, field: &str, max_len: usize) -> Result<(), ModError> {
    if value.len() > max_len {
        return Err(ModError::ManifestValidation(format!(
            "optional field '{field}' exceeds {max_len} bytes"
        )));
    }
    Ok(())
}

/// Load and strictly validate a mod directory's `mod.toml` (Schema 2 authoring or Schema 3 raw-resources).
///
/// Rejects retired manifest coexistence (`mod.toon`, `project.toml`), reserved `simoder.base` IDs,
/// symlinks/reparse points, missing or corrupted assets, vanilla copies disguised as new assets, and
/// inconsistent symbol/property definitions.
pub fn load_mod_definition(directory: &Path) -> Result<ModDefinition, ModError> {
    let dir_meta = assets::reject_reparse_or_symlink(directory)?;
    if !dir_meta.is_dir() {
        return Err(ModError::ManifestValidation(format!(
            "mod path is not a directory: {}",
            directory.display()
        )));
    }
    let manifest_path = directory.join(config::MOD_MANIFEST_FILE_NAME);
    if !manifest_path.try_exists()? {
        return Err(ModError::ManifestValidation(format!(
            "missing '{}'; migrate retired manifests to Schema 2 TOML",
            config::MOD_MANIFEST_FILE_NAME
        )));
    }
    for retired in config::RETIRED_MANIFEST_FILES {
        if directory.join(retired).try_exists()? {
            return Err(ModError::ManifestValidation(format!(
                "ambiguous mod directory: '{}' cannot coexist with retired '{retired}'",
                config::MOD_MANIFEST_FILE_NAME
            )));
        }
    }

    let manifest_meta = assets::reject_reparse_or_symlink(&manifest_path)?;
    if !manifest_meta.is_file() {
        return Err(ModError::ManifestValidation(format!(
            "'{}' is not a regular file",
            config::MOD_MANIFEST_FILE_NAME
        )));
    }
    if manifest_meta.len() > config::MAX_MANIFEST_BYTES as u64 {
        return Err(ModError::ManifestValidation(format!(
            "'{}' exceeds {} bytes limit",
            config::MOD_MANIFEST_FILE_NAME,
            config::MAX_MANIFEST_BYTES
        )));
    }

    let raw_toml = fs::read_to_string(&manifest_path)?;
    let probe: ManifestProbe = toml::from_str(&raw_toml)?;
    let is_raw = probe.contract.as_deref() == Some(config::RAW_RESOURCES_CONTRACT)
        || probe.schema_version == Some(config::RAW_RESOURCES_SCHEMA_VERSION);

    if is_raw {
        raw::parse(directory, &raw_toml)
    } else {
        authoring::parse(directory, &raw_toml)
    }
}

/// Produce a diagnostic blocking activation when a validated Schema 3 raw-resources mod requires runtime support.
pub fn raw_resource_runtime_diagnostic(definition: &ModDefinition) -> Option<String> {
    definition.raw_resources().map(|raw| {
        format!(
            "Validated raw-resources project ({} entries). Activation blocked: resource.register/resource.replace runtime is not enabled in loader.",
            raw.resources.len()
        )
    })
}

/// Compute a deterministic SHA-256 fingerprint covering `mod.toml` bytes and all bundled asset digests.
pub fn compute_definition_fingerprint(definition: &ModDefinition) -> Result<ModFingerprint, ModError> {
    let manifest_bytes = fs::read(definition.directory.join(config::MOD_MANIFEST_FILE_NAME))?;
    let mut hasher = Sha256::new();
    hasher.update(b"simoder.mod.fingerprint.v1\0");
    hasher.update((manifest_bytes.len() as u64).to_le_bytes());
    hasher.update(&manifest_bytes);

    let mut asset_entries: Vec<(&str, &str)> = match &definition.payload {
        ModPayload::Authoring(data) => data
            .new_assets
            .iter()
            .map(|a| (a.relative_path.as_str(), a.sha256.as_str()))
            .collect(),
        ModPayload::RawResources(data) => data
            .resources
            .iter()
            .map(|r| (r.relative_path.as_str(), r.sha256.as_str()))
            .collect(),
    };
    asset_entries.sort();
    for (rel, digest) in asset_entries {
        hasher.update(b"asset\0");
        hasher.update((rel.len() as u64).to_le_bytes());
        hasher.update(rel.as_bytes());
        hasher.update(digest.as_bytes());
    }

    Ok(ModFingerprint(hasher.finalize().into()))
}

/// Load a mod candidate while guarding against concurrent file edits across [`config::CANDIDATE_STABILITY_ATTEMPTS`].
pub fn load_stable_candidate(
    directory: &Path,
) -> Result<(ModDefinition, ModFingerprint), ModError> {
    let manifest_path = directory.join(config::MOD_MANIFEST_FILE_NAME);
    for _ in 0..config::CANDIDATE_STABILITY_ATTEMPTS {
        let before_manifest = assets::sha256_file(&manifest_path)?;
        let definition = load_mod_definition(directory)?;
        if before_manifest != assets::sha256_file(&manifest_path)? {
            continue;
        }
        let payload_paths: Vec<(&Path, &str)> = match &definition.payload {
            ModPayload::Authoring(data) => data
                .new_assets
                .iter()
                .map(|a| (a.resolved_path.as_path(), a.sha256.as_str()))
                .collect(),
            ModPayload::RawResources(data) => data
                .resources
                .iter()
                .map(|r| (r.resolved_path.as_path(), r.sha256.as_str()))
                .collect(),
        };
        let payloads_stable = payload_paths.into_iter().all(|(path, expected)| {
            assets::sha256_file(path)
                .map(|digest| hex::encode(digest) == expected)
                .unwrap_or(false)
        });
        if payloads_stable {
            let fingerprint = compute_definition_fingerprint(&definition)?;
            return Ok((definition, fingerprint));
        }
    }

    Err(ModError::UnstableInputs(format!(
        "mod files in '{}' changed repeatedly while being validated",
        directory.display()
    )))
}
