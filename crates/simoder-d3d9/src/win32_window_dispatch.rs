#![allow(unsafe_code)]

//! Owning-thread Win32 window subclassing, `WM_APP` dispatch, periodic UI timer,
//! and clean teardown for the verified game window thread.

use std::collections::VecDeque;
use std::ffi::c_void;
use std::ptr::null_mut;
use std::sync::mpsc::{self, SyncSender};
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicIsize, AtomicU32, AtomicUsize, Ordering};
use std::time::Duration;

use parking_lot::Mutex;

type Handle = *mut c_void;
type Hwnd = *mut c_void;

const GWLP_WNDPROC: i32 = -4;
const WM_DESTROY: u32 = 0x0002;
const WM_NCDESTROY: u32 = 0x0082;
const WM_NCHITTEST: u32 = 0x0084;
const WM_TIMER: u32 = 0x0113;
const WM_APP_SIMODER_SYNC: u32 = 0x8313;
const HTCLIENT: isize = 1;
const HTCAPTION: isize = 2;
const SIMODER_UI_TIMER_ID: usize = 0x534D_0001;
const UI_SYNC_INTERVAL_MS: u32 = 250;

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

type EnumWindowsProc = unsafe extern "system" fn(hwnd: Hwnd, lparam: isize) -> i32;
type WndProcFn = unsafe extern "system" fn(hwnd: Hwnd, msg: u32, wparam: usize, lparam: isize) -> isize;
type UiTickCallback = fn(usize);

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetCurrentProcessId() -> u32;
    fn GetCurrentThreadId() -> u32;
    fn GetLastError() -> u32;
    fn SetLastError(err: u32);
}

#[link(name = "user32")]
unsafe extern "system" {
    fn EnumWindows(lp_enum_func: EnumWindowsProc, lparam: isize) -> i32;
    fn GetWindowThreadProcessId(hwnd: Hwnd, lpdw_process_id: *mut u32) -> u32;
    fn IsWindow(hwnd: Hwnd) -> i32;
    fn IsWindowVisible(hwnd: Hwnd) -> i32;
    fn GetWindowRect(hwnd: Hwnd, lp_rect: *mut Rect) -> i32;
    fn CallWindowProcW(
        lp_prev_wnd_func: isize,
        hwnd: Hwnd,
        msg: u32,
        wparam: usize,
        lparam: isize,
    ) -> isize;
    fn DefWindowProcW(hwnd: Hwnd, msg: u32, wparam: usize, lparam: isize) -> isize;
    fn PostMessageW(hwnd: Hwnd, msg: u32, wparam: usize, lparam: isize) -> i32;
    fn SetTimer(hwnd: Hwnd, n_id_event: usize, u_elapse: u32, lp_timer_func: Handle) -> usize;
    fn KillTimer(hwnd: Hwnd, u_id_event: usize) -> i32;
    fn SetPropW(hwnd: Hwnd, lp_string: *const u16, h_data: Handle) -> i32;
    fn GetPropW(hwnd: Hwnd, lp_string: *const u16) -> Handle;
    fn RemovePropW(hwnd: Hwnd, lp_string: *const u16) -> Handle;
    #[cfg(target_pointer_width = "32")]
    fn SetWindowLongW(hwnd: Hwnd, n_index: i32, dw_new_long: isize) -> isize;
    #[cfg(target_pointer_width = "32")]
    fn GetWindowLongW(hwnd: Hwnd, n_index: i32) -> isize;
    #[cfg(target_pointer_width = "64")]
    fn SetWindowLongPtrW(hwnd: Hwnd, n_index: i32, dw_new_long: isize) -> isize;
    #[cfg(target_pointer_width = "64")]
    fn GetWindowLongPtrW(hwnd: Hwnd, n_index: i32) -> isize;
}

static PROP_NAME_W: OnceLock<Vec<u16>> = OnceLock::new();
static CURRENT_HWND: AtomicUsize = AtomicUsize::new(0);
static CURRENT_TID: AtomicU32 = AtomicU32::new(0);
static ORIGINAL_WNDPROC: AtomicIsize = AtomicIsize::new(0);
static DISPATCH_CLOSED: AtomicBool = AtomicBool::new(false);
static MODAL_OVERLAY_ACTIVE: AtomicBool = AtomicBool::new(false);
static ACTIVE_CALLS: AtomicU32 = AtomicU32::new(0);
static UI_TICK_CALLBACK: OnceLock<Mutex<Option<UiTickCallback>>> = OnceLock::new();
static PENDING_JS_EVALS: OnceLock<Mutex<VecDeque<PendingJsEval>>> = OnceLock::new();

struct PendingJsEval {
    script: String,
    arg: String,
    reply: SyncSender<Result<String, String>>,
}

fn pending_js_evals() -> &'static Mutex<VecDeque<PendingJsEval>> {
    PENDING_JS_EVALS.get_or_init(|| Mutex::new(VecDeque::new()))
}

fn drain_pending_js_evals(hwnd_val: usize) {
    loop {
        let next = pending_js_evals().lock().pop_front();
        let Some(req) = next else {
            break;
        };
        let res = crate::eawebkit_bridge::eval_active_view_script(hwnd_val, &req.script, &req.arg);
        let _ = req.reply.send(res);
    }
}

/// Evaluate a strict-ES5 JS function on the verified game window owning thread (`WM_APP_SIMODER_SYNC`).
pub fn eval_js_on_game_thread(
    script: &str,
    arg: &str,
    timeout_ms: u32,
) -> Result<String, String> {
    if DISPATCH_CLOSED.load(Ordering::Acquire) {
        return Err("game window dispatch is closed".into());
    }
    let hwnd_val = CURRENT_HWND.load(Ordering::Acquire);
    let tid = CURRENT_TID.load(Ordering::Acquire);
    if hwnd_val == 0 || tid == 0 {
        return Err("game window dispatch is not ready".into());
    }
    // SAFETY: Queries current Win32 thread ID.
    let current_tid = unsafe { GetCurrentThreadId() };
    if current_tid == tid {
        return crate::eawebkit_bridge::eval_active_view_script(hwnd_val, script, arg);
    }
    let (tx, rx) = mpsc::sync_channel(1);
    pending_js_evals().lock().push_back(PendingJsEval {
        script: script.to_string(),
        arg: arg.to_string(),
        reply: tx,
    });
    wake_ui_dispatch();
    let wait = Duration::from_millis(timeout_ms.clamp(100, 15_000) as u64);
    rx.recv_timeout(wait)
        .map_err(|err| format!("EAWebKit UI eval timed out or canceled: {err}"))?
}

fn prop_name() -> &'static [u16] {
    PROP_NAME_W.get_or_init(|| {
        "Simoder.Core.WindowAttachment.v2"
            .encode_utf16()
            .chain(std::iter::once(0))
            .collect()
    })
}

fn callback_slot() -> &'static Mutex<Option<UiTickCallback>> {
    UI_TICK_CALLBACK.get_or_init(|| Mutex::new(None))
}

#[cfg(target_pointer_width = "32")]
unsafe fn set_wnd_long(hwnd: Hwnd, index: i32, val: isize) -> isize {
    // SAFETY: Caller ensures valid `HWND` and 32-bit `GWLP_WNDPROC` target.
    unsafe { SetWindowLongW(hwnd, index, val) }
}

#[cfg(target_pointer_width = "64")]
unsafe fn set_wnd_long(hwnd: Hwnd, index: i32, val: isize) -> isize {
    // SAFETY: Caller ensures valid `HWND` and 64-bit `GWLP_WNDPROC` target.
    unsafe { SetWindowLongPtrW(hwnd, index, val) }
}

#[cfg(target_pointer_width = "32")]
unsafe fn get_wnd_long(hwnd: Hwnd, index: i32) -> isize {
    // SAFETY: Caller ensures valid `HWND`.
    unsafe { GetWindowLongW(hwnd, index) }
}

#[cfg(target_pointer_width = "64")]
unsafe fn get_wnd_long(hwnd: Hwnd, index: i32) -> isize {
    // SAFETY: Caller ensures valid `HWND`.
    unsafe { GetWindowLongPtrW(hwnd, index) }
}

struct ActiveCallGuard;
impl Drop for ActiveCallGuard {
    fn drop(&mut self) {
        ACTIVE_CALLS.fetch_sub(1, Ordering::AcqRel);
    }
}

unsafe extern "system" fn router_wndproc(
    hwnd: Hwnd,
    msg: u32,
    wparam: usize,
    lparam: isize,
) -> isize {
    ACTIVE_CALLS.fetch_add(1, Ordering::AcqRel);
    let _guard = ActiveCallGuard;

    let hwnd_val = hwnd as usize;
    let orig = ORIGINAL_WNDPROC.load(Ordering::Acquire);
    let attached = !DISPATCH_CLOSED.load(Ordering::Acquire)
        && hwnd_val != 0
        && hwnd_val == CURRENT_HWND.load(Ordering::Acquire)
        // SAFETY: Queries property stamped on subclassed `HWND`.
        && unsafe { GetPropW(hwnd, prop_name().as_ptr()) as usize } == hwnd_val
        // SAFETY: Confirms we are running on the verified window thread.
        && unsafe { GetCurrentThreadId() } == CURRENT_TID.load(Ordering::Acquire);

    if attached {
        if msg == WM_DESTROY || msg == WM_NCDESTROY {
            detach_bound_window(hwnd_val);
        } else if msg == WM_APP_SIMODER_SYNC || (msg == WM_TIMER && wparam == SIMODER_UI_TIMER_ID) {
            drain_pending_js_evals(hwnd_val);
            if let Some(cb) = *callback_slot().lock() {
                cb(hwnd_val);
            }
            if msg == WM_APP_SIMODER_SYNC {
                return 0;
            }
        }
    }

    // SAFETY: Forwards message to captured original `WNDPROC` (or `DefWindowProcW`).
    let result = unsafe {
        if orig != 0 {
            CallWindowProcW(orig, hwnd, msg, wparam, lparam)
        } else {
            DefWindowProcW(hwnd, msg, wparam, lparam)
        }
    };

    if attached
        && msg == WM_NCHITTEST
        && MODAL_OVERLAY_ACTIVE.load(Ordering::Acquire)
        && result == HTCAPTION
    {
        return HTCLIENT;
    }

    if msg == WM_NCDESTROY && hwnd_val == CURRENT_HWND.load(Ordering::Acquire) {
        // SAFETY: Cleans window property before window destruction completes.
        unsafe {
            let _ = RemovePropW(hwnd, prop_name().as_ptr());
        }
        CURRENT_HWND.store(0, Ordering::Release);
    }

    result
}

/// Track whether a Simoder modal overlay is open so caption drag hit-testing yields to DOM clicks.
pub fn set_modal_overlay_active(active: bool) {
    MODAL_OVERLAY_ACTIVE.store(active, Ordering::Release);
}

/// Configure the periodic owning-thread UI callback and wake signal.
pub fn configure_ui_tick(callback: UiTickCallback) {
    *callback_slot().lock() = Some(callback);
}

/// Locate the primary visible top-level window belonging to the current process.
pub fn find_primary_game_window() -> Option<usize> {
    struct Search {
        pid: u32,
        best_hwnd: usize,
        best_area: i64,
    }

    unsafe extern "system" fn enum_cb(hwnd: Hwnd, lparam: isize) -> i32 {
        // SAFETY: Stack pointer `&mut search` valid during synchronous `EnumWindows`.
        let search = unsafe { &mut *(lparam as *mut Search) };
        let mut pid: u32 = 0;
        // SAFETY: Valid `HWND` from `EnumWindows`.
        unsafe {
            let _ = GetWindowThreadProcessId(hwnd, &mut pid);
        }
        if pid != search.pid {
            return 1;
        }
        // SAFETY: Check visibility & area.
        if unsafe { IsWindowVisible(hwnd) } == 0 {
            return 1;
        }
        let mut rc = Rect::default();
        // SAFETY: Read outer window bounds.
        if unsafe { GetWindowRect(hwnd, &mut rc) } == 0 {
            return 1;
        }
        let area = ((rc.right - rc.left).max(0) as i64) * ((rc.bottom - rc.top).max(0) as i64);
        if area > search.best_area {
            search.best_area = area;
            search.best_hwnd = hwnd as usize;
        }
        1
    }

    // SAFETY: Current PID lookup.
    let pid = unsafe { GetCurrentProcessId() };
    let mut search = Search {
        pid,
        best_hwnd: 0,
        best_area: 0,
    };
    // SAFETY: Synchronous `EnumWindows` over stack `Search`.
    unsafe {
        let _ = EnumWindows(enum_cb, &mut search as *mut Search as isize);
    }
    (search.best_hwnd != 0).then_some(search.best_hwnd)
}

/// Return the currently bound primary game window `HWND`, or `0` when none is bound.
pub fn current_bound_window() -> usize {
    CURRENT_HWND.load(Ordering::Acquire)
}

fn detach_bound_window(hwnd_val: usize) {
    if hwnd_val == 0 {
        return;
    }
    if CURRENT_HWND
        .compare_exchange(hwnd_val, 0, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        return;
    }
    CURRENT_TID.store(0, Ordering::Release);
    let orig = ORIGINAL_WNDPROC.swap(0, Ordering::AcqRel);
    let hwnd = hwnd_val as Hwnd;
    // SAFETY: Detaches only if our `router_wndproc` remains topmost.
    unsafe {
        let _ = KillTimer(hwnd, SIMODER_UI_TIMER_ID);
        let _ = RemovePropW(hwnd, prop_name().as_ptr());
        if IsWindow(hwnd) != 0 {
            let router_ptr: WndProcFn = router_wndproc;
            if get_wnd_long(hwnd, GWLP_WNDPROC) == ((router_ptr as usize) as isize) && orig != 0 {
                let _ = set_wnd_long(hwnd, GWLP_WNDPROC, orig);
            }
        }
    }
}

/// Safely subclass `hwnd_val` once, stamp chain marker property, and start a 250ms UI sync timer.
pub fn bind_game_window(hwnd_val: usize) -> bool {
    if hwnd_val == 0 || DISPATCH_CLOSED.load(Ordering::Acquire) {
        return false;
    }
    let existing = CURRENT_HWND.load(Ordering::Acquire);
    if existing == hwnd_val {
        return true;
    }
    if existing != 0 {
        detach_bound_window(existing);
    }
    let hwnd = hwnd_val as Hwnd;
    // SAFETY: Verify valid in-process window before subclassing.
    unsafe {
        if IsWindow(hwnd) == 0 {
            return false;
        }
        let mut pid: u32 = 0;
        let tid = GetWindowThreadProcessId(hwnd, &mut pid);
        if tid == 0 || pid != GetCurrentProcessId() {
            return false;
        }
        if SetPropW(hwnd, prop_name().as_ptr(), hwnd as Handle) == 0 {
            return false;
        }
        SetLastError(0);
        let router_ptr: WndProcFn = router_wndproc;
        let prev = set_wnd_long(hwnd, GWLP_WNDPROC, router_ptr as isize);
        if prev == 0 && GetLastError() != 0 {
            let _ = RemovePropW(hwnd, prop_name().as_ptr());
            return false;
        }
        ORIGINAL_WNDPROC.store(prev, Ordering::Release);
        CURRENT_TID.store(tid, Ordering::Release);
        CURRENT_HWND.store(hwnd_val, Ordering::Release);
        let _ = SetTimer(hwnd, SIMODER_UI_TIMER_ID, UI_SYNC_INTERVAL_MS, null_mut());
        let _ = PostMessageW(hwnd, WM_APP_SIMODER_SYNC, 0, 0);
        true
    }
}

/// Enqueue an immediate UI sync message onto the subclassed game window thread.
pub fn wake_ui_dispatch() {
    let hwnd_val = CURRENT_HWND.load(Ordering::Acquire);
    if hwnd_val != 0 && !DISPATCH_CLOSED.load(Ordering::Acquire) {
        // SAFETY: Non-blocking `PostMessageW` to currently bound game window.
        unsafe {
            let _ = PostMessageW(hwnd_val as Hwnd, WM_APP_SIMODER_SYNC, 0, 0);
        }
    }
}

/// Detach our window hook if topmost, stop timers, and reject further UI callbacks.
pub fn shutdown_window_dispatch() {
    DISPATCH_CLOSED.store(true, Ordering::Release);
    MODAL_OVERLAY_ACTIVE.store(false, Ordering::Release);
    *callback_slot().lock() = None;
    {
        let mut queue = pending_js_evals().lock();
        while let Some(req) = queue.pop_front() {
            let _ = req.reply.send(Err("window dispatch shutting down".into()));
        }
    }
    let hwnd_val = CURRENT_HWND.swap(0, Ordering::AcqRel);
    if hwnd_val == 0 {
        return;
    }
    let hwnd = hwnd_val as Hwnd;
    let orig = ORIGINAL_WNDPROC.load(Ordering::Acquire);
    // SAFETY: Detaches only if our `router_wndproc` remains topmost.
    unsafe {
        let _ = KillTimer(hwnd, SIMODER_UI_TIMER_ID);
        let _ = RemovePropW(hwnd, prop_name().as_ptr());
        if IsWindow(hwnd) != 0 {
            let router_ptr: WndProcFn = router_wndproc;
            if get_wnd_long(hwnd, GWLP_WNDPROC) == (router_ptr as isize) && orig != 0 {
                let _ = set_wnd_long(hwnd, GWLP_WNDPROC, orig);
            }
        }
    }
}

/// Return true when no `router_wndproc` frame is currently executing.
pub fn is_window_dispatch_drained() -> bool {
    ACTIVE_CALLS.load(Ordering::Acquire) == 0
}
