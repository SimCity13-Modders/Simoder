//! Lightweight, safe ZIP archive parser and extractor supporting Store (0) and Deflate (8).

use std::fs::{self, File};
use std::io::{Cursor, Read, Seek, SeekFrom};
use std::path::{Component, Path, PathBuf};

use flate2::read::DeflateDecoder;

#[derive(Debug)]
struct CentralDirectoryEntry {
    compression_method: u16,
    compressed_size: u64,
    _uncompressed_size: u64,
    local_header_offset: u64,
    file_name: String,
}

/// Extract all entries from a ZIP archive at `zip_path` into `target_dir`.
/// Validates against directory traversal (Zip Slip vulnerability).
pub fn extract_zip_archive(zip_path: &Path, target_dir: &Path) -> Result<usize, String> {
    let mut file = File::open(zip_path)
        .map_err(|e| format!("Failed opening zip file '{}': {e}", zip_path.display()))?;

    let file_len = file
        .seek(SeekFrom::End(0))
        .map_err(|e| format!("Failed seeking zip file: {e}"))?;

    if file_len < 22 {
        return Err("File is too small to be a valid ZIP archive".into());
    }

    // Locate End of Central Directory Record (EOCD).
    // The EOCD record is 22 bytes, plus up to 65535 bytes of optional comment.
    let max_search = (file_len as usize).min(65535 + 22);
    let search_start = file_len - max_search as u64;
    file.seek(SeekFrom::Start(search_start))
        .map_err(|e| format!("Failed seeking for EOCD: {e}"))?;

    let mut search_buf = vec![0u8; max_search];
    file.read_exact(&mut search_buf)
        .map_err(|e| format!("Failed reading search buffer for EOCD: {e}"))?;

    let eocd_signature = [0x50, 0x4b, 0x05, 0x06];
    let mut eocd_pos_in_buf = None;
    for i in (0..=search_buf.len() - 22).rev() {
        if search_buf[i..i + 4] == eocd_signature {
            eocd_pos_in_buf = Some(i);
            break;
        }
    }

    let eocd_offset = eocd_pos_in_buf
        .ok_or_else(|| "Could not locate ZIP End of Central Directory (EOCD) signature".to_string())?;

    let mut cursor = Cursor::new(&search_buf[eocd_offset..]);
    let mut sig = [0u8; 4];
    cursor.read_exact(&mut sig).map_err(|e| e.to_string())?;
    let mut disk_num = [0u8; 2];
    cursor.read_exact(&mut disk_num).map_err(|e| e.to_string())?;
    let mut cd_disk = [0u8; 2];
    cursor.read_exact(&mut cd_disk).map_err(|e| e.to_string())?;
    let mut disk_entries = [0u8; 2];
    cursor.read_exact(&mut disk_entries).map_err(|e| e.to_string())?;
    let mut total_entries_bytes = [0u8; 2];
    cursor.read_exact(&mut total_entries_bytes).map_err(|e| e.to_string())?;
    let total_entries = u16::from_le_bytes(total_entries_bytes) as usize;

    let mut cd_size_bytes = [0u8; 4];
    cursor.read_exact(&mut cd_size_bytes).map_err(|e| e.to_string())?;
    let _cd_size = u32::from_le_bytes(cd_size_bytes) as u64;

    let mut cd_offset_bytes = [0u8; 4];
    cursor.read_exact(&mut cd_offset_bytes).map_err(|e| e.to_string())?;
    let cd_offset = u32::from_le_bytes(cd_offset_bytes) as u64;

    // Read Central Directory entries.
    file.seek(SeekFrom::Start(cd_offset))
        .map_err(|e| format!("Failed seeking to Central Directory at {cd_offset}: {e}"))?;

    let mut entries = Vec::with_capacity(total_entries);
    for _ in 0..total_entries {
        let mut header = [0u8; 46];
        file.read_exact(&mut header)
            .map_err(|e| format!("Failed reading Central Directory header: {e}"))?;

        if &header[0..4] != [0x50, 0x4b, 0x01, 0x02] {
            return Err("Invalid Central Directory header signature".into());
        }

        let compression = u16::from_le_bytes([header[10], header[11]]);
        let comp_size = u32::from_le_bytes([header[20], header[21], header[22], header[23]]) as u64;
        let uncomp_size = u32::from_le_bytes([header[24], header[25], header[26], header[27]]) as u64;
        let name_len = u16::from_le_bytes([header[28], header[29]]) as usize;
        let extra_len = u16::from_le_bytes([header[30], header[31]]) as usize;
        let comment_len = u16::from_le_bytes([header[32], header[33]]) as usize;
        let local_hdr_offset = u32::from_le_bytes([header[42], header[43], header[44], header[45]]) as u64;

        let mut name_bytes = vec![0u8; name_len];
        file.read_exact(&mut name_bytes)
            .map_err(|e| format!("Failed reading entry name: {e}"))?;
        let file_name = String::from_utf8_lossy(&name_bytes).replace('\\', "/");

        // Skip extra field + comment.
        if extra_len + comment_len > 0 {
            file.seek(SeekFrom::Current((extra_len + comment_len) as i64))
                .map_err(|e| format!("Failed skipping extra/comment in CD: {e}"))?;
        }

        entries.push(CentralDirectoryEntry {
            compression_method: compression,
            compressed_size: comp_size,
            _uncompressed_size: uncomp_size,
            local_header_offset: local_hdr_offset,
            file_name,
        });
    }

    // Determine if the archive has a single common root folder (e.g. `Simoder-Base-main/` from GitHub ZIP export).
    let common_root = find_common_root_prefix(&entries);

    fs::create_dir_all(target_dir)
        .map_err(|e| format!("Failed creating target directory '{}': {e}", target_dir.display()))?;

    let mut extracted_count = 0;

    for entry in entries {
        let relative_str = if let Some(root) = &common_root {
            if entry.file_name == *root || entry.file_name == format!("{root}/") {
                continue;
            }
            if let Some(stripped) = entry.file_name.strip_prefix(&format!("{root}/")) {
                stripped
            } else {
                &entry.file_name
            }
        } else {
            &entry.file_name
        };

        if relative_str.is_empty() {
            continue;
        }

        let is_dir = relative_str.ends_with('/');
        let safe_rel_path = sanitize_relative_path(relative_str)?;
        let dest_full_path = target_dir.join(&safe_rel_path);

        if is_dir {
            fs::create_dir_all(&dest_full_path)
                .map_err(|e| format!("Failed creating dir '{}': {e}", dest_full_path.display()))?;
            continue;
        }

        if let Some(parent) = dest_full_path.parent() {
            fs::create_dir_all(parent)
                .map_err(|e| format!("Failed creating parent dir '{}': {e}", parent.display()))?;
        }

        // Read local file header to find start of compressed data.
        file.seek(SeekFrom::Start(entry.local_header_offset))
            .map_err(|e| format!("Failed seeking to local header: {e}"))?;

        let mut loc_hdr = [0u8; 30];
        file.read_exact(&mut loc_hdr)
            .map_err(|e| format!("Failed reading local header: {e}"))?;

        if &loc_hdr[0..4] != [0x50, 0x4b, 0x03, 0x04] {
            return Err(format!(
                "Invalid local file header signature for '{}'",
                entry.file_name
            ));
        }

        let loc_name_len = u16::from_le_bytes([loc_hdr[26], loc_hdr[27]]) as i64;
        let loc_extra_len = u16::from_le_bytes([loc_hdr[28], loc_hdr[29]]) as i64;
        file.seek(SeekFrom::Current(loc_name_len + loc_extra_len))
            .map_err(|e| format!("Failed seeking past local extra: {e}"))?;

        let mut out_file = File::create(&dest_full_path)
            .map_err(|e| format!("Failed creating output file '{}': {e}", dest_full_path.display()))?;

        match entry.compression_method {
            0 => {
                // Store (no compression).
                let mut reader = (&mut file).take(entry.compressed_size);
                std::io::copy(&mut reader, &mut out_file)
                    .map_err(|e| format!("Failed copying uncompressed data for '{}': {e}", relative_str))?;
            }
            8 => {
                // Deflate.
                let reader = (&mut file).take(entry.compressed_size);
                let mut decoder = DeflateDecoder::new(reader);
                std::io::copy(&mut decoder, &mut out_file)
                    .map_err(|e| format!("Failed decompressing deflate data for '{}': {e}", relative_str))?;
            }
            other => {
                return Err(format!(
                    "Unsupported compression method {other} in entry '{}'",
                    entry.file_name
                ));
            }
        }

        extracted_count += 1;
    }

    Ok(extracted_count)
}

fn find_common_root_prefix(entries: &[CentralDirectoryEntry]) -> Option<String> {
    if entries.is_empty() {
        return None;
    }
    let first = &entries[0].file_name;
    let root_cand = match first.find('/') {
        Some(idx) => &first[..idx],
        None => return None,
    };
    if root_cand.is_empty() {
        return None;
    }
    let prefix = format!("{root_cand}/");
    for e in entries {
        if !e.file_name.starts_with(&prefix) && e.file_name != root_cand {
            return None;
        }
    }
    Some(root_cand.to_string())
}

fn sanitize_relative_path(path_str: &str) -> Result<PathBuf, String> {
    let raw_path = Path::new(path_str);
    let mut safe_path = PathBuf::new();
    for comp in raw_path.components() {
        match comp {
            Component::Normal(part) => safe_path.push(part),
            Component::CurDir => {}
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                return Err(format!(
                    "Path traversal attempt detected in archive entry: '{path_str}'"
                ));
            }
        }
    }
    if safe_path.as_os_str().is_empty() {
        return Err(format!("Invalid empty path for entry: '{path_str}'"));
    }
    Ok(safe_path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn path_sanitizer_rejects_parent_traversal() {
        assert!(sanitize_relative_path("../escape.exe").is_err());
        assert!(sanitize_relative_path("foo/../../bar").is_err());
        assert!(sanitize_relative_path("/etc/passwd").is_err());
        assert_eq!(
            sanitize_relative_path("scripts/init.luau").unwrap(),
            PathBuf::from("scripts/init.luau")
        );
    }
}
