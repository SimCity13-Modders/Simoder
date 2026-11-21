//! Strongly typed Beta crash diagnostic models, fault classifications, x86 exception contexts,
//! symbolication verdicts, consent policies, and window actions.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};
use serde_with::DeserializeFromStr;
use strum::{EnumString, FromRepr, IntoStaticStr, VariantArray};

/// Errors returned by crash handling, redaction validation, dump consent checks, and telemetry decoders.
#[derive(Debug, thiserror::Error)]
pub enum CrashError {
    /// Isolated script error was passed to the native fatal crash handler (`CRASH-002`).
    #[error("isolated script error '{mod_id}' must be handled by script fault policy, not the native crash window: {diagnostic}")]
    IsolatedScriptFaultNotFatal {
        /// Identifier of the faulting script mod.
        mod_id: String,
        /// Diagnostic message reported by the script VM.
        diagnostic: String,
    },
    /// Crash handler was re-entered while another exception was already being processed (`CRASH-001`).
    #[error("re-entrant exception blocked while crash handler is already active on thread {active_thread_id}")]
    ReentrantCrash {
        /// Thread ID currently holding the crash-handler latch.
        active_thread_id: u32,
    },
    /// Full process memory dump was requested without explicit user opt-in consent (`CRASH-004`).
    #[error("full memory dump requires explicit separate user consent with privacy warning acknowledgement")]
    FullDumpConsentRequired,
    /// Shareable report failed pre-export redaction verification (`CRASH-004`).
    #[error("shareable report contains unredacted sensitive data: {0}")]
    RedactionViolation(String),
    /// Unsafe hook target address rejected (for example shared `ret 4` stub `0x81E7C0`).
    #[error("unsafe diagnostic hook target rejected: {0}")]
    UnsafeHookTarget(String),
    /// Invalid or truncated rule telemetry stream header.
    #[error("rule telemetry decode error: {0}")]
    RuleStreamDecode(String),
    /// JSON serialization or deserialization failure.
    #[error("crash report serialization error: {0}")]
    Serialization(#[from] serde_json::Error),
    /// Filesystem I/O failure across both primary and fallback diagnostic directories.
    #[error("crash storage I/O error: {0}")]
    Io(#[from] std::io::Error),
}

/// Architectural distinction between fatal native exceptions and isolated mod script errors (`CRASH-002`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum FaultClassification {
    /// Fatal native hardware or OS exception requiring controlled crash capture and termination.
    NativeFatal {
        /// Structured Win32 SEH/VEH exception code.
        code: ExceptionCode,
    },
    /// Isolated Luau/JavaScript mod error governed by `DEV`/`REGULAR` script fault policy.
    IsolatedScriptError {
        /// Mod identifier owning the faulted VM generation.
        mod_id: String,
        /// Non-zero VM generation counter.
        generation: u64,
        /// Bounded script diagnostic message.
        diagnostic: String,
    },
}

/// Well-known Win32 x86 SEH/VEH exception codes observed in `SimCity.exe` crash diagnostics.
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
    FromRepr,
    VariantArray,
)]
#[repr(u32)]
#[strum(serialize_all = "SCREAMING_SNAKE_CASE", ascii_case_insensitive)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
pub enum ExceptionCode {
    /// `0xC0000005` (`STATUS_ACCESS_VIOLATION`).
    AccessViolation = 0xC000_0005,
    /// `0xC000001D` (`STATUS_ILLEGAL_INSTRUCTION`).
    IllegalInstruction = 0xC000_001D,
    /// `0xC0000094` (`STATUS_INTEGER_DIVIDE_BY_ZERO`).
    IntegerDivideByZero = 0xC000_0094,
    /// `0xC00000FD` (`STATUS_STACK_OVERFLOW`).
    StackOverflow = 0xC000_00FD,
    /// `0xC0000374` (`STATUS_HEAP_CORRUPTION`).
    HeapCorruption = 0xC000_0374,
    /// `0xC0000409` (`STATUS_STACK_BUFFER_OVERRUN`).
    StackBufferOverrun = 0xC000_0409,
    /// `0x80000003` (`STATUS_BREAKPOINT`).
    Breakpoint = 0x8000_0003,
}

impl ExceptionCode {
    /// Return the raw 32-bit NTSTATUS exception code value.
    pub const fn code(self) -> u32 {
        self as u32
    }

    /// Return the canonical uppercase identifier for this exception code.
    pub fn as_str(self) -> &'static str {
        self.into()
    }

    /// Resolve a raw 32-bit Win32 exception code if it matches a known variant.
    pub const fn from_raw(code: u32) -> Option<Self> {
        Self::from_repr(code)
    }
}

/// Memory access type recorded in `EXCEPTION_RECORD.ExceptionInformation[0]` on `STATUS_ACCESS_VIOLATION`.
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
    FromRepr,
    VariantArray,
)]
#[repr(u32)]
#[strum(serialize_all = "lowercase", ascii_case_insensitive)]
#[serde(rename_all = "lowercase")]
pub enum MemoryAccessKind {
    /// Read access violation (`ExceptionInformation[0] == 0`).
    Read = 0,
    /// Write access violation (`ExceptionInformation[0] == 1`).
    Write = 1,
    /// DEP/execute access violation (`ExceptionInformation[0] == 8`).
    Execute = 8,
}

/// Snapshot of the faulting x86 thread register and exception record context (`CRASH-003`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExceptionContext {
    /// Raw 32-bit Win32 exception code (for example `0xC0000005`).
    pub raw_exception_code: u32,
    /// Decoded symbolic exception code when recognized.
    pub known_code: Option<ExceptionCode>,
    /// OS thread identifier of the faulting thread (distinct from any external reporting thread).
    pub faulting_thread_id: u32,
    /// Faulting instruction address (`EXCEPTION_RECORD.ExceptionAddress`).
    pub exception_address: u32,
    /// Optional access violation operation (`Read`, `Write`, `Execute`).
    pub access_kind: Option<MemoryAccessKind>,
    /// Optional target virtual address accessed during `STATUS_ACCESS_VIOLATION`.
    pub access_target_address: Option<u32>,
    /// x86 instruction pointer (`EIP`).
    pub eip: u32,
    /// x86 stack pointer (`ESP`).
    pub esp: u32,
    /// x86 base frame pointer (`EBP`).
    pub ebp: u32,
    /// General-purpose x86 registers (`EAX`, `EBX`, `ECX`, `EDX`, `ESI`, `EDI`).
    pub gpr: [u32; 6],
}

/// Loaded PE32 module descriptor captured from the faulting process (`CRASH-003`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LoadedModule {
    /// Module filename (for example `"SimCity.exe"`, `"EAWebKit.dll"`, `"d3d9.dll"`).
    pub name: String,
    /// Redacted filesystem path of the module.
    pub path: String,
    /// Runtime module base virtual address in the 32-bit process space.
    pub base_address: u32,
    /// Mapped virtual size in bytes.
    pub size_bytes: u32,
    /// Lowercase hex SHA-256 build digest when verified on disk.
    pub sha256: Option<String>,
}

impl LoadedModule {
    /// Return the relative virtual address (`RVA`) if `va` falls within `[base_address, base_address + size_bytes)`.
    pub const fn contains_va(&self, va: u32) -> Option<u32> {
        if va >= self.base_address {
            let offset = va - self.base_address;
            if offset < self.size_bytes {
                return Some(offset);
            }
        }
        None
    }
}

/// Honesty tag indicating whether a stack frame could be symbolicated against an exact build ID (`CRASH-003`).
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
pub enum SymbolStatus {
    /// Module SHA-256 matched the symbol catalog and resolved an exact symbol + offset.
    ExactBuildSymbol,
    /// Address mapped to a loaded module, but no symbols were registered for its build.
    ModuleOffsetOnly,
    /// Module name matched a catalog entry, but its SHA-256 build digest differed; symbol names were suppressed.
    BuildIdMismatch,
    /// Address did not fall within any known loaded module (for example unloaded DLL or JIT page).
    UnmappedAddress,
}

/// Explicit unwind quality and limitation marker attached to a captured call stack (`CRASH-003`).
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
pub enum UnwindCompleteness {
    /// Walked cleanly to the thread base frame.
    Complete,
    /// Stopped after reaching [`crate::config::MAX_CRASH_STACK_FRAMES`].
    TruncatedFrameLimit,
    /// Fault was `STATUS_STACK_OVERFLOW`; minimal safe context captured without deep frame walking.
    StackExhaustedFallback,
    /// `EBP` chain was non-monotonic, misaligned, or outside the thread stack bounds.
    DamagedStackPointer,
    /// Leaf or FPO frame lacked frame pointers; recovered via verified return-address candidates.
    PartialLeafNoFramePointer,
}

/// One symbolicated or module-relative frame in the faulting thread call stack (`CRASH-003`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct StackFrame {
    /// Zero-based frame index (`0` = faulting instruction `EIP`).
    pub index: usize,
    /// Raw 32-bit instruction virtual address.
    pub instruction_va: u32,
    /// Containing module name if mapped.
    pub module_name: Option<String>,
    /// Module-relative offset (`RVA`) if mapped.
    pub module_rva: Option<u32>,
    /// Verified function or entry name only when [`SymbolStatus::ExactBuildSymbol`] holds.
    pub symbol_name: Option<String>,
    /// Byte displacement from the start of `symbol_name` when resolved.
    pub symbol_displacement: Option<u32>,
    /// Symbolication fidelity status.
    pub status: SymbolStatus,
    /// Canonical human-readable frame string (for example `"SimCity.exe!MiniDumpHandler+0x7 [0x0091B4FC]"`).
    pub rendered: String,
}

/// Separate user consent state governing full process memory dump creation (`CRASH-004`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub enum DumpCollectionConsent {
    /// Default policy: write only the allowlisted, redacted shareable text/JSON report.
    #[default]
    DefaultRedactedReportOnly,
    /// User was prompted for a full minidump and declined.
    FullDumpDeclined,
    /// User explicitly opted into writing a local full memory dump after acknowledging the raw-memory privacy warning.
    FullMemoryDumpConsented {
        /// Must be `true` to confirm the user acknowledged that raw memory is not redacted.
        warning_acknowledged: bool,
    },
}

impl DumpCollectionConsent {
    /// Return `true` only when the user explicitly opted into a full memory dump and acknowledged the privacy warning.
    pub const fn allows_full_memory_dump(self) -> bool {
        matches!(
            self,
            Self::FullMemoryDumpConsented {
                warning_acknowledged: true
            }
        )
    }
}

/// Interactive actions exposed by the Beta crash report window (`docs/architecture/crash-diagnostics.md`).
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
pub enum CrashWindowAction {
    /// Validate redaction and copy the shareable report text to the system clipboard.
    CopyShareableReportToClipboard,
    /// Open the local crash-report folder in Explorer without transmitting any files.
    OpenCrashReportFolder,
    /// Open the local diagnostic log folder in Explorer without transmitting any files.
    OpenLogFolder,
}

/// Result returned when executing a [`CrashWindowAction`].
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum CrashActionResponse {
    /// Redacted shareable report text validated and staged for clipboard copy.
    ClipboardPayload(String),
    /// Verified local directory path opened locally with zero network transmission.
    OpenedLocalFolder {
        /// Resolved local folder path.
        path: PathBuf,
        /// Always `false`: opening a folder never transmits its contents.
        transmitted_over_network: bool,
    },
}
