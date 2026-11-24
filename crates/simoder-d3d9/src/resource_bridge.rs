#![allow(unsafe_code)]

//! In-memory `EA::IO::IStream` + `DatabasePackedFile` registration into `IResourceManager`
//! (`SimCity.exe 10.3.6.0`, `i686-pc-windows-msvc`).
//!
//! Serves validated Schema 3 `simoder.raw-resources` entries directly from RAM with zero `.package`
//! files written to disk.

use std::collections::BTreeMap;

use simoder_core::Tgi;
use simoder_formats::build_resource_overlay;
use simoder_mods::{ActiveRawResource, PACKAGE_INFO_GROUP_ID, PROP_TYPE_ID};

/// Assemble a deterministic DBPF v3 byte buffer in RAM from active raw resources.
///
/// Deduplicates by canonical uppercase `TYPE:GROUP:16-HEX-INSTANCE` ID in activation order
/// (last-enabled wins), rejects 64-bit instances exceeding `u32::MAX`, and rejects reserved
/// package-info metadata TGIs `(0x00B1B104, 0x40404000, *)`.
pub fn build_in_memory_dbpf_bytes(resources: &[ActiveRawResource]) -> Result<Vec<u8>, String> {
    let mut deduped: BTreeMap<String, &ActiveRawResource> = BTreeMap::new();
    for res in resources {
        deduped.insert(res.id.to_ascii_uppercase(), res);
    }

    let mut entries = Vec::with_capacity(deduped.len());
    for (canonical_id, res) in deduped {
        if canonical_id.len() != 34
            || canonical_id.as_bytes()[8] != b':'
            || canonical_id.as_bytes()[17] != b':'
        {
            return Err(format!("invalid raw resource ID '{canonical_id}'"));
        }
        let type_id = u32::from_str_radix(&canonical_id[0..8], 16)
            .map_err(|_| format!("invalid type ID in '{canonical_id}'"))?;
        let group = u32::from_str_radix(&canonical_id[9..17], 16)
            .map_err(|_| format!("invalid group ID in '{canonical_id}'"))?;
        let instance64 = u64::from_str_radix(&canonical_id[18..34], 16)
            .map_err(|_| format!("invalid instance ID in '{canonical_id}'"))?;
        let instance = u32::try_from(instance64).map_err(|_| {
            format!("raw resource '{canonical_id}' 64-bit instance exceeds 32-bit DBPF v3 index")
        })?;
        if type_id == PROP_TYPE_ID && group == PACKAGE_INFO_GROUP_ID {
            return Err(format!(
                "refusing package-info metadata TGI '{canonical_id}' in in-memory DBPF"
            ));
        }
        entries.push((
            Tgi {
                type_id,
                group,
                instance,
            },
            res.bytes.clone(),
        ));
    }

    build_resource_overlay(&entries).map_err(|e| format!("DBPF v3 overlay build failed: {e}"))
}

#[cfg(all(windows, target_pointer_width = "32", not(test)))]
mod native {
    use std::ffi::c_void;
    use std::mem::{size_of, transmute, zeroed};
    use std::ptr::{copy_nonoverlapping, null, null_mut};
    use std::sync::atomic::{AtomicI32, AtomicUsize, Ordering};

    use parking_lot::Mutex;
    use simoder_core::{Digest, Sha256};
    use simoder_mods::ActiveRawResource;

    use super::build_in_memory_dbpf_bytes;

    const PREFERRED_BASE: usize = 0x0040_0000;
    const VA_RESOURCE_MANAGER_GLOBAL: usize = 0x00E4_FF1C;
    const VA_GET_RESOURCE_MANAGER: usize = 0x0040_5BF0;
    const VA_REGISTER_DATABASE: usize = 0x0091_A2C0;
    const VA_GAME_ALLOC: usize = 0x0091_DD80;
    const VA_DBPF_CTOR: usize = 0x008E_FFC0;
    const VA_DBPF_OPEN: usize = 0x008E_EA10;
    const VA_DBPF_SET_STREAM: usize = 0x008E_E780;
    const DBPF_OBJECT_SIZE: u32 = 0x378;
    const SIMODER_DATABASE_PRIORITY: i32 = 1000;

    const MEM_COMMIT: u32 = 0x1000;
    const PAGE_NOACCESS: u32 = 0x01;
    const PAGE_GUARD: u32 = 0x100;

    const ISTREAM_TYPE_MEMORY: u32 = 0x534D_4D53; // "SMMS"
    const ACCESS_FLAG_READ: i32 = 1;
    const STATE_SUCCESS: i32 = 0;

    static VERIFIED_BASE: AtomicUsize = AtomicUsize::new(0);
    static MOUNTED_STATE: Mutex<Option<MountedDbState>> = Mutex::new(None);

    struct MountedDbState {
        db_ptr: usize,
        fingerprint: [u8; 32],
    }

    #[repr(C)]
    struct MemoryBasicInformation {
        base_address: usize,
        allocation_base: usize,
        allocation_protect: u32,
        region_size: usize,
        state: u32,
        protect: u32,
        type_: u32,
    }

    const _: () = assert!(size_of::<MemoryBasicInformation>() == 28);

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetModuleHandleW(lp_module_name: *const u16) -> *mut c_void;
        fn VirtualQuery(
            lp_address: *const c_void,
            lp_buffer: *mut MemoryBasicInformation,
            dw_length: usize,
        ) -> usize;
    }

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
    struct InMemoryIStream {
        vtable: *const IStreamVtable,
        ref_count: AtomicI32,
        cursor: Mutex<usize>,
        data: Vec<u8>,
    }

    impl InMemoryIStream {
        fn new_raw(data: Vec<u8>) -> *mut Self {
            Box::into_raw(Box::new(Self {
                vtable: &raw const ISTREAM_VTABLE,
                ref_count: AtomicI32::new(1),
                cursor: Mutex::new(0),
                data,
            }))
        }
    }

    unsafe extern "thiscall" fn istream_dtor(
        this: *mut InMemoryIStream,
        flags: u32,
    ) -> *mut c_void {
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

    unsafe extern "thiscall" fn istream_release(this: *mut InMemoryIStream) -> i32 {
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

    fn is_readable(addr: usize, len: usize) -> bool {
        if addr == 0 || len == 0 {
            return false;
        }
        let mut remaining = len;
        let mut cur = addr;
        while remaining > 0 {
            // SAFETY: `VirtualQuery` inspects committed virtual memory protections safely.
            let mut mbi: MemoryBasicInformation = unsafe { zeroed() };
            let queried = unsafe {
                VirtualQuery(
                    cur as *const c_void,
                    &mut mbi,
                    size_of::<MemoryBasicInformation>(),
                )
            };
            if queried == 0 || mbi.state != MEM_COMMIT {
                return false;
            }
            if (mbi.protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 {
                return false;
            }
            let offset_in_region = cur.saturating_sub(mbi.base_address);
            let avail = mbi.region_size.saturating_sub(offset_in_region);
            if avail == 0 {
                return false;
            }
            remaining = remaining.saturating_sub(avail);
            cur = cur.saturating_add(avail);
        }
        true
    }

    fn va(base: usize, target_va: usize) -> usize {
        base + target_va - PREFERRED_BASE
    }

    /// Verify retail `SimCity.exe 10.3.6.0` load base and all 6 resource-layer entry signatures.
    pub fn initialize_resource_bridge(verified_retail_file: bool) -> Result<(), String> {
        VERIFIED_BASE.store(0, Ordering::Release);
        if !verified_retail_file {
            return Err("Resource bridge requires verified SimCity.exe 10.3.6.0 SHA-256".into());
        }
        // SAFETY: Queries main module handle and checks 6 immutable entry opcode sequences.
        let base = unsafe { GetModuleHandleW(null()) } as usize;
        if base != PREFERRED_BASE {
            return Err(format!(
                "Resource bridge requires preferred load base 0x{PREFERRED_BASE:08X}, got 0x{base:08X}"
            ));
        }

        let checks: &[(usize, &[u8])] = &[
            (VA_GET_RESOURCE_MANAGER, &[0xA1, 0x1C, 0xFF, 0xE4, 0x00, 0xC3]),
            (VA_REGISTER_DATABASE, &[0x53, 0x56, 0x32, 0xDB, 0x57, 0x8B, 0xF9, 0x38]),
            (VA_GAME_ALLOC, &[0x56, 0xE8, 0xBA, 0x1C, 0xB4, 0xFF, 0x8B, 0x4C]),
            (VA_DBPF_CTOR, &[0x53, 0x56, 0x8B, 0xF1, 0xC7, 0x46, 0x04, 0xF0]),
            (VA_DBPF_OPEN, &[0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x14, 0x00, 0x74]),
            (VA_DBPF_SET_STREAM, &[0x53, 0x8B, 0xD9, 0x83, 0x7B, 0x14, 0x00, 0x75]),
        ];
        for &(target_va, expected) in checks {
            let ptr = va(base, target_va);
            if !is_readable(ptr, expected.len()) {
                return Err(format!("Resource entry signature at 0x{target_va:08X} unreadable"));
            }
            // SAFETY: Verified readable by `is_readable(ptr, expected.len())`.
            let actual = unsafe { std::slice::from_raw_parts(ptr as *const u8, expected.len()) };
            if actual != expected {
                return Err(format!("Resource entry signature mismatch at 0x{target_va:08X}"));
            }
        }
        VERIFIED_BASE.store(base, Ordering::Release);
        Ok(())
    }

    pub fn is_resource_bridge_ready() -> bool {
        VERIFIED_BASE.load(Ordering::Acquire) != 0
    }

    fn compute_resources_fingerprint(resources: &[ActiveRawResource]) -> [u8; 32] {
        let mut hasher = Sha256::new();
        hasher.update((resources.len() as u64).to_le_bytes());
        for r in resources {
            hasher.update((r.id.len() as u64).to_le_bytes());
            hasher.update(r.id.as_bytes());
            hasher.update(r.sha256.as_bytes());
        }
        hasher.finalize().into()
    }

    unsafe fn read_resource_manager(base: usize) -> Result<*mut c_void, String> {
        let global_ptr = va(base, VA_RESOURCE_MANAGER_GLOBAL);
        if !is_readable(global_ptr, size_of::<usize>()) {
            return Err("IResourceManager global slot unreadable".into());
        }
        // SAFETY: Verified readable pointer slot.
        let p_rm = unsafe { *(global_ptr as *const *mut c_void) };
        if p_rm.is_null() {
            return Err("IResourceManager singleton not initialized yet".into());
        }
        let rm_addr = p_rm as usize;
        if !is_readable(rm_addr, size_of::<usize>()) {
            return Err("IResourceManager instance unreadable".into());
        }
        // SAFETY: Reads primary vtable pointer from live `IResourceManager`.
        let vtable = unsafe { *(rm_addr as *const usize) };
        let slot_19_ptr = vtable + 0x4C;
        if !is_readable(slot_19_ptr, size_of::<usize>()) {
            return Err("IResourceManager vtable slot +0x4C unreadable".into());
        }
        // SAFETY: Verified readable vtable slot +0x4C (`RegisterDatabase`).
        let reg_fn = unsafe { *(slot_19_ptr as *const usize) };
        if reg_fn != va(base, VA_REGISTER_DATABASE) {
            return Err(format!(
                "IResourceManager RegisterDatabase mismatch: expected 0x{:08X}, got 0x{reg_fn:08X}",
                va(base, VA_REGISTER_DATABASE)
            ));
        }
        Ok(p_rm)
    }

    unsafe fn unregister_and_release_db(base: usize, p_rm: *mut c_void, db_ptr: usize) {
        if db_ptr == 0 || !is_readable(db_ptr, 8) {
            return;
        }
        type FnRegisterDatabase = unsafe extern "thiscall" fn(*mut c_void, u8, *mut c_void, i32) -> u8;
        type FnDbClose = unsafe extern "thiscall" fn(*mut c_void);
        type FnIObjectRelease = unsafe extern "thiscall" fn(*mut c_void) -> i32;

        // SAFETY: Verified `VA_REGISTER_DATABASE` signature and vtable slots of `DatabasePackedFile`.
        unsafe {
            let reg_db: FnRegisterDatabase = transmute(va(base, VA_REGISTER_DATABASE));
            let _ = reg_db(p_rm, 0, db_ptr as *mut c_void, 0);

            let vtbl = *(db_ptr as *const usize);
            let close_ptr = *(vtbl.wrapping_add(0x1C) as *const usize);
            if is_readable(close_ptr, 4) {
                let close_fn: FnDbClose = transmute(close_ptr);
                close_fn(db_ptr as *mut c_void);
            }

            let obj_ptr = (db_ptr + 4) as *mut c_void;
            let obj_vtbl = *((db_ptr + 4) as *const usize);
            let release_ptr = *(obj_vtbl.wrapping_add(0x04) as *const usize);
            if is_readable(release_ptr, 4) {
                let release_fn: FnIObjectRelease = transmute(release_ptr);
                let _ = release_fn(obj_ptr);
            }
        }
    }

    /// Synchronize active Schema 3 `simoder.raw-resources` into `IResourceManager` purely in RAM.
    ///
    /// Returns `Ok(None)` when already synchronized, or `Ok(Some(count))` after mounting/unmounting.
    pub fn sync_in_memory_raw_resources(
        resources: &[ActiveRawResource],
    ) -> Result<Option<usize>, String> {
        let base = VERIFIED_BASE.load(Ordering::Acquire);
        if base == 0 {
            return Err("Resource bridge not initialized".into());
        }
        let fp = compute_resources_fingerprint(resources);
        let mut guard = MOUNTED_STATE.lock();
        if let Some(existing) = guard.as_ref()
            && existing.fingerprint == fp
        {
            return Ok(None);
        }

        // SAFETY: Validated `IResourceManager` global pointer and `RegisterDatabase` vtable slot.
        let p_rm = unsafe { read_resource_manager(base)? };

        if let Some(prev) = guard.take() {
            // SAFETY: Unregisters, closes, and releases the previous Simoder `DatabasePackedFile`.
            unsafe {
                unregister_and_release_db(base, p_rm, prev.db_ptr);
            }
        }

        if resources.is_empty() {
            *guard = Some(MountedDbState {
                db_ptr: 0,
                fingerprint: fp,
            });
            return Ok(Some(0));
        }

        let dbpf_bytes = build_in_memory_dbpf_bytes(resources)?;
        let stream_ptr = InMemoryIStream::new_raw(dbpf_bytes);

        type FnGameAlloc = unsafe extern "cdecl" fn(u32, u32, u32, u32, u32, u32) -> *mut c_void;
        type FnDbpfCtor =
            unsafe extern "thiscall" fn(*mut c_void, *const u16, *mut c_void) -> *mut c_void;
        type FnIObjectAddRef = unsafe extern "thiscall" fn(*mut c_void) -> i32;
        type FnIObjectRelease = unsafe extern "thiscall" fn(*mut c_void) -> i32;
        type FnDbInit = unsafe extern "thiscall" fn(*mut c_void) -> u8;
        type FnDbSetLocation = unsafe extern "thiscall" fn(*mut c_void, *const u16) -> u8;
        type FnDbSetStream = unsafe extern "thiscall" fn(*mut c_void, *mut InMemoryIStream) -> u8;
        type FnDbOpen = unsafe extern "thiscall" fn(*mut c_void, i32, i32, u8) -> u8;
        type FnRegisterDatabase =
            unsafe extern "thiscall" fn(*mut c_void, u8, *mut c_void, i32) -> u8;

        // SAFETY: All entry addresses and signatures verified in `initialize_resource_bridge`.
        unsafe {
            let game_alloc: FnGameAlloc = transmute(va(base, VA_GAME_ALLOC));
            let db_mem = game_alloc(DBPF_OBJECT_SIZE, 0, 0, 0, 0, 0);
            if db_mem.is_null() {
                let _ = istream_release(stream_ptr);
                return Err("GameAlloc(0x378) returned null".into());
            }

            let db_ctor: FnDbpfCtor = transmute(va(base, VA_DBPF_CTOR));
            let p_db = db_ctor(db_mem, null(), null_mut());
            let db_addr = p_db as usize;

            let obj_ptr = (db_addr + 4) as *mut c_void;
            let obj_vtbl = *((db_addr + 4) as *const usize);
            let add_ref: FnIObjectAddRef = transmute(*(obj_vtbl as *const usize));
            let release: FnIObjectRelease = transmute(*(obj_vtbl.wrapping_add(0x04) as *const usize));
            let _ = add_ref(obj_ptr);

            let vtbl = *(db_addr as *const usize);
            let db_init: FnDbInit = transmute(*(vtbl.wrapping_add(0x04) as *const usize));
            let _ = db_init(p_db);

            let wide_uri: Vec<u16> = "simoder://raw-resources"
                .encode_utf16()
                .chain(std::iter::once(0))
                .collect();
            let set_loc: FnDbSetLocation = transmute(*(vtbl.wrapping_add(0x2C) as *const usize));
            let _ = set_loc(p_db, wide_uri.as_ptr());

            let set_stream: FnDbSetStream = transmute(va(base, VA_DBPF_SET_STREAM));
            let _ = set_stream(p_db, stream_ptr);

            let db_open: FnDbOpen = transmute(va(base, VA_DBPF_OPEN));
            if db_open(p_db, 1, 3, 0) == 0 {
                let _ = istream_release(stream_ptr);
                let _ = release(obj_ptr);
                return Err("DatabasePackedFile::Open(1, 3, false) failed on in-memory DBPF".into());
            }

            let reg_db: FnRegisterDatabase = transmute(va(base, VA_REGISTER_DATABASE));
            if reg_db(p_rm, 1, p_db, SIMODER_DATABASE_PRIORITY) == 0 {
                let _ = istream_release(stream_ptr);
                let _ = release(obj_ptr);
                return Err("IResourceManager::RegisterDatabase returned false".into());
            }

            // `DatabasePackedFile` owns its own ref to `stream_ptr` via `SetStream`; drop ours.
            let _ = istream_release(stream_ptr);

            let resource_count = resources.len();
            *guard = Some(MountedDbState {
                db_ptr: db_addr,
                fingerprint: fp,
            });
            Ok(Some(resource_count))
        }
    }

    /// Unregister and release any active in-memory `DatabasePackedFile` on clean DLL shutdown.
    pub fn shutdown_in_memory_raw_resources() {
        let base = VERIFIED_BASE.load(Ordering::Acquire);
        let prev = MOUNTED_STATE.lock().take();
        if base == 0 {
            return;
        }
        if let Some(state) = prev
            && state.db_ptr != 0
            // SAFETY: Unregisters only if `IResourceManager` is still readable during shutdown.
            && let Ok(p_rm) = unsafe { read_resource_manager(base) }
        {
            // SAFETY: Clean teardown of Simoder-owned `DatabasePackedFile`.
            unsafe {
                unregister_and_release_db(base, p_rm, state.db_ptr);
            }
        }
    }
}

#[cfg(not(all(windows, target_pointer_width = "32", not(test))))]
mod native {
    use simoder_mods::ActiveRawResource;

    pub fn initialize_resource_bridge(_verified_retail_file: bool) -> Result<(), String> {
        Err("Resource bridge requires 32-bit Windows retail SimCity.exe".into())
    }

    pub fn is_resource_bridge_ready() -> bool {
        false
    }

    pub fn sync_in_memory_raw_resources(
        _resources: &[ActiveRawResource],
    ) -> Result<Option<usize>, String> {
        Err("Resource bridge requires 32-bit Windows retail SimCity.exe".into())
    }

    pub fn shutdown_in_memory_raw_resources() {}
}

pub use native::{
    initialize_resource_bridge, is_resource_bridge_ready, shutdown_in_memory_raw_resources,
    sync_in_memory_raw_resources,
};
