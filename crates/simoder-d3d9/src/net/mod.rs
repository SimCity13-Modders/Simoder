//! Lightweight, safe WinHTTP client and ZIP extraction for Simoder AutoUpdate & bootstrap.

pub mod archive;
pub mod winhttp;

pub use archive::extract_zip_archive;
pub use winhttp::{download_file, get_json};
