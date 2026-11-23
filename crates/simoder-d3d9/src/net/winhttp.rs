#![allow(unsafe_code)]

//! Native Windows WinHTTP HTTP/HTTPS client for x86 and x64 without OpenSSL dependencies.

use std::ffi::c_void;
use std::fs::File;
use std::io::Write;
use std::path::Path;
use std::ptr::{null, null_mut};

type HInternet = *mut c_void;

const WINHTTP_ACCESS_TYPE_DEFAULT_PROXY: u32 = 0;
const WINHTTP_FLAG_SECURE: u32 = 0x00800000;
const WINHTTP_FLAG_REFRESH: u32 = 0x00000100;
const WINHTTP_QUERY_STATUS_CODE: u32 = 19;
const WINHTTP_QUERY_FLAG_NUMBER: u32 = 0x20000000;
const INTERNET_DEFAULT_HTTP_PORT: u16 = 80;
const INTERNET_DEFAULT_HTTPS_PORT: u16 = 443;

#[link(name = "winhttp")]
unsafe extern "system" {
    fn WinHttpOpen(
        pszAgentW: *const u16,
        dwAccessType: u32,
        pszProxyW: *const u16,
        pszProxyBypassW: *const u16,
        dwFlags: u32,
    ) -> HInternet;

    fn WinHttpConnect(
        hSession: HInternet,
        pswzServerName: *const u16,
        nServerPort: u16,
        dwReserved: u32,
    ) -> HInternet;

    fn WinHttpOpenRequest(
        hConnect: HInternet,
        pwszVerb: *const u16,
        pwszObjectName: *const u16,
        pwszVersion: *const u16,
        pwszReferrer: *const u16,
        ppwszAcceptTypes: *const *const u16,
        dwFlags: u32,
    ) -> HInternet;

    fn WinHttpSendRequest(
        hRequest: HInternet,
        lpszHeaders: *const u16,
        dwHeadersLength: u32,
        lpOptional: *mut c_void,
        dwOptionalLength: u32,
        dwTotalLength: u32,
        dwContext: usize,
    ) -> i32;

    fn WinHttpReceiveResponse(hRequest: HInternet, lpReserved: *mut c_void) -> i32;

    fn WinHttpQueryHeaders(
        hRequest: HInternet,
        dwInfoLevel: u32,
        pwszName: *const u16,
        lpBuffer: *mut c_void,
        lpdwBufferLength: *mut u32,
        lpdwIndex: *mut u32,
    ) -> i32;

    fn WinHttpQueryDataAvailable(hRequest: HInternet, lpdwNumberOfBytesAvailable: *mut u32) -> i32;

    fn WinHttpReadData(
        hRequest: HInternet,
        lpBuffer: *mut c_void,
        dwNumberOfBytesToRead: u32,
        lpdwNumberOfBytesRead: *mut u32,
    ) -> i32;

    fn WinHttpCloseHandle(hInternet: HInternet) -> i32;
}

struct WinHttpHandle(HInternet);

impl Drop for WinHttpHandle {
    fn drop(&mut self) {
        if !self.0.is_null() {
            // SAFETY: Valid WinHTTP handle owned by this wrapper.
            unsafe {
                let _ = WinHttpCloseHandle(self.0);
            }
            self.0 = null_mut();
        }
    }
}

fn to_wide_null(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

struct ParsedUrl {
    is_https: bool,
    host: String,
    port: u16,
    path_and_query: String,
}

fn parse_http_url(url: &str) -> Result<ParsedUrl, String> {
    let (is_https, rest) = if let Some(stripped) = url.strip_prefix("https://") {
        (true, stripped)
    } else if let Some(stripped) = url.strip_prefix("http://") {
        (false, stripped)
    } else {
        return Err(format!("Unsupported or missing protocol in URL: '{url}'"));
    };

    let (host_port, path) = match rest.find('/') {
        Some(idx) => (&rest[..idx], &rest[idx..]),
        None => (rest, "/"),
    };

    let (host, port) = match host_port.find(':') {
        Some(idx) => {
            let host_str = &host_port[..idx];
            let port_num = host_port[idx + 1..]
                .parse::<u16>()
                .map_err(|e| format!("Invalid port in URL '{url}': {e}"))?;
            (host_str.to_string(), port_num)
        }
        None => {
            let def_port = if is_https {
                INTERNET_DEFAULT_HTTPS_PORT
            } else {
                INTERNET_DEFAULT_HTTP_PORT
            };
            (host_port.to_string(), def_port)
        }
    };

    if host.is_empty() {
        return Err(format!("Empty host in URL: '{url}'"));
    }

    Ok(ParsedUrl {
        is_https,
        host,
        port,
        path_and_query: path.to_string(),
    })
}

/// Perform a GET request to `url` with JSON accept headers and return the response body as a string.
pub fn get_json(url: &str) -> Result<String, String> {
    let mut current_url = url.to_string();
    let mut redirect_count = 0;

    loop {
        if redirect_count > 8 {
            return Err("Too many HTTP redirects".into());
        }

        let parsed = parse_http_url(&current_url)?;
        let user_agent = to_wide_null("Simoder-Client/0.2.0 (Windows)");
        let host_wide = to_wide_null(&parsed.host);
        let path_wide = to_wide_null(&parsed.path_and_query);
        let verb_wide = to_wide_null("GET");
        let headers_wide = to_wide_null("Accept: application/vnd.github.v3+json, application/json\r\nUser-Agent: Simoder-Client/0.2.0\r\n");

        // SAFETY: Calling WinHTTP APIs with checked UTF-16 pointers and RAII handle guards.
        unsafe {
            let session = WinHttpOpen(
                user_agent.as_ptr(),
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                null(),
                null(),
                0,
            );
            if session.is_null() {
                return Err("Failed WinHttpOpen".into());
            }
            let _session_guard = WinHttpHandle(session);

            let connect = WinHttpConnect(session, host_wide.as_ptr(), parsed.port, 0);
            if connect.is_null() {
                return Err(format!("Failed WinHttpConnect to '{}'", parsed.host));
            }
            let _connect_guard = WinHttpHandle(connect);

            let mut flags = WINHTTP_FLAG_REFRESH;
            if parsed.is_https {
                flags |= WINHTTP_FLAG_SECURE;
            }

            let request = WinHttpOpenRequest(
                connect,
                verb_wide.as_ptr(),
                path_wide.as_ptr(),
                null(),
                null(),
                null(),
                flags,
            );
            if request.is_null() {
                return Err("Failed WinHttpOpenRequest".into());
            }
            let _request_guard = WinHttpHandle(request);

            let ok = WinHttpSendRequest(
                request,
                headers_wide.as_ptr(),
                headers_wide.len() as u32 - 1,
                null_mut(),
                0,
                0,
                0,
            );
            if ok == 0 {
                return Err("Failed WinHttpSendRequest".into());
            }

            if WinHttpReceiveResponse(request, null_mut()) == 0 {
                return Err("Failed WinHttpReceiveResponse".into());
            }

            let mut status_code: u32 = 0;
            let mut status_size = std::mem::size_of::<u32>() as u32;
            let ok_stat = WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                null(),
                &mut status_code as *mut u32 as *mut c_void,
                &mut status_size,
                null_mut(),
            );
            if ok_stat == 0 {
                return Err("Failed querying HTTP status code".into());
            }

            if status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308 {
                let mut loc_len: u32 = 512;
                let mut loc_buf = vec![0u16; loc_len as usize];
                let ok_loc = WinHttpQueryHeaders(
                    request,
                    33, // WINHTTP_QUERY_LOCATION
                    null(),
                    loc_buf.as_mut_ptr() as *mut c_void,
                    &mut loc_len,
                    null_mut(),
                );
                if ok_loc != 0 {
                    let loc_str = String::from_utf16_lossy(&loc_buf[..(loc_len as usize / 2)]);
                    let trimmed = loc_str.trim_matches('\0').trim().to_string();
                    if !trimmed.is_empty() {
                        current_url = trimmed;
                        redirect_count += 1;
                        continue;
                    }
                }
            }

            if !(200..300).contains(&status_code) {
                return Err(format!("HTTP request failed with status {status_code}"));
            }

            let mut body_bytes = Vec::new();
            loop {
                let mut avail: u32 = 0;
                if WinHttpQueryDataAvailable(request, &mut avail) == 0 {
                    return Err("Failed WinHttpQueryDataAvailable".into());
                }
                if avail == 0 {
                    break;
                }
                let mut chunk = vec![0u8; avail as usize];
                let mut read: u32 = 0;
                if WinHttpReadData(
                    request,
                    chunk.as_mut_ptr() as *mut c_void,
                    avail,
                    &mut read,
                ) == 0
                {
                    return Err("Failed WinHttpReadData".into());
                }
                chunk.truncate(read as usize);
                body_bytes.extend_from_slice(&chunk);
            }

            return String::from_utf8(body_bytes).map_err(|e| format!("Invalid UTF-8 response: {e}"));
        }
    }
}

/// Download a file from `url` directly into `dest_path`, following HTTP redirects.
pub fn download_file<F>(url: &str, dest_path: &Path, mut on_progress: F) -> Result<(), String>
where
    F: FnMut(u64, Option<u64>),
{
    let mut current_url = url.to_string();
    let mut redirect_count = 0;

    loop {
        if redirect_count > 10 {
            return Err("Too many HTTP redirects while downloading file".into());
        }

        let parsed = parse_http_url(&current_url)?;
        let user_agent = to_wide_null("Simoder-Client/0.2.0 (Windows)");
        let host_wide = to_wide_null(&parsed.host);
        let path_wide = to_wide_null(&parsed.path_and_query);
        let verb_wide = to_wide_null("GET");
        let headers_wide = to_wide_null("Accept: application/octet-stream, */*\r\nUser-Agent: Simoder-Client/0.2.0\r\n");

        // SAFETY: Calling WinHTTP APIs with checked UTF-16 pointers and RAII handle guards.
        unsafe {
            let session = WinHttpOpen(
                user_agent.as_ptr(),
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                null(),
                null(),
                0,
            );
            if session.is_null() {
                return Err("Failed WinHttpOpen".into());
            }
            let _session_guard = WinHttpHandle(session);

            let connect = WinHttpConnect(session, host_wide.as_ptr(), parsed.port, 0);
            if connect.is_null() {
                return Err(format!("Failed WinHttpConnect to '{}'", parsed.host));
            }
            let _connect_guard = WinHttpHandle(connect);

            let mut flags = WINHTTP_FLAG_REFRESH;
            if parsed.is_https {
                flags |= WINHTTP_FLAG_SECURE;
            }

            let request = WinHttpOpenRequest(
                connect,
                verb_wide.as_ptr(),
                path_wide.as_ptr(),
                null(),
                null(),
                null(),
                flags,
            );
            if request.is_null() {
                return Err("Failed WinHttpOpenRequest".into());
            }
            let _request_guard = WinHttpHandle(request);

            let ok = WinHttpSendRequest(
                request,
                headers_wide.as_ptr(),
                headers_wide.len() as u32 - 1,
                null_mut(),
                0,
                0,
                0,
            );
            if ok == 0 {
                return Err("Failed WinHttpSendRequest".into());
            }

            if WinHttpReceiveResponse(request, null_mut()) == 0 {
                return Err("Failed WinHttpReceiveResponse".into());
            }

            let mut status_code: u32 = 0;
            let mut status_size = std::mem::size_of::<u32>() as u32;
            let ok_stat = WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                null(),
                &mut status_code as *mut u32 as *mut c_void,
                &mut status_size,
                null_mut(),
            );
            if ok_stat == 0 {
                return Err("Failed querying HTTP status code".into());
            }

            if status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308 {
                let mut loc_len: u32 = 1024;
                let mut loc_buf = vec![0u16; loc_len as usize];
                let ok_loc = WinHttpQueryHeaders(
                    request,
                    33, // WINHTTP_QUERY_LOCATION
                    null(),
                    loc_buf.as_mut_ptr() as *mut c_void,
                    &mut loc_len,
                    null_mut(),
                );
                if ok_loc != 0 {
                    let loc_str = String::from_utf16_lossy(&loc_buf[..(loc_len as usize / 2)]);
                    let trimmed = loc_str.trim_matches('\0').trim().to_string();
                    if !trimmed.is_empty() {
                        current_url = trimmed;
                        redirect_count += 1;
                        continue;
                    }
                }
            }

            if !(200..300).contains(&status_code) {
                return Err(format!("Download failed with HTTP status {status_code}"));
            }

            let mut content_len_val: u32 = 0;
            let mut content_len_size = std::mem::size_of::<u32>() as u32;
            let total_len = if WinHttpQueryHeaders(
                request,
                5 | WINHTTP_QUERY_FLAG_NUMBER, // WINHTTP_QUERY_CONTENT_LENGTH
                null(),
                &mut content_len_val as *mut u32 as *mut c_void,
                &mut content_len_size,
                null_mut(),
            ) != 0
            {
                Some(content_len_val as u64)
            } else {
                None
            };

            if let Some(parent) = dest_path.parent() {
                let _ = std::fs::create_dir_all(parent);
            }

            let mut out_file = File::create(dest_path)
                .map_err(|e| format!("Failed creating target file '{}': {e}", dest_path.display()))?;

            let mut downloaded: u64 = 0;
            on_progress(downloaded, total_len);

            loop {
                let mut avail: u32 = 0;
                if WinHttpQueryDataAvailable(request, &mut avail) == 0 {
                    return Err("Failed WinHttpQueryDataAvailable during download".into());
                }
                if avail == 0 {
                    break;
                }
                let mut chunk = vec![0u8; avail as usize];
                let mut read: u32 = 0;
                if WinHttpReadData(
                    request,
                    chunk.as_mut_ptr() as *mut c_void,
                    avail,
                    &mut read,
                ) == 0
                {
                    return Err("Failed WinHttpReadData during download".into());
                }
                chunk.truncate(read as usize);
                out_file
                    .write_all(&chunk)
                    .map_err(|e| format!("Failed writing chunk to '{}': {e}", dest_path.display()))?;
                downloaded += read as u64;
                on_progress(downloaded, total_len);
            }

            out_file
                .flush()
                .map_err(|e| format!("Failed flushing file '{}': {e}", dest_path.display()))?;

            return Ok(());
        }
    }
}
