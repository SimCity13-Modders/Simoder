//! Strongly typed domain errors, single-source `strum` enums, manifest definitions,
//! asset declarations, and mod UI state snapshots for `simoder-mods`.

use std::fmt;
use std::path::PathBuf;

use serde::{Deserialize, Deserializer, Serialize, Serializer, de};
use serde_with::DeserializeFromStr;
use simoder_core::{CoreError, Tgi, hex, parse_tgi};
use simoder_formats::FormatError;
use strum::{EnumString, IntoStaticStr, VariantArray};

use crate::config::{
    GLB_TYPE_ID, MATERIAL_PARAMS_TYPE_ID, MATERIAL_TEXTURE_SLOT_COUNT, MAX_MOD_ID_LEN, PNG_TYPE_ID,
    PROP_TYPE_ID, RAW_ASSET_TYPE_ID, RW4_TYPE_ID,
};

/// Errors produced during mod discovery, manifest validation, asset inspection,
/// state persistence, patch resolution, or overlay packaging.
#[derive(Debug, thiserror::Error)]
pub enum ModError {
    /// Filesystem I/O failure while reading mod manifests, assets, or `state.toon`.
    #[error("mod i/o error: {0}")]
    Io(#[from] std::io::Error),
    /// Container, TOON, or property binary codec failure from [`simoder_formats`].
    #[error("mod format error: {0}")]
    Format(#[from] FormatError),
    /// Core primitive validation failure (for example invalid TGI formatting).
    #[error("mod core error: {0}")]
    Core(#[from] CoreError),
    /// Failure parsing TOML manifest syntax.
    #[error("manifest TOML parse error: {0}")]
    TomlParse(#[from] toml::de::Error),
    /// Failure parsing JSON inside a `.glb` chunk or material parameter file.
    #[error("asset JSON parse error: {0}")]
    JsonParse(#[from] serde_json::Error),
    /// Invalid mod identifier violating character, length, or separator rules.
    #[error("invalid mod ID '{0}'")]
    InvalidModId(String),
    /// Structural or semantic error in `mod.toml` or persisted state schemas.
    #[error("manifest validation error: {0}")]
    ManifestValidation(String),
    /// Unsafe path component, absolute path, symlink, or reparse point rejected during load.
    #[error("unsafe path rejected: {0}")]
    UnsafePath(String),
    /// Missing asset file, hash/size mismatch, or corrupt binary payload structure.
    #[error("asset validation error: {0}")]
    AssetValidation(String),
    /// Manifest attempted to redistribute unchanged vanilla files or bundle a path on a reference.
    #[error("vanilla copy violation: {0}")]
    VanillaCopyViolation(String),
    /// Concurrent edits prevented stable hashing across bounded read attempts.
    #[error("mod inputs unstable during read: {0}")]
    UnstableInputs(String),
    /// Multiple directories claimed the same canonical mod identifier.
    #[error("duplicate mod ID '{id}' found in multiple directories: {}", format_paths(.paths))]
    DuplicateModId {
        /// Conflicting mod identifier.
        id: String,
        /// Discovered directories claiming `id`.
        paths: Vec<PathBuf>,
    },
    /// Invalid property patch arithmetic, target resolution, or runtime PROP compatibility error.
    #[error("patch validation error: {0}")]
    PatchValidation(String),
    /// Invalid lifecycle transition, reload mismatch, or sticky runtime-stop rejection.
    #[error("mod lifecycle error: {0}")]
    Lifecycle(String),
    /// Invalid DBPF package layout or disallowed metadata policy during overlay build/inspection.
    #[error("overlay package validation error: {0}")]
    PackageValidation(String),
}

fn format_paths(paths: &[PathBuf]) -> String {
    paths
        .iter()
        .map(|p| p.display().to_string())
        .collect::<Vec<_>>()
        .join("; ")
}

/// Lifecycle state tracked by [`crate::manager::ModManager`] for each discovered mod record.
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
pub enum ModState {
    /// Discovered and valid, but not currently registered in [`crate::patch_registry::PatchRegistry`].
    Inactive,
    /// Transiently preparing assets and persisting state before activation.
    Loading,
    /// Enabled, registered in [`crate::patch_registry::PatchRegistry`], and clean on disk.
    Active,
    /// Active with on-disk file modifications or validation diagnostics awaiting reload or disable.
    Changed,
    /// Transiently removing patches and persisting deactivation state.
    Unloading,
    /// Blocked from enabling due to validation failure, duplicate ID, or blocked capabilities.
    Failed,
    /// Active mod directory vanished or ceased to be readable on disk; unloaded from registry.
    Missing,
}

impl ModState {
    /// Return the canonical lowercase state name (`"inactive"`, `"active"`, etc.).
    pub fn as_str(self) -> &'static str {
        self.into()
    }
}

impl fmt::Display for ModState {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Deterministic arithmetic operation applied by a scalar float property patch.
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
pub enum PatchOperation {
    /// Overwrite the target property with `value`.
    Set,
    /// Add `value` to the current property float.
    Add,
    /// Subtract `value` from the current property float.
    Subtract,
    /// Multiply the current property float by `value`.
    Multiply,
    /// Divide the current property float by non-zero `value`.
    Divide,
}

impl PatchOperation {
    /// Return the canonical lowercase operation name.
    pub fn as_str(self) -> &'static str {
        self.into()
    }
}

/// Supported property value type for declarative Schema 2 patches.
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
pub enum PropertyValueType {
    /// 32-bit IEEE-754 scalar float (`sc_properties::PropType::Float`, `0x000D0000` runtime metadata).
    Float,
}

impl PropertyValueType {
    /// Return the canonical lowercase type name (`"float"`).
    pub fn as_str(self) -> &'static str {
        self.into()
    }
}

/// Supported asset payload types declared under `[[assets.new]]` in Schema 2 authoring manifests.
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
pub enum AssetKind {
    /// PNG raster texture/icon image verified via PNG signature and `IHDR` header.
    Png,
    /// Khronos glTF 2.0 binary 3D mesh/scene (`.glb`) verified for header, JSON chunk, and chunk bounds.
    Glb,
    /// JSON object describing GlassBox custom material parameters.
    #[strum(to_string = "material-params", serialize = "material_params")]
    MaterialParams,
    /// Encoded DBPF `0x00B1B104` property list verified via [`simoder_formats::PropertyFile::parse`].
    Prop,
    /// Compiled RenderWare 4 binary mesh/texture verified via RW4 header probe.
    Rw4,
    /// Non-empty custom binary payload not otherwise typed above.
    Raw,
}

impl AssetKind {
    /// Return the canonical kebab-case asset kind identifier.
    pub fn as_str(self) -> &'static str {
        self.into()
    }

    /// Return the default DBPF resource type ID assigned when packing an [`AssetKind`] without an explicit target.
    pub const fn default_type_id(self) -> u32 {
        match self {
            Self::Png => PNG_TYPE_ID,
            Self::Glb => GLB_TYPE_ID,
            Self::MaterialParams => MATERIAL_PARAMS_TYPE_ID,
            Self::Prop => PROP_TYPE_ID,
            Self::Rw4 => RW4_TYPE_ID,
            Self::Raw => RAW_ASSET_TYPE_ID,
        }
    }
}

/// Operation kind permitted per resource entry in Schema 3 raw-resources manifests.
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
pub enum RawResourceOperation {
    /// Register a brand-new resource TGI (requires [`RuntimeCapability::ResourceRegister`]).
    Add,
    /// Replace an existing vanilla resource TGI (requires [`RuntimeCapability::ResourceReplace`]).
    Replace,
}

impl RawResourceOperation {
    /// Return the runtime capability required by this resource operation.
    pub const fn required_capability(self) -> RuntimeCapability {
        match self {
            Self::Add => RuntimeCapability::ResourceRegister,
            Self::Replace => RuntimeCapability::ResourceReplace,
        }
    }
}

/// Privileged resource runtime capabilities declared by Schema 3 manifests.
#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    Hash,
    PartialOrd,
    Ord,
    Serialize,
    DeserializeFromStr,
    EnumString,
    IntoStaticStr,
    VariantArray,
)]
#[strum(ascii_case_insensitive)]
pub enum RuntimeCapability {
    /// Permission to register new resources into SimCity's resource manager (`"resource.register"`).
    #[strum(to_string = "resource.register")]
    #[serde(rename = "resource.register")]
    ResourceRegister,
    /// Permission to override existing base-game resources (`"resource.replace"`).
    #[strum(to_string = "resource.replace")]
    #[serde(rename = "resource.replace")]
    ResourceReplace,
}

impl RuntimeCapability {
    /// Return the canonical capability string identifier.
    pub fn as_str(self) -> &'static str {
        self.into()
    }
}

/// Filesystem mount location determining whether a `.package` can override base-game resources.
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
pub enum PackageMountLocation {
    /// Mounted alongside official game archives (`SimCityData/`); overrides base resources cleanly.
    SimCityData,
    /// Mounted in user profile packages (`Packages/`); does not override base-game resources directly.
    UserPackages,
}

/// Security policy verdict produced by inspecting package metadata records (`0x00B1B104` in `0x40404000`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PackageMetadataVerdict {
    /// Package contains zero `0x00B1B104` / `0x40404000` records and bypasses RSA signature checks.
    BypassSignature,
    /// Package contains exactly one package-info record; retail enforces RSA signature verification.
    RequiresRsaSignature,
    /// Package contains more than one package-info record (`count`) and is rejected by retail loader.
    RejectedDuplicateMetadata(usize),
}

/// 256-bit SHA-256 revision fingerprint over a mod's manifest and validated payloads.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ModFingerprint(pub [u8; 32]);

impl ModFingerprint {
    /// Format the 32-byte fingerprint as lowercase hexadecimal.
    pub fn to_hex(&self) -> String {
        hex::encode(self.0)
    }

    /// Borrow the underlying 32-byte digest array.
    pub const fn as_bytes(&self) -> &[u8; 32] {
        &self.0
    }
}

/// Descriptive metadata header declared under `[metadata]` in `mod.toml`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ModManifest {
    /// Globally unique canonical mod identifier (for example `"dawid.better-sanitizer"`).
    pub id: String,
    /// Human-readable display name.
    pub name: String,
    /// Mod semantic or release version string.
    pub version: String,
    /// Optional author or maintainer attribution.
    pub author: String,
    /// Optional summary of mod behavior.
    pub description: String,
}

/// Resolved symbol metadata and provenance attached when a property patch references `[symbols.properties]`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PropertySymbolOrigin {
    /// Symbol alias resolved from `[symbols.properties]`.
    pub name: String,
    /// Mandatory provenance source identifier from `[symbols.properties]`.
    pub source: String,
    /// Optional human-readable description from the property symbol entry.
    pub description: Option<String>,
    /// Optional unit tag (for example `"§"`, `"seconds"`) from the property symbol entry.
    pub unit: Option<String>,
    /// Optional provenance origin note from the property symbol entry.
    pub origin: Option<String>,
}

/// Single typed property modification targeting one property hash within a PROP resource.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PropertyPatch {
    /// FNV-1 property identifier hash inside the target `0x00B1B104` PROP list.
    pub property_id: u32,
    /// Validated property type (currently [`PropertyValueType::Float`]).
    pub value_type: PropertyValueType,
    /// Arithmetic operation to evaluate against the base float value.
    pub operation: PatchOperation,
    /// Finite IEEE-754 `f32` operand for `operation`.
    pub value: f32,
    /// Resolved symbol alias and provenance when specified via `[symbols.properties]`.
    pub symbol: Option<PropertySymbolOrigin>,
}

/// Resolved symbol metadata and provenance attached when a resource patch references `[symbols.resources]`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ResourceSymbolOrigin {
    /// Symbol alias resolved from `[symbols.resources]`.
    pub name: String,
    /// Mandatory provenance source identifier from `[symbols.resources]`.
    pub source: String,
}

mod tgi_serde {
    use super::*;

    pub fn serialize<S: Serializer>(tgi: &Tgi, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&tgi.to_string())
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Tgi, D::Error> {
        let raw = String::deserialize(deserializer)?;
        parse_tgi(&raw).map_err(de::Error::custom)
    }
}

mod opt_tgi_serde {
    use super::*;

    pub fn serialize<S: Serializer>(tgi: &Option<Tgi>, serializer: S) -> Result<S::Ok, S::Error> {
        match tgi {
            Some(value) => serializer.serialize_some(&value.to_string()),
            None => serializer.serialize_none(),
        }
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(
        deserializer: D,
    ) -> Result<Option<Tgi>, D::Error> {
        let raw = Option::<String>::deserialize(deserializer)?;
        raw.map(|text| parse_tgi(&text).map_err(de::Error::custom))
            .transpose()
    }
}

/// Set of property mutations targeting a single DBPF resource [`Tgi`].
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ResourcePatchDefinition {
    /// Exact target resource TGI receiving the property edits.
    #[serde(with = "tgi_serde")]
    pub target: Tgi,
    /// Resolved symbol alias and provenance when specified via `[symbols.resources]`.
    pub symbol: Option<ResourceSymbolOrigin>,
    /// Non-empty, property-ID-unique list of property patches to apply in order.
    pub properties: Vec<PropertyPatch>,
}

/// Mod-owned newly authored resource file under `assets/` declared in `[[assets.new]]`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NewAssetDefinition {
    /// Mod-local asset identifier referenced by materials and instance allocation.
    pub id: String,
    /// Expected format validated against payload magic/headers.
    pub kind: AssetKind,
    /// Normalized relative POSIX path starting with `"assets/"`.
    pub relative_path: String,
    /// Verified safe absolute path inside the mod directory (not stored across restarts).
    #[serde(skip)]
    pub resolved_path: PathBuf,
    /// Expected lowercase 64-character SHA-256 hex digest of the payload bytes.
    pub sha256: String,
    /// Expected exact payload file size in bytes.
    pub size: u64,
    /// Optional explicit DBPF target TGI overriding deterministic FNV-1 instance allocation.
    #[serde(with = "opt_tgi_serde")]
    pub target: Option<Tgi>,
}

/// Non-bundled vanilla game resource reference declared under `[[assets.references]]`.
///
/// Allows custom mods and material bundles to point to base-game TGIs without shipping or duplicating
/// copyrighted or unchanged vanilla asset files on disk.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct VanillaReferenceDefinition {
    /// Mod-local alias used when binding vanilla textures in material slots.
    pub alias: String,
    /// Vanilla DBPF target TGI referenced by the mod.
    #[serde(with = "tgi_serde")]
    pub target: Tgi,
    /// Mandatory provenance source identifier (for example `"SimCity_Graphics.package"`).
    pub source: String,
    /// Optional lowercase SHA-256 of the upstream vanilla resource used to block accidental copies in `[[assets.new]]`.
    pub expected_sha256: Option<String>,
}

/// Five-slot GlassBox material bundle linking parameter JSON and texture assets/references.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MaterialBundleDefinition {
    /// Material bundle identifier unique within the mod manifest.
    pub id: String,
    /// Asset ID of an [`AssetKind::MaterialParams`] entry declared under `[[assets.new]]`.
    pub params_asset_id: String,
    /// Five ordered texture bindings (`Texture0`..`Texture4`), referencing `assets.new` IDs or `assets.references` aliases.
    pub texture_slot_asset_ids: [String; MATERIAL_TEXTURE_SLOT_COUNT],
    /// Whether the material samples TEXCOORD1 tint coordinates.
    pub uses_tint_texcoord_1: bool,
}

/// Schema 3 raw binary DBPF entry validated under `[[resources]]`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BinaryResourceDefinition {
    /// Uppercase canonical `"TYPE:GROUP:16-HEX-INSTANCE"` DBPF identifier.
    pub id: String,
    /// Resource mutation operation (`add` or `replace`).
    pub operation: RawResourceOperation,
    /// Verified lowercase 64-character SHA-256 hex digest.
    pub sha256: String,
    /// Exact payload file length in bytes.
    pub size: u64,
    /// Normalized relative POSIX path starting with `"assets/"`.
    pub relative_path: String,
    /// Verified safe absolute path inside the mod directory.
    #[serde(skip)]
    pub resolved_path: PathBuf,
}

/// Validated `[scripts]` entry configuration declared in a `mod.toml` manifest.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ScriptModConfig {
    /// Script runtime language (`"luau"` or `"js"`).
    pub language: String,
    /// Normalized relative POSIX path to the entrypoint script inside the mod directory.
    pub entry: String,
    /// Optional per-VM memory cap override in MiB.
    pub memory_mib: Option<u32>,
    /// Optional per-tick execution budget override in milliseconds.
    pub budget_ms: Option<u32>,
    /// Verified safe non-symlink absolute path to `entry`.
    #[serde(skip)]
    pub resolved_path: PathBuf,
}

/// Validated `[native]` DLL configuration declared in a `mod.toml` manifest.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NativeModConfig {
    /// Normalized relative POSIX path to the `.dll` module inside the mod directory.
    pub entry: String,
    /// Declared host ABI contract version (must match ABI `2`).
    pub abi: u32,
    /// Optional lowercase 64-character SHA-256 hex digest pinned in the manifest.
    pub expected_sha256: Option<String>,
    /// Verified safe non-symlink absolute path to `entry`.
    #[serde(skip)]
    pub resolved_path: PathBuf,
}

/// Schema 2 authoring payload containing declarative property patches, newly authored assets,
/// unbundled vanilla references, custom 5-slot material bundles, and optional script/native entries.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct AuthoringModData {
    /// Validated Schema 2 property patch blocks (`[[patches]]`).
    pub patches: Vec<ResourcePatchDefinition>,
    /// Validated Schema 2 newly authored asset entries (`[[assets.new]]`).
    pub new_assets: Vec<NewAssetDefinition>,
    /// Validated Schema 2 unbundled vanilla TGI references (`[[assets.references]]`).
    pub vanilla_references: Vec<VanillaReferenceDefinition>,
    /// Validated Schema 2 material bundles (`[[assets.materials]]`).
    pub materials: Vec<MaterialBundleDefinition>,
    /// Validated optional `[scripts]` runtime entrypoint.
    pub scripts: Option<ScriptModConfig>,
    /// Validated optional `[native]` `.dll` module entrypoint.
    pub native: Option<NativeModConfig>,
}

/// Schema 3 raw binary DBPF resource payload gated by [`RuntimeCapability`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawResourcesModData {
    /// Validated Schema 3 raw binary resources (`[[resources]]`).
    pub resources: Vec<BinaryResourceDefinition>,
    /// Declared runtime capabilities required to execute `resources`.
    pub required_capabilities: Vec<RuntimeCapability>,
}

/// Mutually exclusive manifest payload matching either Schema 2 authoring or Schema 3 raw-resources.
#[derive(Debug, Clone, PartialEq)]
pub enum ModPayload {
    /// @summary Stores the declarative Schema 2 authoring payload indirectly to keep the enum compact.
    Authoring(Box<AuthoringModData>),
    /// Privileged Schema 3 raw DBPF resource payload (`simoder.raw-resources`).
    RawResources(RawResourcesModData),
}

/// Complete validated in-memory representation of a single mod loaded from `mod.toml`.
#[derive(Debug, Clone, PartialEq)]
pub struct ModDefinition {
    /// Canonical filesystem directory containing `mod.toml`.
    pub directory: PathBuf,
    /// Validated `[metadata]` block.
    pub manifest: ModManifest,
    /// Discriminated Schema 2 or Schema 3 payload data.
    pub payload: ModPayload,
}

impl ModDefinition {
    /// Return the Schema 2 authoring payload when this mod uses `simoder.authoring`.
    pub const fn authoring(&self) -> Option<&AuthoringModData> {
        match &self.payload {
            ModPayload::Authoring(data) => Some(data),
            ModPayload::RawResources(_) => None,
        }
    }

    /// Return the Schema 3 raw-resources payload when this mod uses `simoder.raw-resources`.
    pub const fn raw_resources(&self) -> Option<&RawResourcesModData> {
        match &self.payload {
            ModPayload::RawResources(data) => Some(data),
            ModPayload::Authoring(_) => None,
        }
    }

    /// Borrow declared property patch blocks (empty for Schema 3 raw-resources mods).
    pub fn patches(&self) -> &[ResourcePatchDefinition] {
        self.authoring().map_or(&[], |data| &data.patches)
    }

    /// Borrow declared new asset entries under `assets/` (empty for Schema 3 mods).
    pub fn new_assets(&self) -> &[NewAssetDefinition] {
        self.authoring().map_or(&[], |data| &data.new_assets)
    }

    /// Borrow declared unbundled vanilla TGI references (empty for Schema 3 mods).
    pub fn vanilla_references(&self) -> &[VanillaReferenceDefinition] {
        self.authoring()
            .map_or(&[], |data| &data.vanilla_references)
    }

    /// Borrow declared 5-slot material bundles (empty for Schema 3 mods).
    pub fn materials(&self) -> &[MaterialBundleDefinition] {
        self.authoring().map_or(&[], |data| &data.materials)
    }

    /// Return the optional validated `[scripts]` configuration when declared in `mod.toml`.
    pub fn scripts(&self) -> Option<&ScriptModConfig> {
        self.authoring().and_then(|data| data.scripts.as_ref())
    }

    /// Return the optional validated `[native]` DLL configuration when declared in `mod.toml`.
    pub fn native(&self) -> Option<&NativeModConfig> {
        self.authoring().and_then(|data| data.native.as_ref())
    }
}

/// Runtime trust and admission status for a discovered native `.dll` mod (`UI` / `Mods` panel).
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
pub enum NativeModStatus {
    /// Discovered valid PE32 `.dll` awaiting explicit user trust confirmation in the in-game `Mods` UI.
    PendingApproval,
    /// Explicitly approved by the user for this SHA-256 and admitted via `ABI v2`.
    Active,
    /// Explicitly declined or revoked by the user in `native_trust.toon` or the in-game `Mods` UI.
    Denied,
    /// Validation, staging, PE32 parsing, or `ABI v2` admission failed with a diagnostic.
    Failed,
}

impl NativeModStatus {
    /// Return the canonical kebab-case status string.
    pub fn as_str(self) -> &'static str {
        self.into()
    }
}

/// UI snapshot record describing a discovered `[native]` `.dll` mod for `Simoder-Base` / in-game approval.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct NativeModUiRecord {
    /// Canonical mod identifier (`metadata.id`).
    pub mod_id: String,
    /// Human-readable display name (`metadata.name`).
    pub name: String,
    /// Semantic mod version string (`metadata.version`).
    pub version: String,
    /// Normalized relative `.dll` entry declared under `[native]`.
    pub entry: String,
    /// Lowercase 64-character SHA-256 hex digest of the `.dll` bytes on disk.
    pub sha256: String,
    /// Current trust and admission status ([`NativeModStatus`]).
    pub status: NativeModStatus,
    /// Optional human-readable diagnostic or failure detail.
    pub diagnostic: Option<String>,
}

/// Read-only UI snapshot summarizing discovery, lifecycle, revision, and diagnostics for one mod.
#[derive(Debug, Clone, PartialEq)]
pub struct ModUiEntry {
    /// Validated mod ID, or empty when discovery failed before metadata parsing.
    pub id: String,
    /// Display name (or directory leaf name on early discovery failure).
    pub name: String,
    /// Declared mod version string.
    pub version: String,
    /// Declared mod author string.
    pub author: String,
    /// Current lifecycle [`ModState`].
    pub state: ModState,
    /// True when calling [`crate::manager::ModManager::set_enabled`] with `true` is valid.
    pub can_enable: bool,
    /// True when this mod is currently registered in [`crate::patch_registry::PatchRegistry`].
    pub can_disable: bool,
    /// True when the mod is in [`ModState::Changed`].
    pub has_pending_changes: bool,
    /// True when an active mod has a valid pending candidate eligible for [`crate::manager::ModManager::reload`].
    pub can_reload: bool,
    /// Lowercase SHA-256 hex revision token expected by [`crate::manager::ModManager::reload`], if pending.
    pub pending_revision: Option<String>,
    /// Optional human-readable error or change diagnostic message.
    pub diagnostic: Option<String>,
    /// Currently registered property patch blocks when active.
    pub active_patches: Vec<ResourcePatchDefinition>,
    /// Staged replacement property patch blocks when a valid edit is pending reload.
    pub pending_patches: Vec<ResourcePatchDefinition>,
    /// Count of newly authored assets (`[[assets.new]]`).
    pub new_asset_count: usize,
    /// Count of unbundled vanilla references (`[[assets.references]]`).
    pub vanilla_reference_count: usize,
}

/// Validate a canonical Simoder mod identifier (`[a-z0-9._-]`, `1..=128`, no leading/trailing/consecutive separators).
pub fn is_valid_mod_id(id: &str) -> bool {
    if id.is_empty() || id.len() > MAX_MOD_ID_LEN {
        return false;
    }
    let mut prev_separator = true; // Disallows leading separator as well.
    for byte in id.bytes() {
        let is_alnum = matches!(byte, b'a'..=b'z' | b'0'..=b'9');
        let is_sep = matches!(byte, b'.' | b'-' | b'_');
        if !is_alnum && !is_sep {
            return false;
        }
        if is_sep && prev_separator {
            return false;
        }
        prev_separator = is_sep;
    }
    !prev_separator
}
