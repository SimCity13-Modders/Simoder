//! Schema 3 (`simoder.raw-resources`) typed TOML manifest parsing and binary integrity checks.

use std::collections::HashSet;
use std::fs;
use std::path::Path;
use std::str::FromStr;

use serde::Deserialize;
use simoder_core::hex;

use crate::{assets, config, types::*};

use super::{MetadataDto, check_bounded_non_empty};

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct RawResourcesManifestDto {
    schema_version: i64,
    contract: String,
    required_capabilities: Vec<String>,
    metadata: MetadataDto,
    resources: Vec<RawResourceEntryDto>,
}

#[derive(Deserialize)]
struct RawResourceEntryDto {
    id: String,
    operation: String,
    sha256: String,
    size: i64,
    payload: String,
}

pub(super) fn parse(directory: &Path, raw_toml: &str) -> Result<ModDefinition, ModError> {
    let dto: RawResourcesManifestDto = toml::from_str(raw_toml)?;
    if dto.schema_version != config::RAW_RESOURCES_SCHEMA_VERSION
        || dto.contract != config::RAW_RESOURCES_CONTRACT
    {
        return Err(ModError::ManifestValidation(format!(
            "unsupported raw-resources schemaVersion={}, contract='{}'",
            dto.schema_version, dto.contract
        )));
    }
    if dto.required_capabilities.is_empty() {
        return Err(ModError::ManifestValidation(
            "'requiredCapabilities' must not be empty".into(),
        ));
    }

    let mut capabilities = Vec::with_capacity(dto.required_capabilities.len());
    let mut cap_set = HashSet::new();
    for raw_cap in dto.required_capabilities {
        let cap = RuntimeCapability::from_str(&raw_cap).map_err(|_| {
            ModError::ManifestValidation(format!(
                "unknown required runtime capability '{raw_cap}'"
            ))
        })?;
        if cap_set.insert(cap) {
            capabilities.push(cap);
        }
    }

    let manifest = dto.metadata.into_manifest()?;
    if dto.resources.is_empty() || dto.resources.len() > config::MAX_RESOURCE_ENTRIES {
        return Err(ModError::ManifestValidation(format!(
            "'[[resources]]' count must be within 1..={}",
            config::MAX_RESOURCE_ENTRIES
        )));
    }

    let mut seen_ids = HashSet::new();
    let mut resources = Vec::with_capacity(dto.resources.len());
    for entry in dto.resources {
        let id = validate_raw_resource_id(&entry.id)?;
        if !seen_ids.insert(id.clone()) {
            return Err(ModError::ManifestValidation(format!(
                "duplicate raw resource ID '{id}'"
            )));
        }
        let operation = RawResourceOperation::from_str(&entry.operation).map_err(|_| {
            ModError::ManifestValidation(format!(
                "unsupported resource operation '{}'",
                entry.operation
            ))
        })?;
        if !cap_set.contains(&operation.required_capability()) {
            return Err(ModError::ManifestValidation(format!(
                "resource '{id}' operation '{}' requires undeclared capability '{}'",
                entry.operation,
                operation.required_capability().as_str()
            )));
        }
        if !assets::is_lowercase_sha256_hex(&entry.sha256) {
            return Err(ModError::AssetValidation(format!(
                "resource '{id}' requires a 64-character lowercase SHA-256 digest"
            )));
        }
        if entry.size < 0 || entry.size as u64 > config::MAX_ASSET_BYTES {
            return Err(ModError::AssetValidation(format!(
                "resource '{id}' size {} exceeds valid bounds 0..={}",
                entry.size,
                config::MAX_ASSET_BYTES
            )));
        }
        let size = entry.size as u64;
        check_bounded_non_empty(&entry.payload, "resources.payload", config::MAX_SOURCE_LEN)?;
        let resolved_path =
            assets::resolve_safe_relative_path(directory, &entry.payload, Some("assets/"))?;
        let actual_len = fs::metadata(&resolved_path)?.len();
        if actual_len != size {
            return Err(ModError::AssetValidation(format!(
                "resource '{id}' size mismatch: manifest={size}, disk={actual_len}"
            )));
        }
        let actual_sha = hex::encode(assets::sha256_file(&resolved_path)?);
        if actual_sha != entry.sha256 {
            return Err(ModError::AssetValidation(format!(
                "resource '{id}' SHA-256 mismatch: expected {}, got {actual_sha}",
                entry.sha256
            )));
        }

        resources.push(BinaryResourceDefinition {
            id,
            operation,
            sha256: entry.sha256,
            size,
            relative_path: entry.payload,
            resolved_path,
        });
    }

    Ok(ModDefinition {
        directory: directory.to_path_buf(),
        manifest,
        payload: ModPayload::RawResources(RawResourcesModData {
            resources,
            required_capabilities: capabilities,
        }),
    })
}

fn validate_raw_resource_id(raw: &str) -> Result<String, ModError> {
    if raw.len() != 34 || raw.as_bytes()[8] != b':' || raw.as_bytes()[17] != b':' {
        return Err(ModError::ManifestValidation(format!(
            "raw resource ID '{raw}' must follow TYPE:GROUP:64-bit-INSTANCE (34 chars)"
        )));
    }
    for part in [&raw[0..8], &raw[9..17], &raw[18..34]] {
        if !part.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(ModError::ManifestValidation(format!(
                "raw resource ID '{raw}' contains non-hexadecimal digits"
            )));
        }
    }
    Ok(raw.to_ascii_uppercase())
}
