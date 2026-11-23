#![allow(unsafe_code)]

//! Win32 Developer Console allocation (`AllocConsoleWithOptions`, `SW_SHOWNOACTIVATE`) and
//! background diagnostic log drain worker (`simoder/logs/sc13modloader.log` + VT100 `CONOUT$`).

use std::ffi::{CString, c_void};
use std::fs::{self, File, OpenOptions};
use std::io::Write as _;
use std::mem::transmute;
use std::path::Path;
use std::ptr::null_mut;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, OnceLock};
use std::thread::{self, JoinHandle};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use parking_lot::Mutex;
use simoder_core::{DiagnosticLogPipeline, LogConsoleFilter, LogRecord};
use simoder_crash::MAIN_LOG_RELATIVE_PATH;

type Handle = *mut c_void;
type FarProc = *const c_void;

const INVALID_HANDLE_ADDR: usize = !0usize;
const GENERIC_READ: u32 = 0x8000_0000;
const GENERIC_WRITE: u32 = 0x4000_0000;
const FILE_SHARE_READ_WRITE: u32 = 0x0000_0001 | 0x0000_0002;
const OPEN_EXISTING: u32 = 3;
const FILE_ATTRIBUTE_NORMAL: u32 = 0x0000_0080;
const CP_UTF8: u32 = 65_001;
const ENABLE_VIRTUAL_TERMINAL_PROCESSING: u32 = 0x0004;
const ALLOC_CONSOLE_MODE_NEW_WINDOW: i32 = 1;
const ALLOC_CONSOLE_RESULT_NEW_CONSOLE: i32 = 1;
const ALLOC_CONSOLE_RESULT_EXISTING_CONSOLE: i32 = 2;
const SW_SHOWNOACTIVATE: u16 = 4;

#[repr(C)]
struct AllocConsoleOptions {
    mode: i32,
    use_show_window: i32,
    show_window: u16,
    reserved: u16,
}

const _: () = assert!(size_of::<AllocConsoleOptions>() == 12);

#[repr(C)]
#[derive(Default)]
struct SystemTimeWin32 {
    year: u16,
    month: u16,
    day_of_week: u16,
    day: u16,
    hour: u16,
    minute: u16,
    second: u16,
    milliseconds: u16,
}

type FnAllocConsoleWithOptions = unsafe extern "system" fn(*mut AllocConsoleOptions, *mut i32) -> i32;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetModuleHandleW(lp_module_name: *const u16) -> Handle;
    fn GetProcAddress(h_module: Handle, lp_proc_name: *const i8) -> FarProc;
    fn CreateFileW(
        lp_file_name: *const u16,
        dw_desired_access: u32,
        dw_share_mode: u32,
        lp_security_attributes: *mut c_void,
        dw_creation_disposition: u32,
        dw_flags_and_attributes: u32,
        h_template_file: Handle,
    ) -> Handle;
    fn WriteFile(
        h_file: Handle,
        lp_buffer: *const u8,
        n_number_of_bytes_to_write: u32,
        lp_number_of_bytes_written: *mut u32,
        lp_overlapped: *mut c_void,
    ) -> i32;
    fn CloseHandle(h_object: Handle) -> i32;
    fn FreeConsole() -> i32;
    fn SetConsoleTitleW(lp_console_title: *const u16) -> i32;
    fn SetConsoleOutputCP(w_code_page_id: u32) -> i32;
    fn GetConsoleMode(h_console_handle: Handle, lp_mode: *mut u32) -> i32;
    fn SetConsoleMode(h_console_handle: Handle, dw_mode: u32) -> i32;
    fn GetLocalTime(lp_system_time: *mut SystemTimeWin32);
    fn GetCurrentThreadId() -> u32;
    fn AllocConsole() -> i32;
    fn ReadFile(
        h_file: Handle,
        lp_buffer: *mut u8,
        n_number_of_bytes_to_read: u32,
        lp_number_of_bytes_read: *mut u32,
        lp_overlapped: *mut c_void,
    ) -> i32;
    fn ExitProcess(u_exit_code: u32) -> !;
}

struct ConsoleHandle {
    raw_handle: usize,
    allocated_new: bool,
    ansi_enabled: bool,
}

impl ConsoleHandle {
    fn write_utf8(&self, text: &str) {
        let handle = self.raw_handle as Handle;
        if handle.is_null() || self.raw_handle == INVALID_HANDLE_ADDR {
            return;
        }
        let bytes = text.as_bytes();
        let mut offset = 0usize;
        while offset < bytes.len() {
            let chunk = &bytes[offset..];
            let len = chunk.len().min(u32::MAX as usize) as u32;
            let mut written: u32 = 0;
            // SAFETY: Valid `CONOUT$` handle owned exclusively by this logger worker until `Drop`.
            let ok = unsafe {
                WriteFile(
                    handle,
                    chunk.as_ptr(),
                    len,
                    &mut written,
                    null_mut(),
                )
            };
            if ok == 0 || written == 0 {
                break;
            }
            offset += written as usize;
        }
    }
}

impl Drop for ConsoleHandle {
    fn drop(&mut self) {
        if self.raw_handle != 0 && self.raw_handle != INVALID_HANDLE_ADDR {
            // SAFETY: Valid `CONOUT$` handle opened via `CreateFileW`.
            unsafe {
                let _ = CloseHandle(self.raw_handle as Handle);
            }
            self.raw_handle = 0;
        }
        if self.allocated_new {
            // SAFETY: Frees only the console window allocated by this logger.
            unsafe {
                let _ = FreeConsole();
            }
        }
    }
}

fn to_wide_null_str(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

fn try_allocate_developer_console() -> Option<ConsoleHandle> {
    let kernel32_name = to_wide_null_str("kernel32.dll");
    let proc_name = CString::new("AllocConsoleWithOptions").ok()?;
    let title = to_wide_null_str("Simoder Developer Console");
    let conout = to_wide_null_str("CONOUT$");

    // SAFETY: Dynamically resolves `AllocConsoleWithOptions` (`kernel32.dll`, Windows 11 24H2+),
    // allocates a non-focus-stealing console (`SW_SHOWNOACTIVATE`), configures UTF-8 + VT100, and opens `CONOUT$`.
    unsafe {
        let kernel32 = GetModuleHandleW(kernel32_name.as_ptr());
        if kernel32.is_null() {
            return None;
        }
        let proc_ptr = GetProcAddress(kernel32, proc_name.as_ptr());
        if proc_ptr.is_null() {
            return None;
        }
        let alloc_fn: FnAllocConsoleWithOptions = transmute::<FarProc, FnAllocConsoleWithOptions>(proc_ptr);

        let mut options = AllocConsoleOptions {
            mode: ALLOC_CONSOLE_MODE_NEW_WINDOW,
            use_show_window: 1,
            show_window: SW_SHOWNOACTIVATE,
            reserved: 0,
        };
        let mut alloc_result: i32 = 0;
        let hr = alloc_fn(&mut options, &mut alloc_result);
        if hr < 0
            || (alloc_result != ALLOC_CONSOLE_RESULT_NEW_CONSOLE
                && alloc_result != ALLOC_CONSOLE_RESULT_EXISTING_CONSOLE)
        {
            return None;
        }
        let allocated_new = alloc_result == ALLOC_CONSOLE_RESULT_NEW_CONSOLE;

        let _ = SetConsoleTitleW(title.as_ptr());
        let _ = SetConsoleOutputCP(CP_UTF8);

        let handle = CreateFileW(
            conout.as_ptr(),
            GENERIC_WRITE,
            FILE_SHARE_READ_WRITE,
            null_mut(),
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            null_mut(),
        );
        let raw_addr = handle as usize;
        if handle.is_null() || raw_addr == INVALID_HANDLE_ADDR {
            if allocated_new {
                let _ = FreeConsole();
            }
            return None;
        }

        let mut mode: u32 = 0;
        let ansi_enabled = GetConsoleMode(handle, &mut mode) != 0
            && SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;

        Some(ConsoleHandle {
            raw_handle: raw_addr,
            allocated_new,
            ansi_enabled,
        })
    }
}

/// Asynchronous logger draining [`DiagnosticLogPipeline`] batches into `simoder/logs/sc13modloader.log`
/// and an optional non-focus-stealing Windows 11 Developer Console (`CONOUT$`).
pub struct Win32AsyncLogger {
    stop_flag: Arc<AtomicBool>,
    console_active: Arc<AtomicBool>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl Win32AsyncLogger {
    /// Open `sc13modloader.log`, optionally allocate the Developer Console (`allow_console && filter.enabled`),
    /// and spawn the background drain thread.
    pub fn start(
        data_root: &Path,
        pipeline: Arc<DiagnosticLogPipeline>,
        filter: LogConsoleFilter,
        allow_console: bool,
    ) -> Self {
        let log_path = data_root.join(MAIN_LOG_RELATIVE_PATH);
        if let Some(parent) = log_path.parent() {
            let _ = fs::create_dir_all(parent);
        }
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&log_path)
            .ok();

        let console = if allow_console && filter.enabled {
            try_allocate_developer_console()
        } else {
            None
        };
        let console_is_open = console.is_some();

        let stop_flag = Arc::new(AtomicBool::new(false));
        let worker_stop = Arc::clone(&stop_flag);
        let console_active = Arc::new(AtomicBool::new(console_is_open));

        let handle = thread::Builder::new()
            .name("simoder-logger".to_string())
            .spawn(move || {
                logger_worker_loop(worker_stop, pipeline, filter, file, console);
            })
            .ok();

        Self {
            stop_flag,
            console_active,
            worker: Mutex::new(handle),
        }
    }

    /// Return `true` when the external Developer Console window (`CONOUT$`) is open.
    pub fn is_console_active(&self) -> bool {
        self.console_active.load(Ordering::Acquire)
    }

    /// Signal the logger worker to flush remaining events and exit cleanly.
    pub fn stop(&self) {
        self.stop_flag.store(true, Ordering::Release);
        self.console_active.store(false, Ordering::Release);
        if let Some(handle) = self.worker.lock().take() {
            handle.thread().unpark();
            let _ = handle.join();
        }
    }
}

fn logger_worker_loop(
    stop_flag: Arc<AtomicBool>,
    pipeline: Arc<DiagnosticLogPipeline>,
    filter: LogConsoleFilter,
    mut file: Option<File>,
    console: Option<ConsoleHandle>,
) {
    loop {
        let drained = drain_once(&pipeline, filter, file.as_mut(), console.as_ref());
        if stop_flag.load(Ordering::Acquire) {
            while !pipeline.is_empty() {
                drain_once(&pipeline, filter, file.as_mut(), console.as_ref());
            }
            break;
        }
        if !drained {
            thread::park_timeout(Duration::from_millis(25));
        }
    }
}

fn drain_once(
    pipeline: &DiagnosticLogPipeline,
    filter: LogConsoleFilter,
    file: Option<&mut File>,
    console: Option<&ConsoleHandle>,
) -> bool {
    let now_ms = unix_time_ms();
    let batch = pipeline.drain_batch(now_ms, filter);
    if batch.file_records.is_empty() && batch.console_records.is_empty() {
        return false;
    }

    let ts = current_local_timestamp();
    if let Some(f) = file {
        for rec in &batch.file_records {
            let line = format_plain_record(&ts, rec);
            let _ = f.write_all(line.as_bytes());
        }
        let _ = f.flush();
    }

    if let Some(con) = console {
        for rec in &batch.console_records {
            let line = format_console_record(&ts, rec, con.ansi_enabled);
            con.write_utf8(&line);
        }
    }

    true
}

fn format_plain_record(ts: &str, rec: &LogRecord) -> String {
    format!(
        "[{ts}] [{}] [{}] [{}] [T{}] {}\n",
        rec.source_type.as_str(),
        rec.source_name,
        rec.level.as_str(),
        rec.thread_id,
        rec.message
    )
}

fn format_console_record(ts: &str, rec: &LogRecord, ansi_enabled: bool) -> String {
    if ansi_enabled {
        format!(
            "[{ts}] [{}] [{}] [{}] [T{}] {}\r\n",
            rec.source_type.as_str(),
            rec.source_name,
            rec.level.colored_label(),
            rec.thread_id,
            rec.message
        )
    } else {
        format!(
            "[{ts}] [{}] [{}] [{}] [T{}] {}\r\n",
            rec.source_type.as_str(),
            rec.source_name,
            rec.level.as_str(),
            rec.thread_id,
            rec.message
        )
    }
}

fn current_local_timestamp() -> String {
    let mut st = SystemTimeWin32::default();
    // SAFETY: Valid mutable stack buffer passed to `GetLocalTime`.
    unsafe {
        GetLocalTime(&mut st);
    }
    format!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
        st.year, st.month, st.day, st.hour, st.minute, st.second, st.milliseconds
    )
}

/// Return current Unix epoch milliseconds.
pub fn unix_time_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as u64)
}

/// Return the caller's Win32 OS thread identifier (`GetCurrentThreadId`).
pub fn current_thread_id() -> u32 {
    // SAFETY: Lock-free Win32 TLS read of calling thread ID.
    unsafe { GetCurrentThreadId() }
}

static GLOBAL_LOGGER: OnceLock<Mutex<Option<Win32AsyncLogger>>> = OnceLock::new();

fn global_logger_slot() -> &'static Mutex<Option<Win32AsyncLogger>> {
    GLOBAL_LOGGER.get_or_init(|| Mutex::new(None))
}

/// Start (or verify already running) the global [`Win32AsyncLogger`] singleton and return whether
/// the external Developer Console opened successfully.
pub fn start_global_logger(
    data_root: &Path,
    pipeline: Arc<DiagnosticLogPipeline>,
    filter: LogConsoleFilter,
    allow_console: bool,
) -> bool {
    let mut guard = global_logger_slot().lock();
    if let Some(existing) = guard.as_ref() {
        return existing.is_console_active();
    }
    let logger = Win32AsyncLogger::start(data_root, pipeline, filter, allow_console);
    let active = logger.is_console_active();
    *guard = Some(logger);
    active
}

/// Return `true` when the global Developer Console (`CONOUT$`) is currently open.
pub fn is_developer_console_active() -> bool {
    global_logger_slot()
        .lock()
        .as_ref()
        .is_some_and(Win32AsyncLogger::is_console_active)
}

/// Stop and flush the global [`Win32AsyncLogger`] singleton during safe module detach.
pub fn stop_global_logger() {
    if let Some(logger) = global_logger_slot().lock().take() {
        logger.stop();
    }
}

/// Prompt the user in an allocated console window to download and bootstrap missing `Simoder-Base`.
/// If approved (`Y`), downloads the latest release from GitHub and extracts it to `base_dir`.
/// If denied (`N`), exits the game process cleanly.
pub fn prompt_and_bootstrap_base(mods_root: &Path) -> Result<(), String> {
    let base_dir = mods_root.join("simoder.base");
    if base_dir.exists() && base_dir.join("scripts").join("init.luau").exists() {
        return Ok(());
    }

    // Allocate or attach a console window.
    // SAFETY: Allocates standard console for interactive input.
    unsafe {
        AllocConsole();
        let title = to_wide_null_str("Simoder - Mandatory Base Module Bootstrap");
        let _ = SetConsoleTitleW(title.as_ptr());
        let _ = SetConsoleOutputCP(CP_UTF8);
    }

    let conin_name = to_wide_null_str("CONIN$");
    let conout_name = to_wide_null_str("CONOUT$");

    // SAFETY: Opening console stdio handles.
    let h_out = unsafe {
        CreateFileW(
            conout_name.as_ptr(),
            GENERIC_WRITE,
            FILE_SHARE_READ_WRITE,
            null_mut(),
            OPEN_EXISTING,
            0,
            null_mut(),
        )
    };
    let h_in = unsafe {
        CreateFileW(
            conin_name.as_ptr(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ_WRITE,
            null_mut(),
            OPEN_EXISTING,
            0,
            null_mut(),
        )
    };

    let write_str = |text: &str| {
        if !h_out.is_null() && (h_out as usize) != INVALID_HANDLE_ADDR {
            let bytes = text.as_bytes();
            let mut written: u32 = 0;
            // SAFETY: Writing prompt bytes to CONOUT$.
            unsafe {
                WriteFile(
                    h_out,
                    bytes.as_ptr(),
                    bytes.len() as u32,
                    &mut written,
                    null_mut(),
                );
            }
        }
    };

    let read_line = || -> String {
        if !h_in.is_null() && (h_in as usize) != INVALID_HANDLE_ADDR {
            let mut buf = [0u8; 128];
            let mut read_bytes: u32 = 0;
            // SAFETY: Reading user input from CONIN$.
            let ok = unsafe {
                ReadFile(
                    h_in,
                    buf.as_mut_ptr(),
                    buf.len() as u32,
                    &mut read_bytes,
                    null_mut(),
                )
            };
            if ok != 0 && read_bytes > 0 {
                return String::from_utf8_lossy(&buf[..read_bytes as usize]).to_string();
            }
        }
        String::new()
    };

    write_str("\r\n=======================================================\r\n");
    write_str(" [SIMODER BOOTSTRAP]\r\n");
    write_str(" Required base module 'simoder.base' is missing.\r\n");
    write_str(" Simoder cannot run without the base UI and capability engine.\r\n");
    write_str("=======================================================\r\n\r\n");
    write_str(" Do you want to download and install official 'Simoder-Base'\r\n");
    write_str(" from GitHub Releases? [Y/N]: ");

    let answer = read_line().trim().to_uppercase();
    if answer != "Y" && answer != "YES" {
        write_str("\r\n [SIMODER BOOTSTRAP] Installation canceled by user. Exiting game...\r\n");
        std::thread::sleep(std::time::Duration::from_millis(1500));
        // SAFETY: Exiting process on user cancellation.
        unsafe {
            ExitProcess(0);
        }
    }

    write_str("\r\n [SIMODER BOOTSTRAP] Querying latest release from GitHub...\r\n");

    let api_url = "https://api.github.com/repos/MiamiCenterPL/Simoder-Base/releases/latest";
    let release_info = match crate::net::get_json(api_url) {
        Ok(json) => json,
        Err(err) => {
            write_str(&format!(
                " [SIMODER BOOTSTRAP] Failed querying release: {err}\r\n Fallback to default branch zip...\r\n"
            ));
            String::new()
        }
    };

    // Parse download URL from release JSON or fallback to main branch zipball.
    let mut download_url = "https://github.com/MiamiCenterPL/Simoder-Base/archive/refs/heads/main.zip".to_string();
    if !release_info.is_empty()
        && let Ok(parsed) = serde_json::from_str::<serde_json::Value>(&release_info)
    {
        if let Some(assets) = parsed.get("assets").and_then(|a| a.as_array()) {
            for a in assets {
                if let Some(url) = a.get("browser_download_url").and_then(|u| u.as_str())
                    && url.ends_with(".zip")
                {
                    download_url = url.to_string();
                    break;
                }
            }
        }
        if download_url.ends_with("main.zip")
            && let Some(zip_url) = parsed.get("zipball_url").and_then(|u| u.as_str())
        {
            download_url = zip_url.to_string();
        }
    }

    write_str(&format!(" [SIMODER BOOTSTRAP] Downloading from: {}\r\n", download_url));

    let temp_zip = mods_root.join("simoder.base.bootstrap.zip");
    let dl_res = crate::net::download_file(&download_url, &temp_zip, |downloaded, total| {
        if let Some(tot) = total
            && let Some(pct) = downloaded.checked_mul(100).and_then(|value| value.checked_div(tot))
        {
            write_str(&format!("\r [SIMODER BOOTSTRAP] Downloading: {}% ({}/{} bytes)", pct, downloaded, tot));
        } else {
            write_str(&format!("\r [SIMODER BOOTSTRAP] Downloading: {} bytes...", downloaded));
        }
    });

    if let Err(e) = dl_res {
        write_str(&format!("\r\n [SIMODER BOOTSTRAP] ERROR: Download failed: {e}\r\n Exiting game...\r\n"));
        std::thread::sleep(std::time::Duration::from_millis(3000));
        // SAFETY: Exiting on download error.
        unsafe {
            ExitProcess(1);
        }
    }

    write_str("\r\n [SIMODER BOOTSTRAP] Extracting archive to 'mods/simoder.base'...\r\n");
    let extract_res = crate::net::extract_zip_archive(&temp_zip, &base_dir);
    let _ = std::fs::remove_file(&temp_zip);

    match extract_res {
        Ok(count) => {
            write_str(&format!(" [SIMODER BOOTSTRAP] Successfully extracted {} files.\r\n", count));
            write_str(" [SIMODER BOOTSTRAP] Base module ready! Launching SimCity...\r\n\r\n");
            std::thread::sleep(std::time::Duration::from_millis(1000));
            Ok(())
        }
        Err(err) => {
            write_str(&format!(" [SIMODER BOOTSTRAP] ERROR: Extraction failed: {err}\r\n Exiting game...\r\n"));
            std::thread::sleep(std::time::Duration::from_millis(3000));
            // SAFETY: Exiting on extraction failure.
            unsafe {
                ExitProcess(1);
            }
        }
    }
}
