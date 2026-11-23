//! Shared filename constants, defensive parser limits, schema identifiers, DBPF metadata IDs,
//! menu-clone property hashes, and asset slot configuration for `simoder-mods`.

/// Canonical TOML manifest filename required inside every Simoder mod directory.
pub const MOD_MANIFEST_FILE_NAME: &str = "mod.toml";

/// Legacy manifest filenames that must not coexist with [`MOD_MANIFEST_FILE_NAME`].
pub const RETIRED_MANIFEST_FILES: [&str; 2] = ["mod.toon", "project.toml"];

/// Canonical TOON filename storing ordered active mod IDs across launches.
pub const STATE_FILE_NAME: &str = "state.toon";

/// Canonical TOON filename storing user-approved or denied native `.dll` mod SHA-256 trust decisions.
pub const NATIVE_TRUST_FILE_NAME: &str = "native_trust.toon";

/// Reserved internal mod identifier blocked from third-party authoring and raw manifests.
pub const RESERVED_BASE_MOD_ID: &str = "simoder.base";

/// Reserved directory name under the mods folder skipped case-insensitively during discovery.
pub const SHARED_CACHE_DIR_NAME: &str = "cache";

/// Required relative subdirectory prefix for bundled mod asset payloads.
pub const ASSETS_ROOT_DIR_NAME: &str = "assets";

/// Maximum manifest size accepted for `mod.toml` (8 MiB).
///
/// Mirrors the defensive manifest allocation ceiling enforced prior to TOML deserialization.
pub const MAX_MANIFEST_BYTES: usize = 8 * 1024 * 1024;

/// Maximum length in bytes of a validated [`crate::types::ModManifest::id`].
pub const MAX_MOD_ID_LEN: usize = 128;

/// Maximum length in bytes of standard identifier and label strings (`name`, `version`, `author`, aliases).
pub const MAX_FIELD_LEN: usize = 128;

/// Maximum length in bytes of provenance `source` metadata strings.
pub const MAX_SOURCE_LEN: usize = 1024;

/// Maximum length in bytes of free-form description strings.
pub const MAX_DESCRIPTION_LEN: usize = 4096;

/// Maximum number of resource entries permitted in a single Schema 3 raw-resources manifest.
pub const MAX_RESOURCE_ENTRIES: usize = 65_536;

/// Maximum payload size in bytes (256 MiB) permitted for a single bundled mod asset.
pub const MAX_ASSET_BYTES: u64 = 256 * 1024 * 1024;

/// Upper bound on persisted enabled mod IDs accepted when serializing `state.toon`.
///
/// Matches the bounded serializer index guard (`524_288`) used when writing TOON state snapshots.
pub const MAX_ENABLED_STATE_IDS: usize = 524_288;

/// Number of manifest/payload consistency passes performed by [`crate::manifest::load_stable_candidate`].
pub const CANDIDATE_STABILITY_ATTEMPTS: usize = 2;

/// Schema version number for authoring `mod.toml` manifests (`simoder.authoring`).
pub const AUTHORING_SCHEMA_VERSION: i64 = 2;

/// Canonical `format` identifier for authoring `mod.toml` manifests.
pub const AUTHORING_FORMAT: &str = "simoder.authoring";

/// Canonical `format` identifier for packaged native DevTool `mod.toml` manifests.
pub const NATIVE_DEVTOOL_FORMAT: &str = "simoder.native-devtool";

/// Schema version number for raw DBPF resource manifests (`simoder.raw-resources`).
pub const RAW_RESOURCES_SCHEMA_VERSION: i64 = 3;

/// Canonical `contract` identifier for raw DBPF resource manifests.
pub const RAW_RESOURCES_CONTRACT: &str = "simoder.raw-resources";

/// SimCity DBPF resource type ID for binary `0x00B1B104` property files (`PROP`).
///
/// Verified against OpenSCP (`sc_properties::PROPERTY_RESOURCE_TYPE`) and retail `SP::AddPackageIfValid`
/// (retail `SimCity.exe` VA `0x45EC40`).
pub const PROP_TYPE_ID: u32 = 0x00B1_B104;

/// DBPF group ID reserved for package metadata headers inspected by retail `SP::AddPackageIfValid` (VA `0x45EC40`).
///
/// Packages containing zero `0x00B1B104` records in group `0x40404000` register without RSA signatures;
/// one entry triggers `allowNoSignature = false`; more than one entry causes package rejection.
pub const PACKAGE_INFO_GROUP_ID: u32 = 0x4040_4000;

/// SimCity DBPF resource type ID for RenderWare 4 meshes/textures (`RW4`).
pub const RW4_TYPE_ID: u32 = 0x2F4E_681B;

/// SimCity DBPF resource type ID for PNG image resources.
pub const PNG_TYPE_ID: u32 = 0x2F7D_0004;

/// Custom Simoder DBPF overlay resource type ID for validated glTF 2.0 `.glb` payloads (`"SMGB"`).
pub const GLB_TYPE_ID: u32 = 0x534D_4742;

/// Custom Simoder DBPF overlay resource type ID for validated material parameter JSON (`"SMMP"`).
pub const MATERIAL_PARAMS_TYPE_ID: u32 = 0x534D_4D50;

/// Custom Simoder DBPF overlay resource type ID for generic raw mod asset payloads (`"SMRW"`).
pub const RAW_ASSET_TYPE_ID: u32 = 0x534D_5257;

/// Default DBPF group ID assigned when allocating deterministic TGIs for mod-local new assets (`"SMOD"`).
pub const MOD_ASSET_GROUP_ID: u32 = 0x534D_4F44;

/// Property hash `0x0DB9FC63` identifying a menu item's parent-menu TGI [`simoder_formats::Key`] reference.
///
/// Documented in OpenSCP package integration evidence for menu clone creation without breaking parent trees.
pub const MENU_PARENT_PROP_HASH: u32 = 0x0DB9_FC63;

/// Property hash `0x0977AA8F` identifying `menuIcon` TGI [`simoder_formats::Key`] references.
pub const MENU_ICON_PROP_HASH: u32 = 0x0977_AA8F;

/// Property hash `0x0DDEEE56` identifying `menuMarqueeImage` TGI [`simoder_formats::Key`] references.
pub const MENU_MARQUEE_IMAGE_PROP_HASH: u32 = 0x0DDE_EE56;

/// Property hash `0x0975695F` identifying `menuUnit` TGI [`simoder_formats::Key`] references.
pub const MENU_UNIT_PROP_HASH: u32 = 0x0975_695F;

/// Ordered array of all four property hashes rewritten when cloning retail building/unit menu PROPs.
pub const MENU_CLONE_REFERENCE_HASHES: [u32; 4] = [
    MENU_PARENT_PROP_HASH,
    MENU_ICON_PROP_HASH,
    MENU_MARQUEE_IMAGE_PROP_HASH,
    MENU_UNIT_PROP_HASH,
];

/// Fixed number of texture slots required for custom GlassBox building/unit material bundles.
///
/// Mirrors the 5-texture `Texture0`..`Texture4` GlassBox material binding layout (diffuse, normal/gloss,
/// specular/mask, night-illumination, and shared/tint detail).
pub const MATERIAL_TEXTURE_SLOT_COUNT: usize = 5;
