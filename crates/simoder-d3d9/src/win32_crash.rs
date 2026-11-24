#![allow(unsafe_code)]

//! Win32 unhandled exception filter (`SetUnhandledExceptionFilter`) capturing faulting x86 context,
//! bounded `EBP`-chain stack frames, loaded module `RVA`s, and non-blocking log tails into
//! [`simoder_crash::CrashHandlerCoordinator`] (`CRASH-001..004`).

use std::ffi::c_void;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};

#[cfg(target_arch = "x86")]
use simoder_crash::MAX_CRASH_STACK_FRAMES;
use simoder_crash::{
    CrashHandlerCoordinator, ExceptionCode, ExceptionContext, FaultCaptureRequest,
    FaultClassification, LoadedModule, MAX_CRASH_LOG_TAIL_LINES, MemoryAccessKind,
    SymbolCatalog, UnwindCompleteness, redact_sensitive_text,
};

use crate::proxy_ffi::global_simoder_runtime;
use crate::win32_console::{current_thread_id, unix_time_ms};
use crate::win32_crash_ui::show_crash_report_window;

const EXCEPTION_CONTINUE_SEARCH: i32 = 0;
const EXCEPTION_EXECUTE_HANDLER: i32 = 1;
const EXCEPTION_MAXIMUM_PARAMETERS: usize = 15;
const TH32CS_SNAPMODULE: u32 = 0x0000_0008;
const INVALID_HANDLE_VALUE: *mut c_void = (-1isize) as *mut c_void;
const MAX_SNAPSHOT_MODULES: usize = 256;

#[repr(C)]
struct ExceptionRecord {
    exception_code: u32,
    exception_flags: u32,
    exception_record: *mut ExceptionRecord,
    exception_address: *mut c_void,
    number_parameters: u32,
    exception_information: [usize; EXCEPTION_MAXIMUM_PARAMETERS],
}

#[repr(C)]
struct ExceptionPointers {
    exception_record: *mut ExceptionRecord,
    context_record: *mut c_void,
}

#[repr(C)]
struct ModuleEntry32W {
    dw_size: u32,
    th32_module_id: u32,
    th32_process_id: u32,
    glblcnt_usage: u32,
    proccnt_usage: u32,
    mod_base_addr: *mut u8,
    mod_base_size: u32,
    h_module: *mut c_void,
    sz_module: [u16; 256],
    sz_exe_path: [u16; 260],
}

impl Default for ModuleEntry32W {
    fn default() -> Self {
        Self {
            dw_size: std::mem::size_of::<Self>() as u32,
            th32_module_id: 0,
            th32_process_id: 0,
            glblcnt_usage: 0,
            proccnt_usage: 0,
            mod_base_addr: std::ptr::null_mut(),
            mod_base_size: 0,
            h_module: std::ptr::null_mut(),
            sz_module: [0; 256],
            sz_exe_path: [0; 260],
        }
    }
}

type TopLevelExceptionFilter = unsafe extern "system" fn(*mut ExceptionPointers) -> i32;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn SetUnhandledExceptionFilter(
        lp_top_level_exception_filter: Option<TopLevelExceptionFilter>,
    ) -> Option<TopLevelExceptionFilter>;
    fn CreateToolhelp32Snapshot(dw_flags: u32, th32_process_id: u32) -> *mut c_void;
    fn Module32FirstW(h_snapshot: *mut c_void, lpme: *mut ModuleEntry32W) -> i32;
    fn Module32NextW(h_snapshot: *mut c_void, lpme: *mut ModuleEntry32W) -> i32;
    fn CloseHandle(h_object: *mut c_void) -> i32;
    fn GetCurrentProcessId() -> u32;
}

struct CrashRuntimeConfig {
    primary_crash_dir: PathBuf,
    fallback_crash_dir: PathBuf,
    log_dir: PathBuf,
}

static CRASH_CONFIG: OnceLock<CrashRuntimeConfig> = OnceLock::new();
static IN_CRASH_FILTER: AtomicBool = AtomicBool::new(false);

/// Install the process-wide Win32 `SetUnhandledExceptionFilter` crash handler once.
pub(crate) fn install_crash_handler(data_root: &Path) {
    let _ = CRASH_CONFIG.set(CrashRuntimeConfig {
        primary_crash_dir: data_root.join("crashes"),
        fallback_crash_dir: std::env::temp_dir().join("simoder-crashes"),
        log_dir: data_root.join("logs"),
    });
    // SAFETY: Installs a static `extern "system"` unhandled exception filter with re-entrancy guard.
    unsafe {
        let _ = SetUnhandledExceptionFilter(Some(unhandled_exception_filter));
    }
}

fn wide_z_to_string(buf: &[u16]) -> String {
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    String::from_utf16_lossy(&buf[..end])
}

fn collect_loaded_modules() -> Vec<LoadedModule> {
    // SAFETY: Enumerates loaded modules of the current process via Toolhelp32 snapshot.
    let snap = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId()) };
    if snap.is_null() || snap == INVALID_HANDLE_VALUE {
        return Vec::new();
    }
    let mut modules = Vec::new();
    let mut entry = ModuleEntry32W::default();
    // SAFETY: `entry.dw_size` is initialized to `size_of::<ModuleEntry32W>()`.
    if unsafe { Module32FirstW(snap, &mut entry) } != 0 {
        loop {
            let name = wide_z_to_string(&entry.sz_module);
            let path = redact_sensitive_text(&wide_z_to_string(&entry.sz_exe_path));
            modules.push(LoadedModule {
                name,
                path,
                base_address: entry.mod_base_addr as usize as u32,
                size_bytes: entry.mod_base_size,
                sha256: None,
            });
            if modules.len() >= MAX_SNAPSHOT_MODULES {
                break;
            }
            // SAFETY: Advances module enumeration within the same valid snapshot.
            if unsafe { Module32NextW(snap, &mut entry) } == 0 {
                break;
            }
        }
    }
    // SAFETY: Closes the Toolhelp32 snapshot handle.
    unsafe {
        let _ = CloseHandle(snap);
    }
    modules
}

#[cfg(target_arch = "x86")]
unsafe fn read_x86_context(ctx_ptr: *const c_void) -> (u32, u32, u32, [u32; 6]) {
    if ctx_ptr.is_null() {
        return (0, 0, 0, [0; 6]);
    }
    // Standard Win32 `winnt.h` x86 `CONTEXT` byte offsets:
    // Edi=0x9C, Esi=0xA0, Ebx=0xA4, Edx=0xA8, Ecx=0xAC, Eax=0xB0, Ebp=0xB4, Eip=0xB8, Esp=0xC4.
    let base = ctx_ptr as *const u8;
    // SAFETY: Caller passes a valid Win32 x86 `CONTEXT` pointer from `EXCEPTION_POINTERS`.
    let read_u32 = |off: usize| -> u32 { unsafe { base.add(off).cast::<u32>().read_unaligned() } };
    let edi = read_u32(0x9C);
    let esi = read_u32(0xA0);
    let ebx = read_u32(0xA4);
    let edx = read_u32(0xA8);
    let ecx = read_u32(0xAC);
    let eax = read_u32(0xB0);
    let ebp = read_u32(0xB4);
    let eip = read_u32(0xB8);
    let esp = read_u32(0xC4);
    (eip, esp, ebp, [eax, ebx, ecx, edx, esi, edi])
}

#[cfg(not(target_arch = "x86"))]
unsafe fn read_x86_context(_ctx_ptr: *const c_void) -> (u32, u32, u32, [u32; 6]) {
    (0, 0, 0, [0; 6])
}

#[cfg(target_arch = "x86")]
fn walk_x86_ebp_frames(start_ebp: u32) -> (Vec<u32>, UnwindCompleteness) {
    let mut frames = Vec::new();
    let mut cur_ebp = start_ebp;
    for _ in 0..(MAX_CRASH_STACK_FRAMES.saturating_sub(1)) {
        if cur_ebp < 0x1_0000 || (cur_ebp & 0x3) != 0 {
            return (frames, UnwindCompleteness::DamagedStackPointer);
        }
        let Some(next_ebp) = crate::win32_hook_util::read_struct::<u32>(cur_ebp as usize) else {
            return (frames, UnwindCompleteness::DamagedStackPointer);
        };
        let Some(ret_addr) = crate::win32_hook_util::read_struct::<u32>((cur_ebp + 4) as usize)
        else {
            return (frames, UnwindCompleteness::DamagedStackPointer);
        };
        if ret_addr == 0 {
            return (frames, UnwindCompleteness::Complete);
        }
        frames.push(ret_addr);
        if next_ebp <= cur_ebp || next_ebp.saturating_sub(cur_ebp) > 0x0010_0000 {
            return (frames, UnwindCompleteness::PartialLeafNoFramePointer);
        }
        cur_ebp = next_ebp;
    }
    (frames, UnwindCompleteness::TruncatedFrameLimit)
}

#[cfg(not(target_arch = "x86"))]
fn walk_x86_ebp_frames(_start_ebp: u32) -> (Vec<u32>, UnwindCompleteness) {
    (Vec::new(), UnwindCompleteness::PartialLeafNoFramePointer)
}

unsafe extern "system" fn unhandled_exception_filter(info: *mut ExceptionPointers) -> i32 {
    if IN_CRASH_FILTER
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    let Some(cfg) = CRASH_CONFIG.get() else {
        return EXCEPTION_CONTINUE_SEARCH;
    };
    if info.is_null() {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // SAFETY: `info` is non-null from Windows SEH dispatcher.
    let (rec_ptr, ctx_ptr) = unsafe { ((*info).exception_record, (*info).context_record) };
    if rec_ptr.is_null() {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // SAFETY: `rec_ptr` is non-null from `EXCEPTION_POINTERS`.
    let (raw_code, exc_addr, num_params, info0, info1) = unsafe {
        let r = &*rec_ptr;
        (
            r.exception_code,
            r.exception_address as usize as u32,
            r.number_parameters,
            r.exception_information[0],
            r.exception_information[1],
        )
    };

    let known_code = ExceptionCode::from_raw(raw_code);
    let (access_kind, access_target_address) = if raw_code == 0xC000_0005 && num_params >= 2 {
        (
            MemoryAccessKind::from_repr(info0 as u32),
            Some(info1 as u32),
        )
    } else {
        (None, None)
    };

    // SAFETY: `ctx_ptr` comes from `EXCEPTION_POINTERS` on the faulting thread.
    let (eip, esp, ebp, gpr) = unsafe { read_x86_context(ctx_ptr) };
    let effective_eip = if eip != 0 { eip } else { exc_addr };
    let (raw_return_addresses, unwind) = walk_x86_ebp_frames(ebp);
    let loaded_modules = collect_loaded_modules();

    let (log_tail, log_note) = match global_simoder_runtime()
        .logs()
        .try_recent_tail(MAX_CRASH_LOG_TAIL_LINES)
    {
        Some(lines) => (lines, String::new()),
        None => (
            Vec::new(),
            "Log tail skipped because the log pipeline mutex was held during the crash."
                .to_string(),
        ),
    };

    let req = FaultCaptureRequest {
        classification: FaultClassification::NativeFatal {
            code: known_code.unwrap_or(ExceptionCode::AccessViolation),
        },
        context: ExceptionContext {
            raw_exception_code: raw_code,
            known_code,
            faulting_thread_id: current_thread_id(),
            exception_address: exc_addr,
            access_kind,
            access_target_address,
            eip: effective_eip,
            esp,
            ebp,
            gpr,
        },
        raw_return_addresses,
        unwind,
        loaded_modules,
        notes: log_note,
        log_tail,
    };

    let coordinator = CrashHandlerCoordinator::new(
        &cfg.primary_crash_dir,
        &cfg.fallback_crash_dir,
        &cfg.log_dir,
        SymbolCatalog::with_retail_defaults(),
    );

    let Ok(report) = coordinator.capture_fault(&req) else {
        return EXCEPTION_CONTINUE_SEARCH;
    };
    let report_id = format!("simcity-{}", unix_time_ms());
    let saved_path = coordinator
        .persist_shareable_report(&report, &report_id)
        .ok();
    let Ok(shareable_text) = report.to_shareable_text() else {
        return EXCEPTION_CONTINUE_SEARCH;
    };

    let title = format!(
        "Simoder Crash Report ({:#010X})",
        req.context.raw_exception_code
    );
    show_crash_report_window(
        &title,
        &shareable_text,
        saved_path.as_deref(),
        &cfg.primary_crash_dir,
        &cfg.log_dir,
    );

    EXCEPTION_EXECUTE_HANDLER
}
