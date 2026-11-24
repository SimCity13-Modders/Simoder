#![allow(unsafe_code)]

//! In-memory `EA::IO::IStream` COM-like vtable implementation for `DatabasePackedFile`.
//!
//! Provides a read-only, reference-counted stream backed by an owned `Vec<u8>` so
//! `DatabasePackedFile::SetStream` and `Open` can mount DBPF v3 overlays purely from RAM.

use std::ffi::c_void;
use std::ptr::copy_nonoverlapping;
use std::sync::atomic::{AtomicI32, Ordering};

use parking_lot::Mutex;

const ISTREAM_TYPE_MEMORY: u32 = 0x534D_4D53; // "SMMS"
const ACCESS_FLAG_READ: i32 = 1;
const STATE_SUCCESS: i32 = 0;

#[repr(C)]
struct IStreamVtable {
    dtor: unsafe extern "thiscall" fn(*mut InMemoryIStream, u32) -> *mut c_void,
    add_ref: unsafe extern "thiscall" fn(*mut InMemoryIStream) -> i32,
    release: unsafe extern "thiscall" fn(*mut InMemoryIStream) -> i32,
    get_type: unsafe extern "thiscall" fn(*const InMemoryIStream) -> u32,
    get_access_flags: unsafe extern "thiscall" fn(*const InMemoryIStream) -> i32,
    get_state: unsafe extern "thiscall" fn(*const InMemoryIStream) -> i32,
    close: unsafe extern "thiscall" fn(*mut InMemoryIStream) -> u8,
    get_size: unsafe extern "thiscall" fn(*const InMemoryIStream) -> u32,
    set_size: unsafe extern "thiscall" fn(*mut InMemoryIStream, u32) -> u8,
    get_position: unsafe extern "thiscall" fn(*const InMemoryIStream, i32) -> i32,
    set_position: unsafe extern "thiscall" fn(*mut InMemoryIStream, i32, i32) -> u8,
    get_available: unsafe extern "thiscall" fn(*const InMemoryIStream) -> u32,
    read: unsafe extern "thiscall" fn(*mut InMemoryIStream, *mut u8, u32) -> u32,
    flush: unsafe extern "thiscall" fn(*mut InMemoryIStream) -> u8,
    write: unsafe extern "thiscall" fn(*mut InMemoryIStream, *const u8, u32) -> u8,
}

static ISTREAM_VTABLE: IStreamVtable = IStreamVtable {
    dtor: istream_dtor,
    add_ref: istream_add_ref,
    release: istream_release,
    get_type: istream_get_type,
    get_access_flags: istream_get_access_flags,
    get_state: istream_get_state,
    close: istream_close,
    get_size: istream_get_size,
    set_size: istream_set_size,
    get_position: istream_get_position,
    set_position: istream_set_position,
    get_available: istream_get_available,
    read: istream_read,
    flush: istream_flush,
    write: istream_write,
};

#[repr(C)]
pub(super) struct InMemoryIStream {
    vtable: *const IStreamVtable,
    ref_count: AtomicI32,
    cursor: Mutex<usize>,
    data: Vec<u8>,
}

impl InMemoryIStream {
    pub(super) fn new_raw(data: Vec<u8>) -> *mut Self {
        Box::into_raw(Box::new(Self {
            vtable: &raw const ISTREAM_VTABLE,
            ref_count: AtomicI32::new(1),
            cursor: Mutex::new(0),
            data,
        }))
    }
}

unsafe extern "thiscall" fn istream_dtor(this: *mut InMemoryIStream, flags: u32) -> *mut c_void {
    if (flags & 1) != 0 && !this.is_null() {
        // SAFETY: Allocated via `Box::into_raw` in `InMemoryIStream::new_raw`.
        unsafe {
            drop(Box::from_raw(this));
        }
    }
    this.cast()
}

unsafe extern "thiscall" fn istream_add_ref(this: *mut InMemoryIStream) -> i32 {
    // SAFETY: Valid `InMemoryIStream` pointer owned by caller/refcount.
    let stream = unsafe { &*this };
    stream.ref_count.fetch_add(1, Ordering::AcqRel) + 1
}

pub(super) unsafe extern "thiscall" fn istream_release(this: *mut InMemoryIStream) -> i32 {
    // SAFETY: Valid `InMemoryIStream` pointer; drops only when refcount reaches 0.
    let prev = unsafe { (*this).ref_count.fetch_sub(1, Ordering::AcqRel) };
    let remaining = prev - 1;
    if remaining <= 0 {
        // SAFETY: Last reference dropped; reclaims `Box<InMemoryIStream>`.
        unsafe {
            drop(Box::from_raw(this));
        }
        return 0;
    }
    remaining
}

unsafe extern "thiscall" fn istream_get_type(_this: *const InMemoryIStream) -> u32 {
    ISTREAM_TYPE_MEMORY
}

unsafe extern "thiscall" fn istream_get_access_flags(_this: *const InMemoryIStream) -> i32 {
    ACCESS_FLAG_READ
}

unsafe extern "thiscall" fn istream_get_state(_this: *const InMemoryIStream) -> i32 {
    STATE_SUCCESS
}

unsafe extern "thiscall" fn istream_close(_this: *mut InMemoryIStream) -> u8 {
    1
}

unsafe extern "thiscall" fn istream_get_size(this: *const InMemoryIStream) -> u32 {
    // SAFETY: Valid `InMemoryIStream` pointer.
    unsafe { (*this).data.len() as u32 }
}

unsafe extern "thiscall" fn istream_set_size(_this: *mut InMemoryIStream, _size: u32) -> u8 {
    0
}

unsafe extern "thiscall" fn istream_get_position(
    this: *const InMemoryIStream,
    pos_type: i32,
) -> i32 {
    // SAFETY: Valid `InMemoryIStream` pointer.
    let stream = unsafe { &*this };
    let pos = *stream.cursor.lock();
    match pos_type {
        0 => pos as i32,
        1 => 0,
        2 => (pos as isize - stream.data.len() as isize) as i32,
        _ => -1,
    }
}

unsafe extern "thiscall" fn istream_set_position(
    this: *mut InMemoryIStream,
    distance: i32,
    pos_type: i32,
) -> u8 {
    // SAFETY: Valid `InMemoryIStream` pointer.
    let stream = unsafe { &*this };
    let len = stream.data.len() as isize;
    let mut guard = stream.cursor.lock();
    let target = match pos_type {
        0 => distance as isize,
        1 => (*guard as isize).saturating_add(distance as isize),
        2 => len.saturating_add(distance as isize),
        _ => return 0,
    };
    if target < 0 || target > len {
        return 0;
    }
    *guard = target as usize;
    1
}

unsafe extern "thiscall" fn istream_get_available(this: *const InMemoryIStream) -> u32 {
    // SAFETY: Valid `InMemoryIStream` pointer.
    let stream = unsafe { &*this };
    let pos = *stream.cursor.lock();
    stream.data.len().saturating_sub(pos) as u32
}

unsafe extern "thiscall" fn istream_read(
    this: *mut InMemoryIStream,
    out_ptr: *mut u8,
    n_size: u32,
) -> u32 {
    if out_ptr.is_null() || n_size == 0 {
        return 0;
    }
    // SAFETY: Valid `InMemoryIStream` pointer.
    let stream = unsafe { &*this };
    let mut guard = stream.cursor.lock();
    let avail = stream.data.len().saturating_sub(*guard);
    let to_copy = avail.min(n_size as usize);
    if to_copy > 0 {
        // SAFETY: `out_ptr` is caller-provided buffer of at least `n_size` bytes; non-overlapping.
        unsafe {
            copy_nonoverlapping(stream.data.as_ptr().add(*guard), out_ptr, to_copy);
        }
        *guard += to_copy;
    }
    to_copy as u32
}

unsafe extern "thiscall" fn istream_flush(_this: *mut InMemoryIStream) -> u8 {
    1
}

unsafe extern "thiscall" fn istream_write(
    _this: *mut InMemoryIStream,
    _in_ptr: *const u8,
    _n_size: u32,
) -> u8 {
    0
}
