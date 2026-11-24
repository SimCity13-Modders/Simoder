#![forbid(unsafe_code)]

//! PE32 binary analysis built atop [`goblin::pe::PE`] for retail `SimCity.exe` verification,
//! RVA mapping, and x86 instruction reference scanning.

use std::fs::File;
use std::io::Read as _;
use std::path::Path;

pub use goblin::pe::header::COFF_MACHINE_X86;
use goblin::pe::header::Header;
use goblin::pe::optional_header::MAGIC_32;
pub use goblin::pe::section_table::IMAGE_SCN_MEM_EXECUTE;
use goblin::pe::{PE, options::ParseOptions};

use simoder_core::{Digest, LoadedModuleView, Sha256, hex, resolve_string_xref_getter};

use crate::D3d9Error;
use crate::config::{SUPPORTED_GAME_FILE_SHA256, SUPPORTED_GAME_TEXT_SHA256};

/// Maximum accepted PE32 image size (1 GiB defensive cap against malformed multi-gigabyte images).
pub const MAX_PE_FILE_BYTES: usize = 1024 * 1024 * 1024;

/// Maximum COFF section count accepted to avoid unbounded allocations on fuzzed PE headers.
const MAX_SECTIONS: usize = 96;

/// One validated section entry in an x86 PE32 image parsed via [`goblin`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PeSection {
    /// Null-trimmed or string-table section name (for example `".text"`, `".rdata"`).
    pub name: String,
    /// Relative Virtual Address when mapped in memory.
    pub virtual_address: u32,
    /// Virtual size when mapped in memory.
    pub virtual_size: u32,
    /// File byte offset of section raw payload.
    pub raw_offset: u32,
    /// File byte size of section raw payload.
    pub raw_size: u32,
    /// COFF section characteristic bitmask.
    pub characteristics: u32,
}

impl PeSection {
    /// Report whether the PE loader marks this section executable ([`IMAGE_SCN_MEM_EXECUTE`]).
    pub const fn is_executable(&self) -> bool {
        (self.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0
    }
}

/// Parsed 32-bit x86 PE32 binary backed by [`goblin::pe::PE`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Pe32Image {
    bytes: Vec<u8>,
    machine: u16,
    image_base: u32,
    entry_point_rva: u32,
    image_size: u32,
    sections: Vec<PeSection>,
}

/// Compatibility verification snapshot of a PE32 executable file and its `.text` section.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BuildAssessment {
    /// Lowercase SHA-256 hex digest of the full PE32 file.
    pub file_sha256: String,
    /// Lowercase SHA-256 hex digest of `.text` raw bytes if a `.text` section exists.
    pub text_sha256: Option<String>,
    /// COFF machine word from PE header (expected [`COFF_MACHINE_X86`]).
    pub machine: u16,
    /// Preferred PE32 image base address.
    pub image_base: u32,
    /// Mapped PE `SizeOfImage`.
    pub image_size: u32,
    /// `true` when `file_sha256` equals [`SUPPORTED_GAME_FILE_SHA256`].
    pub verified_retail_file: bool,
    /// `true` when `text_sha256` equals [`SUPPORTED_GAME_TEXT_SHA256`].
    pub verified_retail_text: bool,
}

impl Pe32Image {
    /// Parse and validate a complete 32-bit x86 PE32 image from an in-memory byte slice using [`goblin`].
    pub fn parse(bytes: &[u8]) -> Result<Self, D3d9Error> {
        Self::parse_owned(bytes.to_vec())
    }

    fn parse_owned(bytes: Vec<u8>) -> Result<Self, D3d9Error> {
        if bytes.len() < 0x100 || bytes.len() > MAX_PE_FILE_BYTES {
            return Err(D3d9Error::InvalidPe(
                "PE image is too small or exceeds defensive size limit".to_string(),
            ));
        }

        let sections = {
            let mut opts = ParseOptions::default();
            opts.parse_attribute_certificates = false;
            let pe = PE::parse_with_opts(&bytes, &opts)
                .map_err(|err| D3d9Error::InvalidPe(err.to_string()))?;

            let machine = pe.header.coff_header.machine;
            let Some(opt) = pe.header.optional_header else {
                return Err(D3d9Error::InvalidPe(
                    "PE image is missing optional header".to_string(),
                ));
            };
            if machine != COFF_MACHINE_X86 || pe.is_64 || opt.standard_fields.magic != MAGIC_32 {
                return Err(D3d9Error::InvalidPe(
                    "image is not a supported 32-bit x86 PE32 binary".to_string(),
                ));
            }
            if pe.sections.is_empty() || pe.sections.len() > MAX_SECTIONS {
                return Err(D3d9Error::InvalidPe(
                    "PE section count is zero or exceeds maximum".to_string(),
                ));
            }

            let image_base = u32::try_from(pe.image_base).map_err(|_| {
                D3d9Error::InvalidPe("PE32 image base exceeds 32-bit range".to_string())
            })?;
            let entry_point_rva = u32::try_from(pe.entry).map_err(|_| {
                D3d9Error::InvalidPe("PE32 entry point exceeds 32-bit range".to_string())
            })?;
            let image_size = opt.windows_fields.size_of_image;

            let mut sections = Vec::with_capacity(pe.sections.len());
            for sec in &pe.sections {
                let name = sec.name().unwrap_or("<invalid>").to_string();
                let raw_end = (sec.pointer_to_raw_data as usize)
                    .checked_add(sec.size_of_raw_data as usize)
                    .ok_or_else(|| D3d9Error::InvalidPe("section bounds overflow".to_string()))?;
                if raw_end > bytes.len() {
                    return Err(D3d9Error::InvalidPe(format!(
                        "PE section '{name}' raw payload extends outside image"
                    )));
                }
                let mapped_span = sec.virtual_size.max(sec.size_of_raw_data);
                if sec.virtual_address.checked_add(mapped_span).is_none() {
                    return Err(D3d9Error::InvalidPe(format!(
                        "PE section '{name}' virtual address range overflows 32-bit RVA"
                    )));
                }
                sections.push(PeSection {
                    name,
                    virtual_address: sec.virtual_address,
                    virtual_size: sec.virtual_size,
                    raw_offset: sec.pointer_to_raw_data,
                    raw_size: sec.size_of_raw_data,
                    characteristics: sec.characteristics,
                });
            }

            (machine, image_base, entry_point_rva, image_size, sections)
        };

        Ok(Self {
            bytes,
            machine: sections.0,
            image_base: sections.1,
            entry_point_rva: sections.2,
            image_size: sections.3,
            sections: sections.4,
        })
    }

    /// Open and parse an x86 PE32 file from disk within [`MAX_PE_FILE_BYTES`] without duplicate copies.
    pub fn open(path: &Path) -> Result<Self, D3d9Error> {
        let file = File::open(path)?;
        let len = file.metadata()?.len();
        if len < 0x100 || len > MAX_PE_FILE_BYTES as u64 {
            return Err(D3d9Error::InvalidPe(
                "PE file on disk is missing, too small, or exceeds size limit".to_string(),
            ));
        }
        let mut bytes = Vec::with_capacity(len as usize);
        file.take(MAX_PE_FILE_BYTES as u64 + 1)
            .read_to_end(&mut bytes)?;
        Self::parse_owned(bytes)
    }

    /// Return the COFF machine identifier (expected [`COFF_MACHINE_X86`]).
    pub const fn machine(&self) -> u16 {
        self.machine
    }

    /// Return the preferred PE32 image base address.
    pub const fn image_base(&self) -> u32 {
        self.image_base
    }

    /// Return the entry-point Relative Virtual Address (`AddressOfEntryPoint`).
    pub const fn entry_point_rva(&self) -> u32 {
        self.entry_point_rva
    }

    /// Return the mapped PE `SizeOfImage`.
    pub const fn image_size(&self) -> u32 {
        self.image_size
    }

    /// Borrow all parsed [`PeSection`] entries.
    pub fn sections(&self) -> &[PeSection] {
        &self.sections
    }

    /// Borrow the raw file bytes of this PE32 image.
    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// Find the first section with exact `name` (such as `".text"` or `".rdata"`).
    pub fn find_section(&self, name: &str) -> Option<&PeSection> {
        self.sections.iter().find(|sec| sec.name == name)
    }

    /// Find the section owning `rva`, accounting for `max(virtual_size, raw_size)`.
    pub fn find_section_for_rva(&self, rva: u32) -> Option<&PeSection> {
        self.sections.iter().find(|sec| {
            let extent = sec.virtual_size.max(sec.raw_size);
            rva >= sec.virtual_address && (rva - sec.virtual_address) < extent
        })
    }

    /// Map a raw file offset inside a section to its runtime Relative Virtual Address.
    pub fn file_offset_to_rva(&self, file_offset: u32) -> Option<u32> {
        self.sections.iter().find_map(|sec| {
            if file_offset >= sec.raw_offset && (file_offset - sec.raw_offset) < sec.raw_size {
                sec.virtual_address.checked_add(file_offset - sec.raw_offset)
            } else {
                None
            }
        })
    }

    /// Borrow the raw file bytes covered by `section`.
    pub fn section_raw_bytes(&self, section: &PeSection) -> Option<&[u8]> {
        let start = section.raw_offset as usize;
        let end = start.checked_add(section.raw_size as usize)?;
        self.bytes.get(start..end)
    }

    /// Scan all executable sections for a little-endian 32-bit `value` and return matching RVAs.
    pub fn find_value_in_executable_sections(&self, value: u32, max_hits: usize) -> Vec<u32> {
        let pattern = value.to_le_bytes();
        self.executable_windows(4)
            .filter_map(|(rva, window)| (window == pattern).then_some(rva))
            .take(max_hits)
            .collect()
    }

    /// Scan executable sections for x86 `E8 rel32` calls whose decoded target equals `target_va`, returning call RVAs.
    pub fn find_relative_call_references(&self, target_va: u32, max_hits: usize) -> Vec<u32> {
        const CALL_INSN_LEN: usize = 5;
        self.executable_windows(CALL_INSN_LEN)
            .filter_map(|(call_rva, window)| {
                if window[0] != 0xE8 {
                    return None;
                }
                let disp_bytes = <[u8; 4]>::try_from(&window[1..5]).ok()?;
                let displacement = i32::from_le_bytes(disp_bytes);
                let next_va =
                    i64::from(self.image_base) + i64::from(call_rva) + CALL_INSN_LEN as i64;
                (next_va + i64::from(displacement) == i64::from(target_va)).then_some(call_rva)
            })
            .take(max_hits)
            .collect()
    }

    /// Compute SHA-256 build identity and retail compatibility flags from this already-parsed PE32 image.
    pub fn assess(&self) -> BuildAssessment {
        let file_sha256 = hex::encode(Sha256::digest(self.as_bytes()));
        let text_sha256 = self
            .find_section(".text")
            .and_then(|sec| self.section_raw_bytes(sec))
            .map(|raw| hex::encode(Sha256::digest(raw)));

        let verified_retail_file = file_sha256.eq_ignore_ascii_case(SUPPORTED_GAME_FILE_SHA256);
        let verified_retail_text = text_sha256
            .as_deref()
            .is_some_and(|digest| digest.eq_ignore_ascii_case(SUPPORTED_GAME_TEXT_SHA256));

        BuildAssessment {
            file_sha256,
            text_sha256,
            machine: self.machine,
            image_base: self.image_base,
            image_size: self.image_size,
            verified_retail_file,
            verified_retail_text,
        }
    }

    fn executable_windows(&self, width: usize) -> impl Iterator<Item = (u32, &[u8])> + '_ {
        self.sections
            .iter()
            .filter(|sec| sec.is_executable())
            .filter_map(|sec| {
                let raw = self.section_raw_bytes(sec)?;
                Some((sec.virtual_address, raw))
            })
            .flat_map(move |(base_rva, raw)| {
                raw.windows(width).enumerate().filter_map(move |(offset, window)| {
                    u32::try_from(offset)
                        .ok()
                        .and_then(|rel| base_rva.checked_add(rel))
                        .map(|rva| (rva, window))
                })
            })
    }
}

/// Assess build identity and SHA-256 verification flags from an in-memory PE32 image slice.
pub fn assess_pe32_bytes(bytes: &[u8]) -> Result<BuildAssessment, D3d9Error> {
    Ok(Pe32Image::parse(bytes)?.assess())
}

/// Read an executable file from disk and return its [`BuildAssessment`] without re-parsing headers twice.
pub fn assess_game_executable(path: &Path) -> Result<BuildAssessment, D3d9Error> {
    Ok(Pe32Image::open(path)?.assess())
}

/// NUL-terminated console formatting literal inside `WebCore::ChromeClientEA::addMessageToConsole`.
const CONSOLE_FORMAT_ANCHOR: &[u8] = b"%s %s (%d): %s\0";

/// Maximum backward byte window from the `push imm32` anchor xref to search for `GetEAWebKitClient`.
const XREF_LOOKBACK_BYTES: usize = 0x100;

/// Resolved virtual addresses for `EA::WebKit::GetEAWebKitClient` and `gEAWebKitClient`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ResolvedEaWebKitClientGlobal {
    /// Virtual address of the unique `"%s %s (%d): %s\0"` anchor in `.rdata`.
    pub console_format_va: u32,
    /// Virtual address of the unique `push imm32` instruction referencing [`Self::console_format_va`].
    pub console_xref_va: u32,
    /// Virtual address of `EA::WebKit::GetEAWebKitClient` (`mov eax, [gEAWebKitClient]; ret`).
    pub get_client_va: u32,
    /// Virtual address of the global `EAWebKitClient*` pointer (`gEAWebKitClient`).
    pub client_global_ptr_va: u32,
}

/// Extract `.text` and `.rdata` `(virtual_address, virtual_size)` ranges from a loaded 32-bit PE header.
///
/// Uses [`goblin::pe::header::Header::parse`] so only the initial `4 KiB` header page of a mapped
/// module needs to be read.
pub fn parse_loaded_pe_text_and_rdata_ranges(
    header_bytes: &[u8],
) -> Option<((u32, u32), (u32, u32))> {
    let header = Header::parse(header_bytes).ok()?;
    let opt = header.optional_header?;
    if header.coff_header.machine != COFF_MACHINE_X86 || opt.standard_fields.magic != MAGIC_32 {
        return None;
    }

    let dos_pe_offset = header.dos_header.pe_pointer as usize;
    let coff_offset = dos_pe_offset.checked_add(4)?;
    let opt_size = header.coff_header.size_of_optional_header as usize;
    let sections_offset = coff_offset.checked_add(20)?.checked_add(opt_size)?;
    let section_count = header.coff_header.number_of_sections as usize;

    let mut text_range = None;
    let mut rdata_range = None;

    for idx in 0..section_count {
        let off = sections_offset.checked_add(idx.checked_mul(40)?)?;
        let sec = header_bytes.get(off..off + 40)?;
        let name = &sec[0..8];
        let v_size = u32::from_le_bytes(sec[8..12].try_into().ok()?);
        let v_addr = u32::from_le_bytes(sec[12..16].try_into().ok()?);
        let raw_size = u32::from_le_bytes(sec[16..20].try_into().ok()?);
        let size = v_size.max(raw_size);
        if size == 0 {
            continue;
        }
        if name.starts_with(b".text\0") || name == b".text\0\0\0" {
            text_range = Some((v_addr, size));
        } else if name.starts_with(b".rdata\0") || name == b".rdata\0\0" {
            rdata_range = Some((v_addr, size));
        }
    }

    Some((text_range?, rdata_range?))
}

/// Resolve `gEAWebKitClient` via the unique String-XREF anchor in `EAWebKit.dll`.
///
/// Returns `None` if any step finds `0` or `> 1` distinct matches, ensuring ambiguous
/// binaries fail closed rather than patching an unverified address.
pub fn resolve_eawebkit_client_global(
    image_base: u32,
    text_rva: u32,
    text_bytes: &[u8],
    rdata_rva: u32,
    rdata_bytes: &[u8],
) -> Option<ResolvedEaWebKitClientGlobal> {
    let view = LoadedModuleView {
        image_base,
        text_rva,
        text: text_bytes,
        rdata_rva,
        rdata: rdata_bytes,
    };
    let resolved = resolve_string_xref_getter(&view, CONSOLE_FORMAT_ANCHOR, XREF_LOOKBACK_BYTES)?;
    Some(ResolvedEaWebKitClientGlobal {
        console_format_va: resolved.string_va,
        console_xref_va: resolved.xref_va,
        get_client_va: resolved.getter_va,
        client_global_ptr_va: resolved.global_ptr_va,
    })
}
