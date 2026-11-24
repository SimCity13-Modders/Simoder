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
mod istream;

#[cfg(all(windows, target_pointer_width = "32", not(test)))]
mod native {
    use std::ffi::c_void;
    use std::mem::{size_of, transmute, zeroed};
    use std::ptr::{null, null_mut};

    use parking_lot::Mutex;
    use simoder_core::{Digest, ResolutionStrategy, Sha256, resolve_symbol};
    use simoder_mods::ActiveRawResource;

    use super::build_in_memory_dbpf_bytes;
    use super::istream::{InMemoryIStream, istream_release};
    use crate::win32_hook_util::{read_bytes_exact, snapshot_loaded_module};

    const DBPF_OBJECT_SIZE: u32 = 0x378;
    const SIMODER_DATABASE_PRIORITY: i32 = 1000;

    const MEM_COMMIT: u32 = 0x1000;
    const PAGE_NOACCESS: u32 = 0x01;
    const PAGE_GUARD: u32 = 0x100;


    #[derive(Clone, Copy)]
    struct ResolvedResourceBridge {
        resource_manager_global: usize,
        register_database: usize,
        game_alloc: usize,
        dbpf_ctor: usize,
        dbpf_open: usize,
        dbpf_set_stream: usize,
    }

    static RESOLVED_BRIDGE: Mutex<Option<ResolvedResourceBridge>> = Mutex::new(None);
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
        fn VirtualQuery(
            lp_address: *const c_void,
            lp_buffer: *mut MemoryBasicInformation,
            dw_length: usize,
        ) -> usize;
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

    /// Resolve `SimCity.exe` resource-layer entrypoints via [`simoder_core::binary_resolver`].
    pub fn initialize_resource_bridge(verified_retail_file: bool) -> Result<(), String> {
        *RESOLVED_BRIDGE.lock() = None;
        if !verified_retail_file {
            return Err("Resource bridge requires verified SimCity.exe 10.3.6.0 SHA-256".into());
        }
        let snap = snapshot_loaded_module(None)?;
        let view = snap.as_view();

        let get_rm = resolve_symbol(
            &view,
            &ResolutionStrategy::VerifiedHint {
                rva: 0x0000_5BF0,
                expected_prefix: &[0xA1],
            },
        )
        .ok_or_else(|| "Resource manager getter signature mismatch".to_string())?;
        let mut get_rm_stub = [0u8; 6];
        if !read_bytes_exact(get_rm.va as usize, &mut get_rm_stub) || get_rm_stub[5] != 0xC3 {
            return Err("Resource manager getter stub validation failed".into());
        }
        let resource_manager_global = u32::from_le_bytes([
            get_rm_stub[1],
            get_rm_stub[2],
            get_rm_stub[3],
            get_rm_stub[4],
        ]) as usize;

        let register_database = resolve_symbol(
            &view,
            &ResolutionStrategy::HintThenFallback {
                hint_rva: 0x0051_A2C0,
                hint_prefix: &[0x53, 0x56, 0x32, 0xDB, 0x57, 0x8B, 0xF9],
                fallback: &ResolutionStrategy::UniqueSignature {
                    pattern: "53 56 32 DB 57 8B F9 38 5C 24 10 74 3F 8B 07 8B 74 24 14 8B 50 50 56",
                    result_offset: 0,
                },
            },
        )
        .ok_or_else(|| "RegisterDatabase signature not found or ambiguous".to_string())?
        .va as usize;

        let game_alloc = resolve_symbol(
            &view,
            &ResolutionStrategy::VerifiedHint {
                rva: 0x0051_DD80,
                expected_prefix: &[0x56, 0xE8],
            },
        )
        .ok_or_else(|| "GameAlloc signature mismatch".to_string())?
        .va as usize;

        let dbpf_ctor = resolve_symbol(
            &view,
            &ResolutionStrategy::HintThenFallback {
                hint_rva: 0x004E_FFC0,
                hint_prefix: &[0x53, 0x56, 0x8B, 0xF1, 0xC7, 0x46, 0x04],
                fallback: &ResolutionStrategy::UniqueSignature {
                    pattern: "53 56 8B F1 C7 46 04 ?? ?? ?? ?? 57 C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 33 C0 8D 4E 08 87 01 8B 44 24 14 33 DB C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 88 5E 0C 3B C3 75 05 E8 ?? ?? ?? ?? 89 46 10 89 5E 14 89 5E 18",
                    result_offset: 0,
                },
            },
        )
        .ok_or_else(|| "DatabasePackedFile::ctor signature not found or ambiguous".to_string())?
        .va as usize;

        let dbpf_open = resolve_symbol(
            &view,
            &ResolutionStrategy::HintThenFallback {
                hint_rva: 0x004E_EA10,
                hint_prefix: &[0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x14, 0x00],
                fallback: &ResolutionStrategy::UniqueSignature {
                    pattern: "56 8B F1 83 7E 14 00 74 06 32 C0 5E C2 0C 00 53 32 DB 83 BE 64 02 00 00",
                    result_offset: 0,
                },
            },
        )
        .ok_or_else(|| "DatabasePackedFile::Open signature not found or ambiguous".to_string())?
        .va as usize;

        let dbpf_set_stream = resolve_symbol(
            &view,
            &ResolutionStrategy::HintThenFallback {
                hint_rva: 0x004E_E780,
                hint_prefix: &[0x53, 0x8B, 0xD9, 0x83, 0x7B, 0x14, 0x00],
                fallback: &ResolutionStrategy::UniqueSignature {
                    pattern: "53 8B D9 83 7B 14 00 75 38 56 8B 74 24 0C 57 8B BB 60 02 00 00 3B F7 74",
                    result_offset: 0,
                },
            },
        )
        .ok_or_else(|| {
            "DatabasePackedFile::SetStream signature not found or ambiguous".to_string()
        })?
        .va as usize;

        *RESOLVED_BRIDGE.lock() = Some(ResolvedResourceBridge {
            resource_manager_global,
            register_database,
            game_alloc,
            dbpf_ctor,
            dbpf_open,
            dbpf_set_stream,
        });
        Ok(())
    }

    pub fn is_resource_bridge_ready() -> bool {
        RESOLVED_BRIDGE.lock().is_some()
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

    unsafe fn read_resource_manager(bridge: &ResolvedResourceBridge) -> Result<*mut c_void, String> {
        let global_ptr = bridge.resource_manager_global;
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
        if reg_fn != bridge.register_database {
            return Err(format!(
                "IResourceManager RegisterDatabase mismatch: expected 0x{:08X}, got 0x{reg_fn:08X}",
                bridge.register_database
            ));
        }
        Ok(p_rm)
    }

    unsafe fn unregister_and_release_db(
        bridge: &ResolvedResourceBridge,
        p_rm: *mut c_void,
        db_ptr: usize,
    ) {
        if db_ptr == 0 || !is_readable(db_ptr, 8) {
            return;
        }
        type FnRegisterDatabase = unsafe extern "thiscall" fn(*mut c_void, u8, *mut c_void, i32) -> u8;
        type FnDbClose = unsafe extern "thiscall" fn(*mut c_void);
        type FnIObjectRelease = unsafe extern "thiscall" fn(*mut c_void) -> i32;

        // SAFETY: Verified `register_database` signature and vtable slots of `DatabasePackedFile`.
        unsafe {
            let reg_db: FnRegisterDatabase = transmute(bridge.register_database);
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
        let Some(bridge) = *RESOLVED_BRIDGE.lock() else {
            return Err("Resource bridge not initialized".into());
        };
        let fp = compute_resources_fingerprint(resources);
        let mut guard = MOUNTED_STATE.lock();
        if let Some(existing) = guard.as_ref()
            && existing.fingerprint == fp
        {
            return Ok(None);
        }

        // SAFETY: Validated `IResourceManager` global pointer and `RegisterDatabase` vtable slot.
        let p_rm = unsafe { read_resource_manager(&bridge)? };

        if let Some(prev) = guard.take() {
            // SAFETY: Unregisters, closes, and releases the previous Simoder `DatabasePackedFile`.
            unsafe {
                unregister_and_release_db(&bridge, p_rm, prev.db_ptr);
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
            let game_alloc: FnGameAlloc = transmute(bridge.game_alloc);
            let db_mem = game_alloc(DBPF_OBJECT_SIZE, 0, 0, 0, 0, 0);
            if db_mem.is_null() {
                let _ = istream_release(stream_ptr);
                return Err("GameAlloc(0x378) returned null".into());
            }

            let db_ctor: FnDbpfCtor = transmute(bridge.dbpf_ctor);
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

            let set_stream: FnDbSetStream = transmute(bridge.dbpf_set_stream);
            let _ = set_stream(p_db, stream_ptr);

            let db_open: FnDbOpen = transmute(bridge.dbpf_open);
            if db_open(p_db, 1, 3, 0) == 0 {
                let _ = istream_release(stream_ptr);
                let _ = release(obj_ptr);
                return Err("DatabasePackedFile::Open(1, 3, false) failed on in-memory DBPF".into());
            }

            let reg_db: FnRegisterDatabase = transmute(bridge.register_database);
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
        let Some(bridge) = *RESOLVED_BRIDGE.lock() else {
            return;
        };
        let prev = MOUNTED_STATE.lock().take();
        if let Some(state) = prev
            && state.db_ptr != 0
            // SAFETY: Unregisters only if `IResourceManager` is still readable during shutdown.
            && let Ok(p_rm) = unsafe { read_resource_manager(&bridge) }
        {
            // SAFETY: Clean teardown of Simoder-owned `DatabasePackedFile`.
            unsafe {
                unregister_and_release_db(&bridge, p_rm, state.db_ptr);
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
