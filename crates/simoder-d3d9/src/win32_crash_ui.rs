#![allow(unsafe_code)]

//! Native Win32 modal crash report window (`CRASH-001..004`) displaying a redacted crash report
//! with buttons to copy the shareable anonymous report, open the crash/log folders, or close.

use std::ffi::c_void;
use std::path::{Path, PathBuf};
use std::ptr::{null, null_mut};
use std::sync::OnceLock;

use parking_lot::Mutex;
use simoder_crash::validate_shareable_redaction;

use crate::win32_loader::to_wide_null;
use crate::win32_native::open_url_in_browser;
use crate::win32_semantic_abi::copy_text_to_clipboard;

type HWnd = *mut c_void;
type HInstance = *mut c_void;
type HFont = *mut c_void;
type HMenu = *mut c_void;
type HIcon = *mut c_void;
type HCursor = *mut c_void;
type HBrush = *mut c_void;

const WS_OVERLAPPEDWINDOW: u32 = 0x00CF_0000;
const WS_VISIBLE: u32 = 0x1000_0000;
const WS_CHILD: u32 = 0x4000_0000;
const WS_TABSTOP: u32 = 0x0001_0000;
const WS_VSCROLL: u32 = 0x0020_0000;
const WS_HSCROLL: u32 = 0x0010_0000;
const WS_BORDER: u32 = 0x0080_0000;
const ES_MULTILINE: u32 = 0x0004;
const ES_AUTOVSCROLL: u32 = 0x0040;
const ES_READONLY: u32 = 0x0800;
const BS_PUSHBUTTON: u32 = 0x0000;
const BS_DEFPUSHBUTTON: u32 = 0x0001;

const WM_CREATE: u32 = 0x0001;
const WM_DESTROY: u32 = 0x0002;
const WM_SETFONT: u32 = 0x0030;
const WM_COMMAND: u32 = 0x0111;
const COLOR_WINDOW: usize = 5;
const IDC_ARROW: usize = 32512;
const MB_OK: u32 = 0x0000_0000;
const MB_ICONERROR: u32 = 0x0000_0010;
const SW_SHOW: i32 = 5;
const SM_CXSCREEN: i32 = 0;
const SM_CYSCREEN: i32 = 1;

const ID_COPY: usize = 101;
const ID_OPEN_CRASH: usize = 102;
const ID_OPEN_LOGS: usize = 103;
const ID_CLOSE: usize = 104;

#[repr(C)]
struct WndClassExW {
    cb_size: u32,
    style: u32,
    lpfn_wnd_proc: unsafe extern "system" fn(HWnd, u32, usize, isize) -> isize,
    cb_cls_extra: i32,
    cb_wnd_extra: i32,
    h_instance: HInstance,
    h_icon: HIcon,
    h_cursor: HCursor,
    hbr_background: HBrush,
    lpsz_menu_name: *const u16,
    lpsz_class_name: *const u16,
    h_icon_sm: HIcon,
}

#[repr(C)]
#[derive(Default)]
struct Point {
    x: i32,
    y: i32,
}

#[repr(C)]
#[derive(Default)]
struct Msg {
    hwnd: HWnd,
    message: u32,
    w_param: usize,
    l_param: isize,
    time: u32,
    pt: Point,
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetModuleHandleW(lp_module_name: *const u16) -> HInstance;
}

#[link(name = "user32")]
unsafe extern "system" {
    fn RegisterClassExW(lpwcx: *const WndClassExW) -> u16;
    fn CreateWindowExW(
        dw_ex_style: u32,
        lp_class_name: *const u16,
        lp_window_name: *const u16,
        dw_style: u32,
        x: i32,
        y: i32,
        n_width: i32,
        n_height: i32,
        h_wnd_parent: HWnd,
        h_menu: HMenu,
        h_instance: HInstance,
        lp_param: *mut c_void,
    ) -> HWnd;
    fn DestroyWindow(h_wnd: HWnd) -> i32;
    fn ShowWindow(h_wnd: HWnd, n_cmd_show: i32) -> i32;
    fn UpdateWindow(h_wnd: HWnd) -> i32;
    fn DefWindowProcW(h_wnd: HWnd, msg: u32, w_param: usize, l_param: isize) -> isize;
    fn PostQuitMessage(n_exit_code: i32);
    fn GetMessageW(lp_msg: *mut Msg, h_wnd: HWnd, w_min: u32, w_max: u32) -> i32;
    fn TranslateMessage(lp_msg: *const Msg) -> i32;
    fn DispatchMessageW(lp_msg: *const Msg) -> isize;
    fn SendMessageW(h_wnd: HWnd, msg: u32, w_param: usize, l_param: isize) -> isize;
    fn SetWindowTextW(h_wnd: HWnd, lp_string: *const u16) -> i32;
    fn GetDlgItem(h_dlg: HWnd, n_id_dlg_item: i32) -> HWnd;
    fn LoadCursorW(h_instance: HInstance, lp_cursor_name: *const u16) -> HCursor;
    fn GetSystemMetrics(n_index: i32) -> i32;
    fn MessageBoxW(h_wnd: HWnd, lp_text: *const u16, lp_caption: *const u16, u_type: u32) -> i32;
}

#[link(name = "gdi32")]
unsafe extern "system" {
    fn CreateFontW(
        c_height: i32,
        c_width: i32,
        c_escapement: i32,
        c_orientation: i32,
        c_weight: i32,
        b_italic: u32,
        b_underline: u32,
        b_strike_out: u32,
        i_char_set: u32,
        i_out_precision: u32,
        i_clip_precision: u32,
        i_quality: u32,
        i_pitch_and_family: u32,
        psz_face_name: *const u16,
    ) -> HFont;
    fn DeleteObject(ho: *mut c_void) -> i32;
}

struct ActiveCrashWindowState {
    shareable_text: String,
    crash_dir: PathBuf,
    log_dir: PathBuf,
    header_text: String,
    mono_font: usize,
    ui_font: usize,
}

static ACTIVE_WINDOW_STATE: OnceLock<Mutex<Option<ActiveCrashWindowState>>> = OnceLock::new();

fn window_state_slot() -> &'static Mutex<Option<ActiveCrashWindowState>> {
    ACTIVE_WINDOW_STATE.get_or_init(|| Mutex::new(None))
}

unsafe extern "system" fn crash_window_proc(
    hwnd: HWnd,
    msg: u32,
    w_param: usize,
    l_param: isize,
) -> isize {
    match msg {
        WM_CREATE => {
            // SAFETY: Queries module instance for child control creation on the owning thread.
            let h_inst = unsafe { GetModuleHandleW(null()) };
            let (header_text, body_crlf, mono_font, ui_font) = {
                let mut guard = window_state_slot().lock();
                if let Some(st) = guard.as_mut() {
                    let mono_name = to_wide_null(std::ffi::OsStr::new("Consolas"));
                    let ui_name = to_wide_null(std::ffi::OsStr::new("Segoe UI"));
                    // SAFETY: Creates GDI fonts owned until `WM_DESTROY`.
                    let mono = unsafe {
                        CreateFontW(
                            -13,
                            0,
                            0,
                            0,
                            400,
                            0,
                            0,
                            0,
                            1,
                            0,
                            0,
                            0,
                            0,
                            mono_name.as_ptr(),
                        )
                    };
                    // SAFETY: Creates GDI UI font owned until `WM_DESTROY`.
                    let ui = unsafe {
                        CreateFontW(
                            -14,
                            0,
                            0,
                            0,
                            400,
                            0,
                            0,
                            0,
                            1,
                            0,
                            0,
                            0,
                            0,
                            ui_name.as_ptr(),
                        )
                    };
                    st.mono_font = mono as usize;
                    st.ui_font = ui as usize;
                    (
                        st.header_text.clone(),
                        st.shareable_text.replace('\n', "\r\n"),
                        mono,
                        ui,
                    )
                } else {
                    (String::new(), String::new(), null_mut(), null_mut())
                }
            };

            let static_cls = to_wide_null(std::ffi::OsStr::new("STATIC"));
            let edit_cls = to_wide_null(std::ffi::OsStr::new("EDIT"));
            let btn_cls = to_wide_null(std::ffi::OsStr::new("BUTTON"));
            let hdr_w = to_wide_null(std::ffi::OsStr::new(&header_text));
            let body_w = to_wide_null(std::ffi::OsStr::new(&body_crlf));

            // SAFETY: Valid parent `hwnd` and child control parameters.
            unsafe {
                let lbl = CreateWindowExW(
                    0,
                    static_cls.as_ptr(),
                    hdr_w.as_ptr(),
                    WS_CHILD | WS_VISIBLE,
                    14,
                    12,
                    716,
                    40,
                    hwnd,
                    null_mut(),
                    h_inst,
                    null_mut(),
                );
                if !lbl.is_null() && !ui_font.is_null() {
                    SendMessageW(lbl, WM_SETFONT, ui_font as usize, 1);
                }

                let edit = CreateWindowExW(
                    0,
                    edit_cls.as_ptr(),
                    body_w.as_ptr(),
                    WS_CHILD
                        | WS_VISIBLE
                        | WS_BORDER
                        | WS_VSCROLL
                        | WS_HSCROLL
                        | ES_MULTILINE
                        | ES_AUTOVSCROLL
                        | ES_READONLY,
                    14,
                    58,
                    716,
                    386,
                    hwnd,
                    null_mut(),
                    h_inst,
                    null_mut(),
                );
                if !edit.is_null() && !mono_font.is_null() {
                    SendMessageW(edit, WM_SETFONT, mono_font as usize, 1);
                }

                let specs = [
                    (ID_COPY, "Copy Anonymous Report", 14, 190, BS_DEFPUSHBUTTON),
                    (ID_OPEN_CRASH, "Open Crash Folder", 214, 150, BS_PUSHBUTTON),
                    (ID_OPEN_LOGS, "Open Logs Folder", 374, 150, BS_PUSHBUTTON),
                    (ID_CLOSE, "Close", 620, 110, BS_PUSHBUTTON),
                ];
                for (id, label, x, w, style) in specs {
                    let lbl_w = to_wide_null(std::ffi::OsStr::new(label));
                    let btn = CreateWindowExW(
                        0,
                        btn_cls.as_ptr(),
                        lbl_w.as_ptr(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                        x,
                        458,
                        w,
                        32,
                        hwnd,
                        id as HMenu,
                        h_inst,
                        null_mut(),
                    );
                    if !btn.is_null() && !ui_font.is_null() {
                        SendMessageW(btn, WM_SETFONT, ui_font as usize, 1);
                    }
                }
            }
            0
        }
        WM_COMMAND => {
            let cmd_id = w_param & 0xFFFF;
            match cmd_id {
                ID_COPY => {
                    let text_opt = window_state_slot()
                        .lock()
                        .as_ref()
                        .map(|s| s.shareable_text.clone());
                    if let Some(text) = text_opt
                        && validate_shareable_redaction(&text).is_ok()
                        && copy_text_to_clipboard(&text).is_ok()
                    {
                        // SAFETY: Updates button caption on owning window thread.
                        unsafe {
                            let btn = GetDlgItem(hwnd, ID_COPY as i32);
                            if !btn.is_null() {
                                let copied = to_wide_null(std::ffi::OsStr::new("Copied!"));
                                SetWindowTextW(btn, copied.as_ptr());
                            }
                        }
                    }
                }
                ID_OPEN_CRASH => {
                    if let Some(dir) = window_state_slot()
                        .lock()
                        .as_ref()
                        .map(|s| s.crash_dir.clone())
                    {
                        open_url_in_browser(&dir.display().to_string());
                    }
                }
                ID_OPEN_LOGS => {
                    if let Some(dir) = window_state_slot()
                        .lock()
                        .as_ref()
                        .map(|s| s.log_dir.clone())
                    {
                        open_url_in_browser(&dir.display().to_string());
                    }
                }
                ID_CLOSE => {
                    // SAFETY: Destroys our own crash report window on user click.
                    unsafe {
                        let _ = DestroyWindow(hwnd);
                    }
                }
                _ => {}
            }
            0
        }
        WM_DESTROY => {
            if let Some(st) = window_state_slot().lock().take() {
                // SAFETY: Deletes GDI font handles created in `WM_CREATE`.
                unsafe {
                    if st.mono_font != 0 {
                        let _ = DeleteObject(st.mono_font as *mut c_void);
                    }
                    if st.ui_font != 0 {
                        let _ = DeleteObject(st.ui_font as *mut c_void);
                    }
                }
            }
            // SAFETY: Ends the local modal crash window message loop.
            unsafe {
                PostQuitMessage(0);
            }
            0
        }
        // SAFETY: Standard Win32 default window procedure forwarding.
        _ => unsafe { DefWindowProcW(hwnd, msg, w_param, l_param) },
    }
}

/// Display a native Win32 crash report window with copy-to-clipboard (re-validated for redaction)
/// and folder actions, falling back to `MessageBoxW` if window creation fails.
pub(crate) fn show_crash_report_window(
    title: &str,
    shareable_text: &str,
    saved_path: Option<&Path>,
    crash_dir: &Path,
    log_dir: &Path,
) {
    let header_text = match saved_path {
        Some(p) => format!(
            "SimCity encountered a fatal native exception. Anonymous report saved to:\n{}",
            p.display()
        ),
        None => "SimCity encountered a fatal native exception. Review or copy the anonymous report below.".to_string(),
    };

    *window_state_slot().lock() = Some(ActiveCrashWindowState {
        shareable_text: shareable_text.to_string(),
        crash_dir: crash_dir.to_path_buf(),
        log_dir: log_dir.to_path_buf(),
        header_text: header_text.clone(),
        mono_font: 0,
        ui_font: 0,
    });

    let class_name = to_wide_null(std::ffi::OsStr::new("SimoderCrashReportWindow"));
    let title_w = to_wide_null(std::ffi::OsStr::new(title));
    // SAFETY: Registers window class, creates centered crash report window, and runs its message loop.
    unsafe {
        let h_inst = GetModuleHandleW(null());
        let wnd_cls = WndClassExW {
            cb_size: std::mem::size_of::<WndClassExW>() as u32,
            style: 0,
            lpfn_wnd_proc: crash_window_proc,
            cb_cls_extra: 0,
            cb_wnd_extra: 0,
            h_instance: h_inst,
            h_icon: null_mut(),
            h_cursor: LoadCursorW(null_mut(), IDC_ARROW as *const u16),
            hbr_background: (COLOR_WINDOW + 1) as HBrush,
            lpsz_menu_name: null(),
            lpsz_class_name: class_name.as_ptr(),
            h_icon_sm: null_mut(),
        };
        let _ = RegisterClassExW(&wnd_cls);

        let win_w = 760;
        let win_h = 540;
        let scr_w = GetSystemMetrics(SM_CXSCREEN).max(win_w);
        let scr_h = GetSystemMetrics(SM_CYSCREEN).max(win_h);
        let x = (scr_w - win_w) / 2;
        let y = (scr_h - win_h) / 2;

        let hwnd = CreateWindowExW(
            0,
            class_name.as_ptr(),
            title_w.as_ptr(),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            x,
            y,
            win_w,
            win_h,
            null_mut(),
            null_mut(),
            h_inst,
            null_mut(),
        );
        if hwnd.is_null() {
            let fallback_body = format!("{header_text}\n\n{}", &shareable_text[..shareable_text.len().min(1200)]);
            let body_w = to_wide_null(std::ffi::OsStr::new(&fallback_body));
            let _ = MessageBoxW(null_mut(), body_w.as_ptr(), title_w.as_ptr(), MB_OK | MB_ICONERROR);
            *window_state_slot().lock() = None;
            return;
        }

        let _ = ShowWindow(hwnd, SW_SHOW);
        let _ = UpdateWindow(hwnd);

        let mut msg = Msg::default();
        while GetMessageW(&mut msg, null_mut(), 0, 0) > 0 {
            let _ = TranslateMessage(&msg);
            let _ = DispatchMessageW(&msg);
        }
    }
}
