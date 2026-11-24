#![allow(unsafe_code)]

//! Owning-thread Win32 window subclassing, `WM_APP` dispatch, periodic UI timer,
//! and clean teardown for the verified game window thread.

use std::collections::VecDeque;
use std::ffi::c_void;
use std::ptr::null_mut;
use std::sync::mpsc::{self, SyncSender};
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicIsize, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use parking_lot::Mutex;

type Handle = *mut c_void;
type Hwnd = *mut c_void;

const GWLP_WNDPROC: i32 = -4;
const WM_DESTROY: u32 = 0x0002;
const WM_NCDESTROY: u32 = 0x0082;
const WM_NCHITTEST: u32 = 0x0084;
const WM_TIMER: u32 = 0x0113;
const WM_MOUSEMOVE: u32 = 0x0200;
const WM_LBUTTONDOWN: u32 = 0x0201;
const WM_LBUTTONUP: u32 = 0x0202;
const WM_CAPTURECHANGED: u32 = 0x0215;
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
    fn GetConsoleWindow() -> Hwnd;
    fn GetCurrentProcessId() -> u32;
    fn GetCurrentThreadId() -> u32;
    fn GetLastError() -> u32;
    fn SetLastError(err: u32);
}

#[link(name = "user32")]
unsafe extern "system" {
    fn EnumWindows(lp_enum_func: EnumWindowsProc, lparam: isize) -> i32;
    fn GetClassNameW(hwnd: Hwnd, lp_class_name: *mut u16, n_max_count: i32) -> i32;
    fn GetWindowThreadProcessId(hwnd: Hwnd, lpdw_process_id: *mut u32) -> u32;
    fn IsWindow(hwnd: Hwnd) -> i32;
    fn IsWindowVisible(hwnd: Hwnd) -> i32;
    fn GetWindowRect(hwnd: Hwnd, lp_rect: *mut Rect) -> i32;
    fn GetClientRect(hwnd: Hwnd, lp_rect: *mut Rect) -> i32;
    fn SetCapture(hwnd: Hwnd) -> Hwnd;
    fn ReleaseCapture() -> i32;
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
static LAST_TICK_MS: AtomicU64 = AtomicU64::new(0);
static DEV_UI_OVERLAY_STATE: OnceLock<Mutex<DevUiOverlayHitState>> = OnceLock::new();
static ACTIVE_CALLS: AtomicU32 = AtomicU32::new(0);

#[derive(Default)]
struct DevUiOverlayHitState {
    overlay_rect: Option<[f32; 4]>,
    drag_handle_rect: Option<[f32; 4]>,
    dragging: bool,
    last_norm_pos: Option<[f32; 2]>,
    pending_delta: [f32; 2],
    last_wake: Option<Instant>,
}

fn dev_ui_overlay_state() -> &'static Mutex<DevUiOverlayHitState> {
    DEV_UI_OVERLAY_STATE.get_or_init(|| Mutex::new(DevUiOverlayHitState::default()))
}

fn client_norm_from_lparam(hwnd: Hwnd, lparam: isize) -> Option<[f32; 2]> {
    let mut cr = Rect::default();
    // SAFETY: Queries client bounds of the verified attached `HWND`.
    if unsafe { GetClientRect(hwnd, &mut cr) } == 0 {
        return None;
    }
    let w = (cr.right - cr.left).max(1) as f32;
    let h = (cr.bottom - cr.top).max(1) as f32;
    let cx = (lparam & 0xFFFF) as i16 as f32;
    let cy = ((lparam >> 16) & 0xFFFF) as i16 as f32;
    Some([cx / w, cy / h])
}

fn rect_contains(rect: [f32; 4], pos: [f32; 2]) -> bool {
    pos[0] >= rect[0] && pos[0] <= rect[2] && pos[1] >= rect[1] && pos[1] <= rect[3]
}
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
            LAST_TICK_MS.store(crate::win32_console::unix_time_ms(), Ordering::Release);
            drain_pending_js_evals(hwnd_val);
            if let Some(cb) = *callback_slot().lock() {
                cb(hwnd_val);
            }
            if msg == WM_APP_SIMODER_SYNC {
                return 0;
            }
        } else if msg == WM_LBUTTONDOWN {
            if let Some(pos) = client_norm_from_lparam(hwnd, lparam) {
                let mut st = dev_ui_overlay_state().lock();
                if let Some(handle) = st.drag_handle_rect
                    && rect_contains(handle, pos)
                {
                    st.dragging = true;
                    st.last_norm_pos = Some(pos);
                    st.pending_delta = [0.0, 0.0];
                    st.last_wake = None;
                    drop(st);
                    // SAFETY: Captures mouse for the verified attached `HWND` until `WM_LBUTTONUP`.
                    unsafe {
                        let _ = SetCapture(hwnd);
                    }
                    return 0;
                }
            }
        } else if msg == WM_MOUSEMOVE {
            let mut should_wake = false;
            let mut is_dragging = false;
            {
                let mut st = dev_ui_overlay_state().lock();
                if st.dragging {
                    is_dragging = true;
                    if let Some(pos) = client_norm_from_lparam(hwnd, lparam) {
                        if let Some(prev) = st.last_norm_pos {
                            st.pending_delta[0] += pos[0] - prev[0];
                            st.pending_delta[1] += pos[1] - prev[1];
                        }
                        st.last_norm_pos = Some(pos);
                    }
                    let now = Instant::now();
                    if st
                        .last_wake
                        .is_none_or(|t| now.duration_since(t) >= Duration::from_millis(16))
                    {
                        st.last_wake = Some(now);
                        should_wake = true;
                    }
                }
            }
            if is_dragging {
                if should_wake {
                    wake_ui_dispatch();
                }
                return 0;
            }
        } else if msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED {
            let was_dragging = {
                let mut st = dev_ui_overlay_state().lock();
                let was = st.dragging;
                st.dragging = false;
                st.last_norm_pos = None;
                was
            };
            if was_dragging {
                if msg == WM_LBUTTONUP {
                    // SAFETY: Releases mouse capture acquired on `WM_LBUTTONDOWN`.
                    unsafe {
                        let _ = ReleaseCapture();
                    }
                }
                wake_ui_dispatch();
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

    if attached && msg == WM_NCHITTEST && result == HTCAPTION {
        if MODAL_OVERLAY_ACTIVE.load(Ordering::Acquire) {
            return HTCLIENT;
        }
        let (dragging, overlay_rect) = {
            let st = dev_ui_overlay_state().lock();
            (st.dragging, st.overlay_rect)
        };
        if dragging {
            return HTCLIENT;
        }
        if let Some(rect) = overlay_rect {
            let sx = (lparam & 0xFFFF) as i16 as i32;
            let sy = ((lparam >> 16) & 0xFFFF) as i16 as i32;
            let mut wr = Rect::default();
            // SAFETY: Queries bounding rectangle of the verified attached `HWND`.
            if unsafe { GetWindowRect(hwnd, &mut wr) } != 0 {
                let w = (wr.right - wr.left).max(1) as f32;
                let h = (wr.bottom - wr.top).max(1) as f32;
                let nx = (sx - wr.left) as f32 / w;
                let ny = (sy - wr.top) as f32 / h;
                if rect_contains(rect, [nx, ny]) {
                    return HTCLIENT;
                }
            }
        }
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

/// Track the normalized `[left, top, right, bottom]` rectangles of any active `dev_ui` overlay
/// and its drag handle so `WM_NCHITTEST` and native Win32 drag capture operate smoothly.
pub fn set_dev_ui_overlay_rects(
    overlay_rect: Option<[f32; 4]>,
    drag_handle_rect: Option<[f32; 4]>,
) {
    let mut st = dev_ui_overlay_state().lock();
    st.overlay_rect = overlay_rect;
    st.drag_handle_rect = drag_handle_rect;
}

/// Drain any accumulated normalized `(dx, dy)` drag delta produced by Win32 drag-handle capture.
pub fn take_dev_ui_drag_delta() -> Option<[f32; 2]> {
    let mut st = dev_ui_overlay_state().lock();
    let [dx, dy] = st.pending_delta;
    if dx.abs() < 1e-5 && dy.abs() < 1e-5 {
        return None;
    }
    st.pending_delta = [0.0, 0.0];
    Some([dx, dy])
}

/// Configure the periodic owning-thread UI callback and wake signal.
pub fn configure_ui_tick(callback: UiTickCallback) {
    *callback_slot().lock() = Some(callback);
}

/// Locate the primary visible top-level window belonging to the current process,
/// excluding any attached console window (`GetConsoleWindow` / `ConsoleWindowClass`).
pub fn find_primary_game_window() -> Option<usize> {
    struct Search {
        pid: u32,
        console_hwnd: usize,
        best_hwnd: usize,
        best_area: i64,
    }

    unsafe extern "system" fn enum_cb(hwnd: Hwnd, lparam: isize) -> i32 {
        // SAFETY: Stack pointer `&mut search` valid during synchronous `EnumWindows`.
        let search = unsafe { &mut *(lparam as *mut Search) };
        if (hwnd as usize) == search.console_hwnd {
            return 1;
        }
        let mut pid: u32 = 0;
        // SAFETY: Valid `HWND` from `EnumWindows`.
        let tid = unsafe { GetWindowThreadProcessId(hwnd, &mut pid) };
        if tid == 0 || pid != search.pid {
            return 1;
        }
        // SAFETY: Check visibility & area.
        if unsafe { IsWindowVisible(hwnd) } == 0 {
            return 1;
        }
        let mut class_buf = [0u16; 64];
        // SAFETY: Valid `HWND` and stack UTF-16 buffer.
        let class_len =
            unsafe { GetClassNameW(hwnd, class_buf.as_mut_ptr(), class_buf.len() as i32) };
        if class_len > 0 {
            let class_name = String::from_utf16_lossy(&class_buf[..class_len as usize]);
            if matches!(
                class_name.as_str(),
                "ConsoleWindowClass" | "PseudoConsoleWindow" | "CASCADIA_HOSTING_WINDOW_CLASS"
            ) {
                return 1;
            }
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

    // SAFETY: Current PID and console window handle lookup.
    let pid = unsafe { GetCurrentProcessId() };
    // SAFETY: Queries any console `HWND` attached to the current process.
    let console_hwnd = unsafe { GetConsoleWindow() } as usize;
    let mut search = Search {
        pid,
        console_hwnd,
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
    LAST_TICK_MS.store(0, Ordering::Release);
    MODAL_OVERLAY_ACTIVE.store(false, Ordering::Release);
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
        let prev = set_wnd_long(hwnd, GWLP_WNDPROC, (router_ptr as usize) as isize);
        if prev == 0 && GetLastError() != 0 {
            let _ = RemovePropW(hwnd, prop_name().as_ptr());
            return false;
        }
        ORIGINAL_WNDPROC.store(prev, Ordering::Release);
        CURRENT_TID.store(tid, Ordering::Release);
        CURRENT_HWND.store(hwnd_val, Ordering::Release);
        LAST_TICK_MS.store(crate::win32_console::unix_time_ms(), Ordering::Release);
        let _ = SetTimer(hwnd, SIMODER_UI_TIMER_ID, UI_SYNC_INTERVAL_MS, null_mut());
        let _ = PostMessageW(hwnd, WM_APP_SIMODER_SYNC, 0, 0);
        true
    }
}

/// Verify that the bound window still exists, retains our `router_wndproc` subclass, and
/// continues receiving periodic timer ticks; self-heal if the game replaced `GWLP_WNDPROC`
/// or killed `SIMODER_UI_TIMER_ID` across a phase/window transition.
pub fn ensure_window_dispatch_healthy(hwnd_val: usize) -> Option<&'static str> {
    if hwnd_val == 0 || DISPATCH_CLOSED.load(Ordering::Acquire) {
        return None;
    }
    let hwnd = hwnd_val as Hwnd;
    // SAFETY: Checks window validity on the bound `HWND`.
    if unsafe { IsWindow(hwnd) } == 0 {
        detach_bound_window(hwnd_val);
        return Some("window destroyed");
    }
    let now_ms = crate::win32_console::unix_time_ms();
    let last_ms = LAST_TICK_MS.load(Ordering::Acquire);
    if now_ms.saturating_sub(last_ms) < 1500 {
        return None;
    }
    // SAFETY: Queries current `GWLP_WNDPROC` on a verified live `HWND`.
    let current_proc = unsafe { get_wnd_long(hwnd, GWLP_WNDPROC) };
    let router_ptr: WndProcFn = router_wndproc;
    if current_proc != ((router_ptr as usize) as isize) {
        CURRENT_HWND.store(0, Ordering::Release);
        CURRENT_TID.store(0, Ordering::Release);
        ORIGINAL_WNDPROC.store(0, Ordering::Release);
        if bind_game_window(hwnd_val) {
            return Some("WNDPROC replaced");
        }
        return None;
    }
    // SAFETY: Re-arms `SIMODER_UI_TIMER_ID` and posts an immediate wake on the verified `HWND`.
    unsafe {
        let _ = SetTimer(hwnd, SIMODER_UI_TIMER_ID, UI_SYNC_INTERVAL_MS, null_mut());
        let _ = PostMessageW(hwnd, WM_APP_SIMODER_SYNC, 0, 0);
    }
    LAST_TICK_MS.store(now_ms, Ordering::Release);
    Some("timer re-armed")
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
            if get_wnd_long(hwnd, GWLP_WNDPROC) == ((router_ptr as usize) as isize) && orig != 0 {
                let _ = set_wnd_long(hwnd, GWLP_WNDPROC, orig);
            }
        }
    }
}

/// Return true when no `router_wndproc` frame is currently executing.
pub fn is_window_dispatch_drained() -> bool {
    ACTIVE_CALLS.load(Ordering::Acquire) == 0
}
