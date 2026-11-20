//! Path-traversal and symlink/reparse guards, SHA-256 helpers, external codec validation
//! (`png`, `gltf`, `PropertyFile`, `Rw4File`), instance allocation, and `walkdir` vanilla-copy audits.

use std::collections::HashSet;
use std::fs::{self, File};
use std::io::{self, Cursor};
use std::path::{Path, PathBuf};

use simoder_core::{Digest, Sha256, fnv1_name_hash, hex};
use simoder_formats::{PropertyFile, Rw4File};
use walkdir::WalkDir;

use crate::{config, types::*};

#[cfg(windows)]
const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x0400;

/// Inspect `path` using `symlink_metadata` and reject symbolic links or Windows reparse points.
pub fn reject_reparse_or_symlink(path: &Path) -> Result<fs::Metadata, ModError> {
    let meta = fs::symlink_metadata(path)?;
    if meta.file_type().is_symlink() {
        return Err(ModError::UnsafePath(format!(
            "symbolic link is not allowed: {}",
            path.display()
        )));
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt as _;
        if meta.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            return Err(ModError::UnsafePath(format!(
                "reparse point / junction is not allowed: {}",
                path.display()
            )));
        }
    }
    Ok(meta)
}

/// Safely resolve a portable POSIX `relative` asset path inside `mod_dir`.
///
/// Rejects backslashes, colons/drive letters, empty or traversal components (`.`, `..`), trailing
/// dots/spaces, control characters, wildcard characters, and any intermediate symlink or reparse
/// point before requiring the final target to be a regular file.
pub fn resolve_safe_relative_path(
    mod_dir: &Path,
    relative: &str,
    required_prefix: Option<&str>,
) -> Result<PathBuf, ModError> {
    if relative.is_empty()
        || relative.starts_with('/')
        || relative.ends_with('/')
        || relative.contains('\\')
        || relative.contains(':')
    {
        return Err(ModError::UnsafePath(format!(
            "asset path must be a normalized relative POSIX path without backslashes or colons: '{relative}'"
        )));
    }
    if let Some(prefix) = required_prefix
        && !relative.starts_with(prefix)
    {
        return Err(ModError::UnsafePath(format!(
            "asset path '{relative}' must reside under '{prefix}'"
        )));
    }

    let root_meta = reject_reparse_or_symlink(mod_dir)?;
    if !root_meta.is_dir() {
        return Err(ModError::UnsafePath(format!(
            "mod directory is not a directory: {}",
            mod_dir.display()
        )));
    }

    let mut current = mod_dir.to_path_buf();
    let mut final_meta = None;
    for component in relative.split('/') {
        if component.is_empty()
            || component == "."
            || component == ".."
            || component.ends_with('.')
            || component.ends_with(' ')
            || component
                .bytes()
                .any(|b| b < 32 || matches!(b, b'*' | b'?' | b'"' | b'<' | b'>' | b'|'))
        {
            return Err(ModError::UnsafePath(format!(
                "unsafe path component '{component}' in '{relative}'"
            )));
        }
        current.push(component);
        final_meta = Some(reject_reparse_or_symlink(&current)?);
    }

    let Some(meta) = final_meta else {
        return Err(ModError::UnsafePath(format!(
            "asset path resolved to no components: '{relative}'"
        )));
    };
    if !meta.is_file() {
        return Err(ModError::UnsafePath(format!(
            "asset target is not a regular file: '{relative}'"
        )));
    }
    Ok(current)
}

/// Compute the 32-byte SHA-256 digest of a slice in memory.
pub fn sha256_bytes(bytes: &[u8]) -> [u8; 32] {
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    hasher.finalize().into()
}

/// Stream a file through [`Sha256`] using [`std::io::copy`] without loading the entire payload at once.
pub fn sha256_file(path: &Path) -> Result<[u8; 32], ModError> {
    let mut file = File::open(path)?;
    let mut hasher = Sha256::new();
    io::copy(&mut file, &mut hasher)?;
    Ok(hasher.finalize().into())
}

/// Return `true` when `value` is a strictly lowercase 64-character SHA-256 hexadecimal digest.
pub fn is_lowercase_sha256_hex(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// Validate asset headers and decoded structure for an [`AssetKind`] using dedicated external codecs.
pub fn validate_asset_format(
    kind: AssetKind,
    bytes: &[u8],
    path_hint: &str,
) -> Result<(), ModError> {
    match kind {
        AssetKind::Png => validate_png(bytes, path_hint),
        AssetKind::Glb => validate_glb(bytes, path_hint),
        AssetKind::MaterialParams => {
            let parsed: serde_json::Value = serde_json::from_slice(bytes)?;
            if !parsed.is_object() {
                return Err(ModError::AssetValidation(format!(
                    "{path_hint}: material-params JSON root must be an object"
                )));
            }
            Ok(())
        }
        AssetKind::Prop => PropertyFile::parse(bytes).map(|_| ()).map_err(|err| {
            ModError::AssetValidation(format!("{path_hint}: invalid PROP binary: {err}"))
        }),
        AssetKind::Rw4 => Rw4File::parse(bytes).map(|_| ()).map_err(|err| {
            ModError::AssetValidation(format!("{path_hint}: invalid RW4 binary: {err}"))
        }),
        AssetKind::Raw => {
            if bytes.is_empty() {
                Err(ModError::AssetValidation(format!(
                    "{path_hint}: raw asset payload must not be empty"
                )))
            } else {
                Ok(())
            }
        }
    }
}

fn validate_png(bytes: &[u8], path_hint: &str) -> Result<(), ModError> {
    let reader = png::Decoder::new(Cursor::new(bytes))
        .read_info()
        .map_err(|err| ModError::AssetValidation(format!("{path_hint}: invalid PNG: {err}")))?;
    let info = reader.info();
    if info.width == 0 || info.height == 0 {
        return Err(ModError::AssetValidation(format!(
            "{path_hint}: PNG dimensions ({}x{}) must be non-zero",
            info.width, info.height
        )));
    }
    Ok(())
}

fn validate_glb(bytes: &[u8], path_hint: &str) -> Result<(), ModError> {
    let glb = gltf::Glb::from_slice(bytes).map_err(|err| {
        ModError::AssetValidation(format!("{path_hint}: invalid GLB container: {err}"))
    })?;
    if glb.header.length as usize != bytes.len() {
        return Err(ModError::AssetValidation(format!(
            "{path_hint}: GLB header length {} does not match file size {}",
            glb.header.length,
            bytes.len()
        )));
    }
    let gltf_doc = gltf::Gltf::from_slice(bytes).map_err(|err| {
        ModError::AssetValidation(format!("{path_hint}: invalid glTF 2.0 document: {err}"))
    })?;
    let version = &gltf_doc.document.as_json().asset.version;
    if version != "2" && !version.starts_with("2.") {
        return Err(ModError::AssetValidation(format!(
            "{path_hint}: unsupported glTF asset version '{version}'"
        )));
    }
    Ok(())
}

/// Derive a deterministic GlassBox 32-bit FNV-1 instance ID for a mod-authored resource (`"{mod_id}/{asset_id}"`).
///
/// Mirrors the FNV-1 derivation used for collision-free custom locale tables via [`fnv1_name_hash`].
pub fn allocate_mod_instance_id(mod_id: &str, asset_id: &str) -> u32 {
    fnv1_name_hash(&format!("{mod_id}/{asset_id}"))
}

/// Reject any declared [`NewAssetDefinition`] whose SHA-256 matches `known_vanilla_digests`.
pub fn validate_no_vanilla_copies(
    new_assets: &[NewAssetDefinition],
    known_vanilla_digests: &HashSet<String>,
) -> Result<(), ModError> {
    if known_vanilla_digests.is_empty() {
        return Ok(());
    }
    let normalized = normalized_digest_set(known_vanilla_digests);
    for asset in new_assets {
        let digest = asset.sha256.to_ascii_lowercase();
        if normalized.contains(&digest) {
            return Err(ModError::VanillaCopyViolation(format!(
                "new asset '{}' ({}) matches known vanilla SHA-256 {}; use [[assets.references]] instead",
                asset.id, asset.relative_path, digest
            )));
        }
    }
    Ok(())
}

/// Recursively audit every regular file under `mod_dir` (excluding root `mod.toml`) for copies
/// matching `known_vanilla_digests`, returning a canonically sorted list of offending file paths.
pub fn audit_mod_directory_no_vanilla_copies(
    mod_dir: &Path,
    known_vanilla_digests: &HashSet<String>,
) -> Result<Vec<PathBuf>, ModError> {
    reject_reparse_or_symlink(mod_dir)?;
    if known_vanilla_digests.is_empty() {
        return Ok(Vec::new());
    }
    let normalized = normalized_digest_set(known_vanilla_digests);
    let mut offenders = Vec::new();
    for entry in WalkDir::new(mod_dir).follow_links(false).sort_by_file_name() {
        let entry = entry.map_err(|err| {
            let path = err.path().map(|p| p.display().to_string()).unwrap_or_default();
            ModError::UnsafePath(format!("directory walk error at '{path}': {err}"))
        })?;
        if entry.depth() == 0 {
            continue;
        }
        let meta = reject_reparse_or_symlink(entry.path())?;
        let is_root_manifest = entry.depth() == 1
            && entry
                .file_name()
                .to_str()
                .is_some_and(|name| name.eq_ignore_ascii_case(config::MOD_MANIFEST_FILE_NAME));
        if meta.is_file() && !is_root_manifest {
            let digest = hex::encode(sha256_file(entry.path())?);
            if normalized.contains(&digest) {
                offenders.push(entry.into_path());
            }
        }
    }
    offenders.sort();
    Ok(offenders)
}

fn normalized_digest_set(digests: &HashSet<String>) -> HashSet<String> {
    digests.iter().map(|d| d.to_ascii_lowercase()).collect()
}
