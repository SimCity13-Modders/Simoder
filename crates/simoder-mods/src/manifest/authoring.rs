//! Schema 2 (`simoder.authoring`) typed TOML manifest parsing and cross-field validation.

use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::Read as _;
use std::path::Path;
use std::str::FromStr;

use serde::Deserialize;
use simoder_core::{Tgi, hex, parse_tgi, tgi_key};

use crate::{assets, config, types::*};

use super::{HexOrU32, MetadataDto, TgiSpec, check_bounded, check_bounded_non_empty};

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct AuthoringManifestDto {
    schema_version: i64,
    format: String,
    metadata: MetadataDto,
    #[serde(default)]
    symbols: SymbolsDto,
    #[serde(default)]
    patches: Vec<PatchBlockDto>,
    #[serde(default)]
    assets: AssetsDto,
    #[serde(default)]
    scripts: Option<ScriptsDto>,
    #[serde(default)]
    native: Option<NativeDto>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct ScriptsDto {
    language: String,
    entry: String,
    #[serde(alias = "memoryMiB", alias = "memory_mib")]
    memory_mib: Option<u32>,
    #[serde(alias = "budgetMs", alias = "budget_ms")]
    budget_ms: Option<u32>,
}

const fn default_native_abi() -> u32 {
    crate::native_host::SD_U2A_HOST_VERSION
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct NativeDto {
    #[serde(alias = "module")]
    entry: String,
    #[serde(alias = "contractVersion", default = "default_native_abi")]
    abi: u32,
    sha256: Option<String>,
}

#[derive(Default, Deserialize)]
struct SymbolsDto {
    #[serde(default)]
    properties: Vec<PropertySymbolDto>,
    #[serde(default)]
    resources: Vec<ResourceSymbolDto>,
}

#[derive(Deserialize)]
struct PropertySymbolDto {
    name: String,
    source: String,
    id: HexOrU32,
    #[serde(rename = "type", default)]
    declared_type: String,
    description: Option<String>,
    unit: Option<String>,
    origin: Option<String>,
}

#[derive(Deserialize)]
struct ResourceSymbolDto {
    name: String,
    source: String,
    #[serde(rename = "type")]
    type_id: HexOrU32,
    group: HexOrU32,
    instance: HexOrU32,
}

#[derive(Deserialize)]
struct PatchBlockDto {
    target: PatchTargetDto,
    properties: Vec<PropertyPatchDto>,
}

#[derive(Deserialize)]
#[serde(untagged)]
enum PatchTargetDto {
    Text(String),
    Table {
        name: Option<String>,
        #[serde(rename = "type")]
        type_id: Option<HexOrU32>,
        group: Option<HexOrU32>,
        instance: Option<HexOrU32>,
    },
}

#[derive(Deserialize)]
struct PropertyPatchDto {
    name: Option<String>,
    id: Option<HexOrU32>,
    #[serde(rename = "type")]
    value_type: String,
    operation: String,
    value: NumberDto,
}

#[derive(Deserialize)]
#[serde(untagged)]
enum NumberDto {
    Float(f64),
    Int(i64),
}

#[derive(Default, Deserialize)]
struct AssetsDto {
    #[serde(default)]
    references: Vec<VanillaRefDto>,
    #[serde(default)]
    new: Vec<NewAssetDto>,
    #[serde(default)]
    materials: Vec<MaterialBundleDto>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct VanillaRefDto {
    alias: String,
    target: TgiSpec,
    source: String,
    expected_sha256: Option<String>,
    path: Option<toml::Value>,
    payload: Option<toml::Value>,
}

#[derive(Deserialize)]
struct NewAssetDto {
    id: String,
    kind: String,
    path: String,
    sha256: String,
    size: i64,
    target: Option<TgiSpec>,
}

#[derive(Deserialize)]
struct MaterialBundleDto {
    id: String,
    params: String,
    slots: Vec<String>,
    #[serde(default)]
    uses_tint_texcoord_1: bool,
}

#[derive(Default)]
struct SymbolCatalog {
    properties: HashMap<String, PropertySymbolDto>,
    resources: HashMap<String, (Tgi, String)>,
}

pub(super) fn parse(
    directory: &Path,
    raw_toml: &str,
    allow_reserved_base: bool,
) -> Result<ModDefinition, ModError> {
    let dto: AuthoringManifestDto = toml::from_str(raw_toml)?;
    if dto.schema_version != config::AUTHORING_SCHEMA_VERSION {
        return Err(ModError::ManifestValidation(format!(
            "unsupported authoring schemaVersion {}; expected {}",
            dto.schema_version,
            config::AUTHORING_SCHEMA_VERSION
        )));
    }
    if dto.format != config::AUTHORING_FORMAT && dto.format != config::NATIVE_DEVTOOL_FORMAT {
        return Err(ModError::ManifestValidation(format!(
            "unsupported manifest format '{}'; expected '{}' or '{}'",
            dto.format,
            config::AUTHORING_FORMAT,
            config::NATIVE_DEVTOOL_FORMAT
        )));
    }

    let manifest = dto.metadata.into_manifest(allow_reserved_base)?;
    let catalog = build_symbol_catalog(dto.symbols)?;
    let patches = resolve_patches(dto.patches, &catalog)?;
    let (new_assets, vanilla_references, materials) = resolve_assets(directory, dto.assets)?;
    let scripts = dto
        .scripts
        .map(|s| resolve_scripts(directory, s))
        .transpose()?;
    let native = dto
        .native
        .map(|n| resolve_native(directory, n))
        .transpose()?;

    if patches.is_empty()
        && new_assets.is_empty()
        && vanilla_references.is_empty()
        && scripts.is_none()
        && native.is_none()
    {
        return Err(ModError::ManifestValidation(
            "authoring mod must declare at least one [[patches]], [[assets.new]], [[assets.references]], [scripts], or [native] entry".into(),
        ));
    }

    Ok(ModDefinition {
        directory: directory.to_path_buf(),
        manifest,
        payload: ModPayload::Authoring(AuthoringModData {
            patches,
            new_assets,
            vanilla_references,
            materials,
            scripts,
            native,
        }),
    })
}

fn resolve_scripts(directory: &Path, dto: ScriptsDto) -> Result<ScriptModConfig, ModError> {
    check_bounded_non_empty(&dto.language, "scripts.language", config::MAX_FIELD_LEN)?;
    let language = dto.language.trim().to_ascii_lowercase();
    if language != "luau" && language != "js" {
        return Err(ModError::ManifestValidation(format!(
            "unsupported script language '{}'; expected 'luau' or 'js'",
            dto.language
        )));
    }
    check_bounded_non_empty(&dto.entry, "scripts.entry", config::MAX_SOURCE_LEN)?;
    let resolved_path = assets::resolve_safe_relative_path(directory, &dto.entry, None)?;
    Ok(ScriptModConfig {
        language,
        entry: dto.entry,
        memory_mib: dto.memory_mib,
        budget_ms: dto.budget_ms,
        resolved_path,
    })
}

fn resolve_native(directory: &Path, dto: NativeDto) -> Result<NativeModConfig, ModError> {
    check_bounded_non_empty(&dto.entry, "native.entry", config::MAX_SOURCE_LEN)?;
    if !dto.entry.to_ascii_lowercase().ends_with(".dll") {
        return Err(ModError::ManifestValidation(format!(
            "native entry '{}' must have a .dll extension",
            dto.entry
        )));
    }
    if dto.abi != crate::native_host::SD_U2A_HOST_VERSION {
        return Err(ModError::ManifestValidation(format!(
            "unsupported native host ABI version {}; expected {}",
            dto.abi,
            crate::native_host::SD_U2A_HOST_VERSION
        )));
    }
    let resolved_path = assets::resolve_safe_relative_path(directory, &dto.entry, None)?;
    let expected_sha256 = if let Some(sha) = dto.sha256 {
        if !assets::is_lowercase_sha256_hex(&sha) {
            return Err(ModError::ManifestValidation(
                "native 'sha256' must be a 64-character lowercase hex digest".into(),
            ));
        }
        let actual = hex::encode(assets::sha256_file(&resolved_path)?);
        if actual != sha {
            return Err(ModError::AssetValidation(format!(
                "native DLL '{}' SHA-256 mismatch: expected {sha}, got {actual}",
                dto.entry
            )));
        }
        Some(sha)
    } else {
        None
    };
    Ok(NativeModConfig {
        entry: dto.entry,
        abi: dto.abi,
        expected_sha256,
        resolved_path,
    })
}

fn build_symbol_catalog(dto: SymbolsDto) -> Result<SymbolCatalog, ModError> {
    let mut catalog = SymbolCatalog::default();
    for sym in dto.properties {
        check_bounded_non_empty(&sym.name, "symbols.properties.name", config::MAX_FIELD_LEN)?;
        check_bounded_non_empty(&sym.source, "symbols.properties.source", config::MAX_SOURCE_LEN)?;
        check_bounded(&sym.declared_type, "symbols.properties.type", 64)?;
        if let Some(desc) = &sym.description {
            check_bounded(desc, "symbols.properties.description", config::MAX_DESCRIPTION_LEN)?;
        }
        if let Some(unit) = &sym.unit {
            check_bounded(unit, "symbols.properties.unit", 64)?;
        }
        if let Some(origin) = &sym.origin {
            check_bounded(origin, "symbols.properties.origin", 64)?;
        }
        let name = sym.name.clone();
        if catalog.properties.insert(name.clone(), sym).is_some() {
            return Err(ModError::ManifestValidation(format!(
                "duplicate property symbol name '{name}'"
            )));
        }
    }
    for sym in dto.resources {
        check_bounded_non_empty(&sym.name, "symbols.resources.name", config::MAX_FIELD_LEN)?;
        check_bounded_non_empty(&sym.source, "symbols.resources.source", config::MAX_SOURCE_LEN)?;
        let target = Tgi {
            type_id: sym.type_id.0,
            group: sym.group.0,
            instance: sym.instance.0,
        };
        if catalog
            .resources
            .insert(sym.name.clone(), (target, sym.source))
            .is_some()
        {
            return Err(ModError::ManifestValidation(format!(
                "duplicate resource symbol name '{}'",
                sym.name
            )));
        }
    }
    Ok(catalog)
}

fn resolve_patches(
    blocks: Vec<PatchBlockDto>,
    catalog: &SymbolCatalog,
) -> Result<Vec<ResourcePatchDefinition>, ModError> {
    let mut seen_tgi_prop = HashSet::new();
    let mut out = Vec::with_capacity(blocks.len());
    for block in blocks {
        let (target, symbol) = resolve_patch_target(block.target, catalog)?;
        if block.properties.is_empty() {
            return Err(ModError::PatchValidation(
                "patch 'properties' array must not be empty".into(),
            ));
        }
        let mut properties = Vec::with_capacity(block.properties.len());
        for prop_dto in block.properties {
            let patch = resolve_property_patch(prop_dto, catalog)?;
            if !seen_tgi_prop.insert((tgi_key(target), patch.property_id)) {
                return Err(ModError::PatchValidation(format!(
                    "one mod cannot patch property 0x{:08X} on target {target} twice",
                    patch.property_id
                )));
            }
            properties.push(patch);
        }
        out.push(ResourcePatchDefinition {
            target,
            symbol,
            properties,
        });
    }
    Ok(out)
}

fn resolve_patch_target(
    dto: PatchTargetDto,
    catalog: &SymbolCatalog,
) -> Result<(Tgi, Option<ResourceSymbolOrigin>), ModError> {
    match dto {
        PatchTargetDto::Text(raw) => Ok((parse_tgi(&raw)?, None)),
        PatchTargetDto::Table {
            name: Some(alias),
            type_id,
            group,
            instance,
        } => {
            check_bounded_non_empty(&alias, "patches.target.name", config::MAX_FIELD_LEN)?;
            let (sym_target, sym_source) = catalog.resources.get(&alias).ok_or_else(|| {
                ModError::PatchValidation(format!("unknown resource symbol name '{alias}'"))
            })?;
            match (type_id, group, instance) {
                (None, None, None) => {}
                (Some(t), Some(g), Some(i)) => {
                    let explicit = Tgi {
                        type_id: t.0,
                        group: g.0,
                        instance: i.0,
                    };
                    if explicit != *sym_target {
                        return Err(ModError::PatchValidation(format!(
                            "resource name/TGI mismatch for '{alias}'"
                        )));
                    }
                }
                _ => {
                    return Err(ModError::PatchValidation(format!(
                        "partial TGI fields on aliased target '{alias}'"
                    )));
                }
            }
            Ok((
                *sym_target,
                Some(ResourceSymbolOrigin {
                    name: alias,
                    source: sym_source.clone(),
                }),
            ))
        }
        PatchTargetDto::Table {
            name: None,
            type_id: Some(t),
            group: Some(g),
            instance: Some(i),
        } => Ok((
            Tgi {
                type_id: t.0,
                group: g.0,
                instance: i.0,
            },
            None,
        )),
        PatchTargetDto::Table { name: None, .. } => Err(ModError::PatchValidation(
            "patch target requires 'name' or explicit 'type'/'group'/'instance'".into(),
        )),
    }
}

fn resolve_property_patch(
    dto: PropertyPatchDto,
    catalog: &SymbolCatalog,
) -> Result<PropertyPatch, ModError> {
    let (property_id, symbol_origin) = match (dto.name, dto.id) {
        (Some(alias), explicit_id) => {
            check_bounded_non_empty(&alias, "patches.properties.name", config::MAX_FIELD_LEN)?;
            let sym = catalog.properties.get(&alias).ok_or_else(|| {
                ModError::PatchValidation(format!("unknown property symbol name '{alias}'"))
            })?;
            if let Some(id) = explicit_id
                && id.0 != sym.id.0
            {
                return Err(ModError::PatchValidation(format!(
                    "property name/id mismatch for '{alias}'"
                )));
            }
            if !sym.declared_type.is_empty()
                && !sym.declared_type.eq_ignore_ascii_case("float")
                && !sym.declared_type.eq_ignore_ascii_case("float32")
            {
                return Err(ModError::PatchValidation(format!(
                    "symbol '{alias}' declares type '{}', incompatible with float patch",
                    sym.declared_type
                )));
            }
            (
                sym.id.0,
                Some(PropertySymbolOrigin {
                    name: alias,
                    source: sym.source.clone(),
                    description: sym.description.clone(),
                    unit: sym.unit.clone(),
                    origin: sym.origin.clone(),
                }),
            )
        }
        (None, Some(id)) => (id.0, None),
        (None, None) => {
            return Err(ModError::PatchValidation(
                "property patch requires 'name' or 'id'".into(),
            ));
        }
    };

    let value_type = PropertyValueType::from_str(&dto.value_type).map_err(|_| {
        ModError::PatchValidation(format!(
            "unsupported property type '{}'; only 'float' is supported",
            dto.value_type
        ))
    })?;
    let operation = PatchOperation::from_str(&dto.operation).map_err(|_| {
        ModError::PatchValidation(format!("unsupported patch operation '{}'", dto.operation))
    })?;

    let raw_f64 = match dto.value {
        NumberDto::Float(f) => f,
        NumberDto::Int(i) => i as f64,
    };
    if !raw_f64.is_finite() || raw_f64.abs() > f32::MAX as f64 {
        return Err(ModError::PatchValidation(
            "patch field 'value' must fit within finite IEEE-754 f32 range".into(),
        ));
    }
    let value = raw_f64 as f32;
    if operation == PatchOperation::Divide && value == 0.0 {
        return Err(ModError::PatchValidation(
            "divide patch cannot use zero".into(),
        ));
    }

    Ok(PropertyPatch {
        property_id,
        value_type,
        operation,
        value,
        symbol: symbol_origin,
    })
}

type ResolvedAuthoringAssets = (
    Vec<NewAssetDefinition>,
    Vec<VanillaReferenceDefinition>,
    Vec<MaterialBundleDefinition>,
);

fn resolve_assets(
    directory: &Path,
    dto: AssetsDto,
) -> Result<ResolvedAuthoringAssets, ModError> {
    let mut references = Vec::with_capacity(dto.references.len());
    let mut ref_aliases = HashSet::new();
    let mut ref_targets = HashSet::new();
    let mut ref_digests = HashSet::new();
    for r in dto.references {
        if r.path.is_some() || r.payload.is_some() {
            return Err(ModError::VanillaCopyViolation(
                "vanilla reference under [[assets.references]] must not bundle 'path' or 'payload' files".into(),
            ));
        }
        check_bounded_non_empty(&r.alias, "assets.references.alias", config::MAX_FIELD_LEN)?;
        check_bounded_non_empty(&r.source, "assets.references.source", config::MAX_SOURCE_LEN)?;
        if !ref_aliases.insert(r.alias.clone()) {
            return Err(ModError::ManifestValidation(format!(
                "duplicate vanilla reference alias '{}'",
                r.alias
            )));
        }
        let target = r.target.resolve()?;
        if let Some(digest) = &r.expected_sha256 {
            if !assets::is_lowercase_sha256_hex(digest) {
                return Err(ModError::ManifestValidation(format!(
                    "vanilla reference '{}' 'expected_sha256' must be 64 lowercase hex characters",
                    r.alias
                )));
            }
            ref_digests.insert(digest.clone());
        }
        ref_targets.insert(target);
        references.push(VanillaReferenceDefinition {
            alias: r.alias,
            target,
            source: r.source,
            expected_sha256: r.expected_sha256,
        });
    }

    let mut new_assets = Vec::with_capacity(dto.new.len());
    let mut new_ids = HashMap::new();
    let mut new_targets = HashSet::new();
    for a in dto.new {
        check_bounded_non_empty(&a.id, "assets.new.id", config::MAX_FIELD_LEN)?;
        if new_ids.contains_key(&a.id) {
            return Err(ModError::ManifestValidation(format!(
                "duplicate new asset id '{}'",
                a.id
            )));
        }
        let kind = AssetKind::from_str(&a.kind).map_err(|_| {
            ModError::AssetValidation(format!("unsupported asset kind '{}' for '{}'", a.kind, a.id))
        })?;
        check_bounded_non_empty(&a.path, "assets.new.path", config::MAX_SOURCE_LEN)?;
        let assets_prefix = format!("{}/", config::ASSETS_ROOT_DIR_NAME);
        let resolved_path =
            assets::resolve_safe_relative_path(directory, &a.path, Some(&assets_prefix))?;
        if !assets::is_lowercase_sha256_hex(&a.sha256) {
            return Err(ModError::AssetValidation(format!(
                "new asset '{}' requires a 64-character lowercase SHA-256 digest",
                a.id
            )));
        }
        if a.size < 0 || a.size as u64 > config::MAX_ASSET_BYTES {
            return Err(ModError::AssetValidation(format!(
                "new asset '{}' size {} exceeds 0..={}",
                a.id,
                a.size,
                config::MAX_ASSET_BYTES
            )));
        }
        let size = a.size as u64;
        let target = a
            .target
            .map(TgiSpec::resolve)
            .transpose()?
            .map(|tgi| {
                if ref_targets.contains(&tgi) {
                    return Err(ModError::VanillaCopyViolation(format!(
                        "new asset '{}' targets vanilla reference TGI {tgi}",
                        a.id
                    )));
                }
                if !new_targets.insert(tgi) {
                    return Err(ModError::ManifestValidation(format!(
                        "duplicate explicit target TGI {tgi} among [[assets.new]]"
                    )));
                }
                Ok(tgi)
            })
            .transpose()?;

        let file = File::open(&resolved_path)?;
        let disk_len = file.metadata()?.len();
        if disk_len != size {
            return Err(ModError::AssetValidation(format!(
                "new asset '{}' size mismatch: manifest={size}, disk={disk_len}",
                a.id
            )));
        }
        let mut bytes = Vec::with_capacity(size as usize);
        file.take(size + 1).read_to_end(&mut bytes)?;
        if bytes.len() as u64 != size {
            return Err(ModError::AssetValidation(format!(
                "new asset '{}' size changed during read: expected {size}, got {}",
                a.id,
                bytes.len()
            )));
        }
        let actual_sha = hex::encode(assets::sha256_bytes(&bytes));
        if actual_sha != a.sha256 {
            return Err(ModError::AssetValidation(format!(
                "new asset '{}' SHA-256 mismatch: expected {}, got {actual_sha}",
                a.id, a.sha256
            )));
        }
        if ref_digests.contains(&actual_sha) {
            return Err(ModError::VanillaCopyViolation(format!(
                "new asset '{}' duplicates SHA-256 {actual_sha} of a declared vanilla reference",
                a.id
            )));
        }
        assets::validate_asset_format(kind, &bytes, &a.path)?;

        new_ids.insert(a.id.clone(), kind);
        new_assets.push(NewAssetDefinition {
            id: a.id,
            kind,
            relative_path: a.path,
            resolved_path,
            sha256: a.sha256,
            size,
            target,
        });
    }

    let mut materials = Vec::with_capacity(dto.materials.len());
    let mut mat_ids = HashSet::new();
    for m in dto.materials {
        check_bounded_non_empty(&m.id, "assets.materials.id", config::MAX_FIELD_LEN)?;
        if !mat_ids.insert(m.id.clone()) {
            return Err(ModError::ManifestValidation(format!(
                "duplicate material bundle id '{}'",
                m.id
            )));
        }
        if new_ids.get(&m.params).copied() != Some(AssetKind::MaterialParams) {
            return Err(ModError::AssetValidation(format!(
                "material '{}' params '{}' must reference a declared material-params asset in [[assets.new]]",
                m.id, m.params
            )));
        }
        if m.slots.len() != config::MATERIAL_TEXTURE_SLOT_COUNT {
            return Err(ModError::AssetValidation(format!(
                "material '{}' must specify exactly {} texture slots, got {}",
                m.id,
                config::MATERIAL_TEXTURE_SLOT_COUNT,
                m.slots.len()
            )));
        }
        for (idx, slot_ref) in m.slots.iter().enumerate() {
            if !new_ids.contains_key(slot_ref) && !ref_aliases.contains(slot_ref) {
                return Err(ModError::AssetValidation(format!(
                    "material '{}' slot {idx} references unknown asset/alias '{slot_ref}'",
                    m.id
                )));
            }
        }
        let texture_slot_asset_ids: [String; config::MATERIAL_TEXTURE_SLOT_COUNT] =
            m.slots.try_into().expect("slot count verified");
        materials.push(MaterialBundleDefinition {
            id: m.id,
            params_asset_id: m.params,
            texture_slot_asset_ids,
            uses_tint_texcoord_1: m.uses_tint_texcoord_1,
        });
    }

    Ok((new_assets, references, materials))
}
