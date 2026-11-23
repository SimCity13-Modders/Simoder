//! Structured [`CrashReport`] schema, build-verified [`SymbolCatalog`], single-owner [`CrashHandlerCoordinator`],
//! full-dump consent enforcement, and bounded report storage with fallback and rotation.

use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use serde::{Deserialize, Serialize};
use tempfile::NamedTempFile;

use crate::config::{
    MAX_CRASH_LOG_TAIL_LINES, MAX_CRASH_REPORT_BYTES, MAX_CRASH_STACK_FRAMES,
    MAX_RETAINED_CRASH_REPORTS, RETAIL_EAWEBKIT_DLL_SHA256, RETAIL_ECOGAME_GLOBAL_RULE_FROM_INDEX_VA,
    RETAIL_ECOGAME_LOG_ACTION_VA, RETAIL_ECOGAME_LOG_GLOBAL_RULE_VA, RETAIL_ECOGAME_LOG_UNIT_RULE_VA,
    RETAIL_ECOGAME_UNIT_RULE_FROM_INDEX_VA, RETAIL_MINIDUMP_HANDLER_VA, RETAIL_MINIDUMP_RETURN_VA,
    RETAIL_SIMCITY_EXE_SHA256,
};
use crate::redaction::{redact_sensitive_text, validate_shareable_redaction};
use crate::types::{
    CrashActionResponse, CrashError, CrashWindowAction, DumpCollectionConsent, ExceptionContext,
    FaultClassification, LoadedModule, StackFrame, SymbolStatus, UnwindCompleteness,
};

/// Allowlisted shareable Beta crash report payload with sensitive user paths and credentials redacted (`CRASH-004`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CrashReport {
    /// Human-readable fault summary (redacted).
    pub summary: String,
    /// Captured stack frames rendered as canonical strings (with sensitive user profile segments redacted).
    pub frames: Vec<String>,
    /// Additional redacted engine/loader notes.
    pub redacted_notes: String,
    /// Precise host executable build SHA-256 when known (`CRASH-003`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub build_sha256: Option<String>,
    /// Faulting thread and x86 register context (`CRASH-003`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub exception_context: Option<ExceptionContext>,
    /// Structured symbolicated stack frames (`CRASH-003`).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub structured_frames: Vec<StackFrame>,
    /// Explicit call-stack unwind completeness and limitation tag (`CRASH-003`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub unwind_completeness: Option<UnwindCompleteness>,
    /// Explicit notice reminding readers that the top stack frame does not prove fault ownership (`CRASH-002`).
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub ownership_disclaimer: String,
    /// Bounded recent diagnostic log tail (redacted, up to [`MAX_CRASH_LOG_TAIL_LINES`]).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub log_tail: Vec<String>,
}

impl CrashReport {
    /// Format this report as human-readable shareable plain text and validate that no sensitive data remains.
    pub fn to_shareable_text(&self) -> Result<String, CrashError> {
        let mut out = String::new();
        out.push_str("=== Simoder Beta Crash Report ===\n");
        out.push_str(&format!("Summary: {}\n", redact_sensitive_text(&self.summary)));
        if let Some(sha) = &self.build_sha256 {
            out.push_str(&format!("Build SHA-256: {sha}\n"));
        }
        if let Some(ctx) = &self.exception_context {
            let label = ctx.known_code.map_or("UNKNOWN_EXCEPTION", |c| c.as_str());
            out.push_str(&format!(
                "Faulting Thread: {} | Exception: {:#010X} ({label}) @ {:#010X}\n",
                ctx.faulting_thread_id, ctx.raw_exception_code, ctx.exception_address
            ));
            out.push_str(&format!(
                "Registers: EIP={:#010X} ESP={:#010X} EBP={:#010X}\n",
                ctx.eip, ctx.esp, ctx.ebp
            ));
        }
        if let Some(unwind) = self.unwind_completeness {
            let tag: &'static str = unwind.into();
            out.push_str(&format!("Unwind Completeness: {tag}\n"));
        }
        if !self.ownership_disclaimer.is_empty() {
            out.push_str(&format!(
                "Disclaimer: {}\n",
                redact_sensitive_text(&self.ownership_disclaimer)
            ));
        }
        out.push_str("\n--- Call Stack ---\n");
        for frame in &self.frames {
            out.push_str(&format!("  {}\n", redact_sensitive_text(frame)));
        }
        if !self.redacted_notes.is_empty() {
            out.push_str(&format!(
                "\n--- Notes ---\n{}\n",
                redact_sensitive_text(&self.redacted_notes)
            ));
        }
        if !self.log_tail.is_empty() {
            out.push_str("\n--- Recent Log Tail ---\n");
            for line in &self.log_tail {
                out.push_str(&format!("  {}\n", redact_sensitive_text(line)));
            }
        }
        validate_shareable_redaction(&out)?;
        Ok(out)
    }

    /// Serialize this report as pretty JSON after validating all fields pass producer redaction (`CRASH-004`).
    pub fn to_shareable_json(&self) -> Result<String, CrashError> {
        let _ = self.to_shareable_text()?;
        let json = serde_json::to_string_pretty(self)?;
        if json.len() > MAX_CRASH_REPORT_BYTES {
            return Err(CrashError::RedactionViolation(format!(
                "serialized crash report ({} bytes) exceeds {} byte limit",
                json.len(),
                MAX_CRASH_REPORT_BYTES
            )));
        }
        validate_shareable_redaction(&json)?;
        Ok(json)
    }
}

/// Build-verified symbol catalog entry mapping `[rva_start, rva_start + size)` to a verified symbol name.
#[derive(Debug, Clone, PartialEq, Eq)]
struct SymbolRange {
    rva_start: u32,
    size: u32,
    name: String,
}

/// Offline build-matched symbol catalog preventing guessed function names on mismatched binaries (`CRASH-003`).
#[derive(Debug, Clone, Default)]
pub struct SymbolCatalog {
    /// Keyed by lowercase module name -> `(expected_sha256, sorted_ranges)`.
    modules: BTreeMap<String, (String, Vec<SymbolRange>)>,
}

impl SymbolCatalog {
    /// Construct a symbol catalog pre-seeded with verified retail `SimCity.exe` (`10.3.6.0`) and
    /// `EAWebKit.dll` (`12.4.2.2.2`) anchor symbols from `Simoder-OpenSCP` RE evidence.
    pub fn with_retail_defaults() -> Self {
        let mut catalog = Self::default();
        let exe_base = 0x0040_0000_u32;
        for (va, size, name) in [
            (RETAIL_MINIDUMP_HANDLER_VA, 0x30, "SimCity::WriteCrashMiniDump"),
            (RETAIL_ECOGAME_LOG_ACTION_VA, 0x90, "cEcoGame::LogAction"),
            (RETAIL_ECOGAME_LOG_GLOBAL_RULE_VA, 0x80, "cEcoGame::LogGlobalRule"),
            (RETAIL_ECOGAME_LOG_UNIT_RULE_VA, 0x120, "cEcoGame::LogUnitRule"),
            (
                RETAIL_ECOGAME_GLOBAL_RULE_FROM_INDEX_VA,
                0x60,
                "cEcoGameScripts::GlobalRuleFromIndex",
            ),
            (
                RETAIL_ECOGAME_UNIT_RULE_FROM_INDEX_VA,
                0x60,
                "cEcoGameScripts::UnitRuleFromIndex",
            ),
        ] {
            catalog.register_symbol(
                "SimCity.exe",
                RETAIL_SIMCITY_EXE_SHA256,
                va - exe_base,
                size,
                name,
            );
        }
        catalog.register_symbol(
            "EAWebKit.dll",
            RETAIL_EAWEBKIT_DLL_SHA256,
            0x005A_C490,
            0x100,
            "WebCore::ChromeClientEA::addMessageToConsole",
        );
        catalog
    }

    /// Register a verified symbol range `[rva_start, rva_start + size)` for `(module_name, build_sha256)`.
    pub fn register_symbol(
        &mut self,
        module_name: &str,
        build_sha256: &str,
        rva_start: u32,
        size: u32,
        symbol_name: &str,
    ) {
        let key = module_name.to_ascii_lowercase();
        let entry = self
            .modules
            .entry(key)
            .or_insert_with(|| (build_sha256.to_ascii_lowercase(), Vec::new()));
        entry.0 = build_sha256.to_ascii_lowercase();
        entry.1.push(SymbolRange {
            rva_start,
            size: size.max(1),
            name: symbol_name.to_string(),
        });
        entry.1.sort_by_key(|r| r.rva_start);
    }

    /// Resolve a virtual address against `loaded_modules` without inventing symbol names when build hashes differ.
    pub fn symbolize_frame(&self, index: usize, va: u32, loaded_modules: &[LoadedModule]) -> StackFrame {
        let Some(module) = loaded_modules.iter().find(|m| m.contains_va(va).is_some()) else {
            return StackFrame {
                index,
                instruction_va: va,
                module_name: None,
                module_rva: None,
                symbol_name: None,
                symbol_displacement: None,
                status: SymbolStatus::UnmappedAddress,
                rendered: format!("<unmapped> [{va:#010X}]"),
            };
        };

        let rva = va - module.base_address;
        let mod_key = module.name.to_ascii_lowercase();
        if let Some((expected_sha, ranges)) = self.modules.get(&mod_key) {
            match module.sha256.as_deref() {
                Some(actual_sha) if actual_sha.eq_ignore_ascii_case(expected_sha) => {
                    if let Some(found) = ranges
                        .iter()
                        .find(|r| rva >= r.rva_start && rva < r.rva_start.saturating_add(r.size))
                    {
                        let disp = rva - found.rva_start;
                        return StackFrame {
                            index,
                            instruction_va: va,
                            module_name: Some(module.name.clone()),
                            module_rva: Some(rva),
                            symbol_name: Some(found.name.clone()),
                            symbol_displacement: Some(disp),
                            status: SymbolStatus::ExactBuildSymbol,
                            rendered: format!(
                                "{}!{}+{:#x} [{va:#010X}]",
                                module.name, found.name, disp
                            ),
                        };
                    }
                }
                Some(_) => {
                    return StackFrame {
                        index,
                        instruction_va: va,
                        module_name: Some(module.name.clone()),
                        module_rva: Some(rva),
                        symbol_name: None,
                        symbol_displacement: None,
                        status: SymbolStatus::BuildIdMismatch,
                        rendered: format!(
                            "{}+{rva:#010x} [{va:#010X}] (build-id mismatch; symbols suppressed)",
                            module.name
                        ),
                    };
                }
                None => {}
            }
        }

        StackFrame {
            index,
            instruction_va: va,
            module_name: Some(module.name.clone()),
            module_rva: Some(rva),
            symbol_name: None,
            symbol_displacement: None,
            status: SymbolStatus::ModuleOffsetOnly,
            rendered: format!("{}+{rva:#010x} [{va:#010X}]", module.name),
        }
    }
}

/// Request descriptor for [`CrashHandlerCoordinator::capture_fault`].
#[derive(Debug, Clone)]
pub struct FaultCaptureRequest {
    /// Classification distinguishing fatal native exceptions from isolated script errors.
    pub classification: FaultClassification,
    /// Faulting thread register and exception record context.
    pub context: ExceptionContext,
    /// Raw return addresses collected from the faulting thread stack (excluding `context.eip`, which is prepended).
    pub raw_return_addresses: Vec<u32>,
    /// Unwind completeness verdict computed by the stack walker.
    pub unwind: UnwindCompleteness,
    /// Modules loaded in the process at fault time.
    pub loaded_modules: Vec<LoadedModule>,
    /// Additional engine/loader diagnostic notes (will be redacted).
    pub notes: String,
    /// Recent log tail lines (will be redacted and bounded to [`MAX_CRASH_LOG_TAIL_LINES`]).
    pub log_tail: Vec<String>,
}

/// Outcome of a full memory dump request (`MiniDumpWriteDump` coexistence at `0x0091B4F5`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct FullDumpDescriptor {
    /// Path where the consented local minidump was written.
    pub dump_path: PathBuf,
    /// Retail `SimCity.exe` minidump handler entry VA (`0x0091B4F5`).
    pub retail_handler_va: u32,
    /// Retail `SimCity.exe` minidump return VA (`0x0091B502`).
    pub retail_return_va: u32,
    /// Always `false`: full dumps are never uploaded automatically.
    pub uploaded_over_network: bool,
}

/// Process-lifetime single-DLL crash coordinator enforcing re-entry protection, script-fault isolation,
/// redaction, consent gating, and bounded persistence with fallback directories (`CRASH-001..004`).
pub struct CrashHandlerCoordinator {
    in_crash: AtomicBool,
    active_thread_id: AtomicU32,
    catalog: SymbolCatalog,
    primary_crash_dir: PathBuf,
    fallback_crash_dir: PathBuf,
    log_dir: PathBuf,
}

impl CrashHandlerCoordinator {
    /// Construct a new coordinator bound to primary/fallback crash directories and `log_dir`.
    pub fn new(
        primary_crash_dir: impl Into<PathBuf>,
        fallback_crash_dir: impl Into<PathBuf>,
        log_dir: impl Into<PathBuf>,
        catalog: SymbolCatalog,
    ) -> Self {
        Self {
            in_crash: AtomicBool::new(false),
            active_thread_id: AtomicU32::new(0),
            catalog,
            primary_crash_dir: primary_crash_dir.into(),
            fallback_crash_dir: fallback_crash_dir.into(),
            log_dir: log_dir.into(),
        }
    }

    /// Always returns `false`: automatic network upload and external crash helper EXEs are prohibited (`CRASH-001`, `CRASH-004`).
    pub const fn network_upload_authorized(&self) -> bool {
        false
    }

    /// Capture and redact a fatal native fault, rejecting isolated script faults and re-entrant exceptions.
    pub fn capture_fault(&self, req: &FaultCaptureRequest) -> Result<CrashReport, CrashError> {
        match &req.classification {
            FaultClassification::IsolatedScriptError {
                mod_id, diagnostic, ..
            } => {
                return Err(CrashError::IsolatedScriptFaultNotFatal {
                    mod_id: mod_id.clone(),
                    diagnostic: diagnostic.clone(),
                });
            }
            FaultClassification::NativeFatal { .. } => {}
        }

        if self
            .in_crash
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Err(CrashError::ReentrantCrash {
                active_thread_id: self.active_thread_id.load(Ordering::Acquire),
            });
        }
        self.active_thread_id
            .store(req.context.faulting_thread_id, Ordering::Release);
        let _guard = ReentryResetGuard {
            flag: &self.in_crash,
            thread_slot: &self.active_thread_id,
        };

        let is_stack_overflow = req.context.raw_exception_code == 0xC000_00FD;
        let effective_unwind = if is_stack_overflow {
            UnwindCompleteness::StackExhaustedFallback
        } else if req.raw_return_addresses.len() + 1 > MAX_CRASH_STACK_FRAMES {
            UnwindCompleteness::TruncatedFrameLimit
        } else {
            req.unwind
        };

        let max_extra = if is_stack_overflow {
            1
        } else {
            MAX_CRASH_STACK_FRAMES - 1
        };

        let mut addresses = Vec::with_capacity(1 + req.raw_return_addresses.len().min(max_extra));
        addresses.push(req.context.eip);
        addresses.extend(req.raw_return_addresses.iter().copied().take(max_extra));

        let structured_frames: Vec<StackFrame> = addresses
            .into_iter()
            .enumerate()
            .map(|(idx, va)| self.catalog.symbolize_frame(idx, va, &req.loaded_modules))
            .collect();

        let frames: Vec<String> = structured_frames
            .iter()
            .map(|f| redact_sensitive_text(&f.rendered))
            .collect();

        let build_sha256 = req
            .loaded_modules
            .iter()
            .find(|m| m.name.eq_ignore_ascii_case("SimCity.exe"))
            .and_then(|m| m.sha256.clone());

        let code_label = req
            .context
            .known_code
            .map_or("UNKNOWN_EXCEPTION", |c| c.as_str());
        let summary = redact_sensitive_text(&format!(
            "Native fatal exception {:#010X} ({code_label}) on thread {} at {:#010X}",
            req.context.raw_exception_code,
            req.context.faulting_thread_id,
            req.context.exception_address
        ));

        let log_tail: Vec<String> = req
            .log_tail
            .iter()
            .rev()
            .take(MAX_CRASH_LOG_TAIL_LINES)
            .collect::<Vec<_>>()
            .into_iter()
            .rev()
            .map(|line| redact_sensitive_text(line))
            .collect();

        let report = CrashReport {
            summary,
            frames,
            redacted_notes: redact_sensitive_text(&req.notes),
            build_sha256,
            exception_context: Some(req.context.clone()),
            structured_frames,
            unwind_completeness: Some(effective_unwind),
            ownership_disclaimer:
                "Top stack frame records the faulting instruction address only and does not by itself prove mod or engine fault ownership."
                    .to_string(),
            log_tail,
        };

        // Validate redaction invariants immediately upon construction.
        let _ = report.to_shareable_text()?;
        Ok(report)
    }

    /// Persist a validated shareable [`CrashReport`] atomically, falling back to `fallback_crash_dir`
    /// when `primary_crash_dir` is write-denied, and rotating older reports beyond [`MAX_RETAINED_CRASH_REPORTS`].
    pub fn persist_shareable_report(
        &self,
        report: &CrashReport,
        report_id: &str,
    ) -> Result<PathBuf, CrashError> {
        let json_payload = report.to_shareable_json()?;
        let safe_stem: String = report_id
            .chars()
            .map(|c| {
                if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                    c
                } else {
                    '_'
                }
            })
            .collect();
        let file_name = format!("crash-{safe_stem}.json");

        let written_path = write_atomic_with_fallback(
            &self.primary_crash_dir,
            &self.fallback_crash_dir,
            &file_name,
            json_payload.as_bytes(),
        )?;

        if let Some(parent) = written_path.parent() {
            let _ = enforce_report_retention(parent, MAX_RETAINED_CRASH_REPORTS);
        }
        Ok(written_path)
    }

    /// Authorize and write a local full memory dump stub (`MiniDumpWriteDump` coexistence at `0x0091B4F5`)
    /// ONLY when `consent` is [`DumpCollectionConsent::FullMemoryDumpConsented`] with `warning_acknowledged: true`.
    pub fn write_full_memory_dump_if_consented(
        &self,
        consent: DumpCollectionConsent,
        report_id: &str,
        raw_minidump_bytes: &[u8],
    ) -> Result<FullDumpDescriptor, CrashError> {
        if !consent.allows_full_memory_dump() {
            return Err(CrashError::FullDumpConsentRequired);
        }
        let safe_stem: String = report_id
            .chars()
            .map(|c| {
                if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                    c
                } else {
                    '_'
                }
            })
            .collect();
        let file_name = format!("minidump-{safe_stem}.dmp");
        let dump_path = write_atomic_with_fallback(
            &self.primary_crash_dir,
            &self.fallback_crash_dir,
            &file_name,
            raw_minidump_bytes,
        )?;
        Ok(FullDumpDescriptor {
            dump_path,
            retail_handler_va: RETAIL_MINIDUMP_HANDLER_VA,
            retail_return_va: RETAIL_MINIDUMP_RETURN_VA,
            uploaded_over_network: false,
        })
    }

    /// Execute one of the three Beta crash window user actions (`CRASH-004`).
    pub fn execute_window_action(
        &self,
        action: CrashWindowAction,
        report: &CrashReport,
    ) -> Result<CrashActionResponse, CrashError> {
        match action {
            CrashWindowAction::CopyShareableReportToClipboard => {
                let text = report.to_shareable_text()?;
                Ok(CrashActionResponse::ClipboardPayload(text))
            }
            CrashWindowAction::OpenCrashReportFolder => {
                let dir = ensure_writable_dir(&self.primary_crash_dir, &self.fallback_crash_dir)?;
                Ok(CrashActionResponse::OpenedLocalFolder {
                    path: dir,
                    transmitted_over_network: false,
                })
            }
            CrashWindowAction::OpenLogFolder => {
                fs::create_dir_all(&self.log_dir)?;
                Ok(CrashActionResponse::OpenedLocalFolder {
                    path: self.log_dir.clone(),
                    transmitted_over_network: false,
                })
            }
        }
    }
}

struct ReentryResetGuard<'a> {
    flag: &'a AtomicBool,
    thread_slot: &'a AtomicU32,
}

impl Drop for ReentryResetGuard<'_> {
    fn drop(&mut self) {
        self.thread_slot.store(0, Ordering::Release);
        self.flag.store(false, Ordering::Release);
    }
}

fn ensure_writable_dir(primary: &Path, fallback: &Path) -> Result<PathBuf, std::io::Error> {
    if fs::create_dir_all(primary).is_ok() && NamedTempFile::new_in(primary).is_ok() {
        return Ok(primary.to_path_buf());
    }
    fs::create_dir_all(fallback)?;
    Ok(fallback.to_path_buf())
}

fn write_atomic_with_fallback(
    primary_dir: &Path,
    fallback_dir: &Path,
    file_name: &str,
    bytes: &[u8],
) -> Result<PathBuf, std::io::Error> {
    if let Ok(path) = try_write_atomic_in_dir(primary_dir, file_name, bytes) {
        return Ok(path);
    }
    try_write_atomic_in_dir(fallback_dir, file_name, bytes)
}

fn try_write_atomic_in_dir(
    dir: &Path,
    file_name: &str,
    bytes: &[u8],
) -> Result<PathBuf, std::io::Error> {
    fs::create_dir_all(dir)?;
    let mut tmp = NamedTempFile::new_in(dir)?;
    tmp.write_all(bytes)?;
    tmp.as_file().sync_all()?;
    let target = dir.join(file_name);
    tmp.persist(&target).map_err(|e| e.error)?;
    Ok(target)
}

fn enforce_report_retention(dir: &Path, max_retained: usize) -> Result<(), std::io::Error> {
    let mut files: Vec<PathBuf> = fs::read_dir(dir)?
        .filter_map(Result::ok)
        .map(|e| e.path())
        .filter(|p| {
            p.is_file()
                && p.file_name()
                    .and_then(|n| n.to_str())
                    .is_some_and(|n| n.starts_with("crash-") && n.ends_with(".json"))
        })
        .collect();
    files.sort();
    if files.len() > max_retained {
        let remove_count = files.len() - max_retained;
        for old in files.into_iter().take(remove_count) {
            let _ = fs::remove_file(old);
        }
    }
    Ok(())
}
