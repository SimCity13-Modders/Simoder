//! Native module `.dll` host boundary (`ABI v2`), decoupled mod-provided MCP tool extension registry
//! ([`HostToolRegistry`]), and `Simoder-OpenSCP` debug-tools overlay builder (`build_debug_tools_overlay`).
//!
//! Follows `docs/architecture/native-module-boundary.md`, `docs/architecture/mcp-devtool.md` (`ADR 0001`),
//! and `Simoder-SDK/Native/native.h`:
//! - Native `.dll` mods negotiate `ABI v2` (`SimoderModule_QueryV2`, `SimoderModule_PrepareV2`,
//!   `SimoderModule_AdmitV2`, `SimoderModule_QuiesceV2`, `SimoderModule_DrainV2`) against
//!   [`SD_CONTRACT_SHA256_HEX`].
//! - Any mod (Luau, JS, or Native `.dll`) can register optional MCP tools into [`HostToolRegistry`]
//!   without having a hard dependency on the optional `simoder.devtools.mcp` native mod `.dll`.

pub mod tool_registry;

use std::sync::Arc;

use serde::{Deserialize, Serialize};
use strum::{FromRepr, IntoStaticStr, VariantArray};
pub use tool_registry::*;

use crate::types::is_valid_mod_id;

/// Canonical `Simoder-SDK/Native/native.h` normative contract bundle SHA-256 hex digest (`SD_CONTRACT_SHA256_HEX`).
pub const SD_CONTRACT_SHA256_HEX: &str =
    "00c98e36f669c6c4f16f7ffd9073130ae9798543b3d0759d7c1937f443e6e236";

/// Pre-decoded 32-byte array matching [`SD_CONTRACT_SHA256_HEX`].
pub const SD_CONTRACT_SHA256_BYTES: [u8; 32] = [
    0x00, 0xc9, 0x8e, 0x36, 0xf6, 0x69, 0xc6, 0xc4, 0xf1, 0x6f, 0x7f, 0xfd, 0x90, 0x73, 0x13, 0x0a,
    0xe9, 0x79, 0x85, 0x43, 0xb3, 0xd0, 0x75, 0x9d, 0x7c, 0x19, 0x37, 0xf4, 0x43, 0xe6, 0xe2, 0x36,
];

/// Root host ABI version (`SD_U2A_HOST_VERSION = 2`).
pub const SD_U2A_HOST_VERSION: u32 = 2;

/// Leaf table ABI version (`SD_ABI_VERSION = 1`).
pub const SD_LEAF_ABI_VERSION: u32 = 1;

/// Base contract version required by `SdModuleDescriptorV2` (`SD_BASE_CONTRACT_VERSION = 1`).
pub const SD_BASE_CONTRACT_VERSION: u32 = 1;

/// Exact byte size of `SdModuleDescriptorV2` on 32-bit x86 (`56` bytes).
pub const SD_MODULE_DESCRIPTOR_V2_SIZE: u32 = 56;

/// Maximum number of retained native DLL image slots per process (`native-module-boundary.md`).
pub const MAX_RETAINED_NATIVE_IMAGES: usize = 8;

/// Maximum aggregate declared mapped image space across retained native DLLs (`64 MiB`).
pub const MAX_NATIVE_MAPPED_BYTES: usize = 64 * 1024 * 1024;

/// Maximum on-disk byte size of a single native mod `.dll` (`16 MiB`).
pub const MAX_NATIVE_SOURCE_DLL_BYTES: usize = 16 * 1024 * 1024;

/// Maximum number of logical native module generations / staging attempts per process (`128`).
pub const MAX_NATIVE_GENERATIONS: usize = 128;

/// Maximum native lifecycle subscriptions per generation (`64`).
pub const MAX_NATIVE_SUBSCRIPTIONS_PER_GEN: usize = 64;

/// Readiness bitmask flag indicating core runtime initialization (`SD_READY_RUNTIME = 1`).
pub const SD_READY_RUNTIME: u32 = 1;

/// Readiness bitmask flag indicating front-end main menu readiness (`SD_READY_MENU = 2`).
pub const SD_READY_MENU: u32 = 2;

/// Readiness bitmask flag indicating finalized loaded city state (`SD_READY_CITY = 4`).
pub const SD_READY_CITY: u32 = 4;

/// Native module ABI v2 result codes (`SdResult` `0..=16` in `Simoder-SDK/Native/{abi_v1.h,native.h}`).
#[repr(u32)]
#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    Hash,
    Serialize,
    Deserialize,
    FromRepr,
    IntoStaticStr,
    VariantArray,
)]
pub enum SdResult {
    /// Operation succeeded (`SD_OK = 0`).
    Ok = 0,
    /// Invalid argument, null pointer, or malformed descriptor (`SD_INVALID = 1`).
    Invalid = 1,
    /// Incompatible host/module ABI version or contract SHA-256 (`SD_VERSION = 2`).
    Version = 2,
    /// Handle, generation, or invocation reference is stale (`SD_STALE = 3`).
    Stale = 3,
    /// Permission, trust, or authentication check denied (`SD_DENIED = 4`).
    Denied = 4,
    /// Required provider or base runtime is not ready (`SD_NOT_READY = 5`).
    NotReady = 5,
    /// Resource or callback is still in flight; retain module without forced unload (`SD_BUSY = 6`).
    Busy = 6,
    /// Duplicate active owner or conflicting state revision (`SD_CONFLICT = 7`).
    Conflict = 7,
    /// Uncommitted operation was cancelled (`SD_CANCELLED = 8`).
    Cancelled = 8,
    /// Outcome could not be confirmed before deadline (`SD_INDETERMINATE = 9`).
    Indeterminate = 9,
    /// Retained native image or generation budget exhausted; process restart required (`SD_RESTART_REQUIRED = 10`).
    RestartRequired = 10,
    /// Caller buffer capacity is smaller than `required` (`SD_BUFFER_SMALL = 11`).
    BufferSmall = 11,
    /// Requested optional capability or cached snapshot is unavailable (`SD_CAPABILITY_UNAVAILABLE = 12`).
    CapabilityUnavailable = 12,
    /// City or region context epoch expired (`SD_CONTEXT_EXPIRED = 13`).
    ContextExpired = 13,
    /// Underlying game or runtime operation failed (`SD_OPERATION_FAILED = 14`).
    OperationFailed = 14,
    /// Per-generation task, mailbox, or subscription cap reached (`SD_RESOURCE_LIMIT = 15`).
    ResourceLimit = 15,
    /// Operation exceeded its bounded execution deadline (`SD_DEADLINE = 16`).
    Deadline = 16,
}

/// Canonical observed runtime lifecycle state (`SdLifecycleState` in `abi_v1.h`).
#[repr(u32)]
#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    Hash,
    Serialize,
    Deserialize,
    FromRepr,
    IntoStaticStr,
    VariantArray,
)]
pub enum SdLifecycleState {
    /// Process bootstrap before runtime initialization (`SD_STATE_COLD = 0`).
    Cold = 0,
    /// Runtime and base services are initializing (`SD_STATE_STARTING = 1`).
    Starting = 1,
    /// Runtime is ready to admit and serve operations (`SD_STATE_READY = 2`).
    Ready = 2,
    /// Runtime is quiescing and draining active services (`SD_STATE_STOPPING = 3`).
    Stopping = 3,
    /// Runtime has stopped; no MCP or native services are callable (`SD_STATE_STOPPED = 4`).
    Stopped = 4,
    /// Runtime encountered a sticky fault requiring restart (`SD_STATE_FAULTED = 5`).
    Faulted = 5,
}

/// Host-allocated opaque handle (`SdHandle`: `slot` + `generation`, 8 bytes).
#[repr(C)]
#[derive(
    Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default, Serialize, Deserialize,
)]
pub struct SdHandle {
    /// 32-bit slot index in the host registry.
    pub slot: u32,
    /// 32-bit monotonic generation counter (never `0` for valid handles).
    pub generation: u32,
}

/// Structure negotiation header prefix (`SdHeader`: `size` + `version`, 8 bytes).
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct SdHeader {
    /// Exact structure byte size.
    pub size: u32,
    /// Structure schema version.
    pub version: u32,
}

/// Module contract identity descriptor (`SdModuleDescriptorV2`, 56 bytes).
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SdModuleDescriptorV2 {
    /// Negotiation header (`size == 56`, `version == 2`).
    pub header: SdHeader,
    /// Requested host ABI version (`2`).
    pub host_version: u32,
    /// Required base contract version (`1`).
    pub base_contract_version: u32,
    /// Reserved field (must be `0`).
    pub reserved0: u32,
    /// Reserved field (must be `0`).
    pub reserved1: u32,
    /// 32-byte raw SHA-256 digest matching [`SD_CONTRACT_SHA256_HEX`].
    pub contract_sha256: [u8; 32],
}

impl SdModuleDescriptorV2 {
    /// Construct an empty caller-sized V2 descriptor header ready for `SimoderModule_QueryV2`.
    pub const fn new_for_query() -> Self {
        Self {
            header: SdHeader {
                size: SD_MODULE_DESCRIPTOR_V2_SIZE,
                version: SD_U2A_HOST_VERSION,
            },
            host_version: 0,
            base_contract_version: 0,
            reserved0: 0,
            reserved1: 0,
            contract_sha256: [0; 32],
        }
    }

    /// Populate this descriptor with the canonical `ABI v2` / `base v1` identity ([`SD_CONTRACT_SHA256_HEX`]).
    pub fn fill_canonical(&mut self) -> SdResult {
        if self.header.size != SD_MODULE_DESCRIPTOR_V2_SIZE
            || self.header.version != SD_U2A_HOST_VERSION
        {
            return SdResult::Version;
        }
        self.host_version = SD_U2A_HOST_VERSION;
        self.base_contract_version = SD_BASE_CONTRACT_VERSION;
        self.reserved0 = 0;
        self.reserved1 = 0;
        self.contract_sha256 = SD_CONTRACT_SHA256_BYTES;
        SdResult::Ok
    }

    /// Validate that a descriptor returned by `SimoderModule_QueryV2` matches [`SD_CONTRACT_SHA256_HEX`] and V2 rules.
    pub fn validate_canonical(&self) -> SdResult {
        if self.header.size != SD_MODULE_DESCRIPTOR_V2_SIZE
            || self.header.version != SD_U2A_HOST_VERSION
            || self.host_version != SD_U2A_HOST_VERSION
            || self.base_contract_version != SD_BASE_CONTRACT_VERSION
            || self.reserved0 != 0
            || self.reserved1 != 0
            || self.contract_sha256 != SD_CONTRACT_SHA256_BYTES
        {
            return SdResult::Version;
        }
        SdResult::Ok
    }
}

/// Lifecycle state of a single native module generation managed by [`NativeModuleHost`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum NativeModuleScopeState {
    /// Staged via `PrepareV2`; awaiting base readiness and `AdmitV2` (no listeners or tools active yet).
    Prepared,
    /// Post-commit admitted via `AdmitV2`; actively serving.
    Active,
    /// `QuiesceV2` called; admission closed, awaiting `DrainV2` completion.
    Stopping,
    /// `DrainV2` returned `SdResult::Ok`; borrow ended, image remains retained until process exit.
    Drained,
}

/// Cooperative V2 lifecycle interface implemented by a native mod `.dll` (such as `simoder_mcp.dll`).
pub trait NativeModuleCallbacks: Send + Sync {
    /// `SimoderModule_QueryV2`: negotiate ABI v2 and populate `descriptor`.
    fn query_v2(&self, host_version: u32, descriptor: &mut SdModuleDescriptorV2) -> SdResult;

    /// `SimoderModule_PrepareV2`: stage module generation for `owner` without starting listeners.
    fn prepare_v2(&self, owner: SdHandle) -> SdResult;

    /// `SimoderModule_AdmitV2`: activate module generation after native trust and base readiness.
    fn admit_v2(&self, owner: SdHandle) -> SdResult;

    /// `SimoderModule_QuiesceV2`: close listeners and reject new work immediately (idempotent, nonblocking).
    fn quiesce_v2(&self, owner: SdHandle) -> SdResult;

    /// `SimoderModule_DrainV2`: confirm no in-flight callbacks remain (`Busy` retains module and resources).
    fn drain_v2(&self, owner: SdHandle) -> SdResult;
}

/// Reviewed registration metadata required before loading a native mod `.dll` (`native-module-boundary.md`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NativeModuleRegistration {
    /// Canonical mod identifier (for example `"simoder.devtools.mcp"`).
    pub module_id: String,
    /// Expected lowercase 64-character SHA-256 hex digest of the `.dll` file.
    pub expected_sha256: String,
    /// Actual observed SHA-256 hex digest of the staged `.dll` file.
    pub observed_sha256: String,
    /// On-disk byte length of the `.dll` file (`<= 16 MiB`).
    pub source_size_bytes: usize,
    /// Declared PE mapped image size in bytes (`SizeOfImage`, aggregate `<= 64 MiB`).
    pub mapped_image_bytes: usize,
    /// Explicit user approval for unrestricted native machine code execution.
    pub native_trust_approved: bool,
}

struct RetainedNativeImage {
    module_id: String,
    sha256: String,
    mapped_image_bytes: usize,
}

struct NativeGenerationRecord {
    owner: SdHandle,
    module_id: String,
    state: NativeModuleScopeState,
    module_drained_once: bool,
    callbacks: Arc<dyn NativeModuleCallbacks>,
}

/// Core-owned Native Module Host Boundary (`ABI v2`) managing trusted native `.dll` mods (`native-module-boundary.md`).
pub struct NativeModuleHost {
    max_retained_images: usize,
    max_generations: usize,
    base_ready: bool,
    lifecycle_state: SdLifecycleState,
    readiness_flags: u32,
    retained_images: Vec<RetainedNativeImage>,
    generations: Vec<NativeGenerationRecord>,
}

impl Default for NativeModuleHost {
    fn default() -> Self {
        Self::new()
    }
}

impl NativeModuleHost {
    /// Create a [`NativeModuleHost`] with production finite process budgets (`8` images, `128` generations).
    pub const fn new() -> Self {
        Self::with_limits(MAX_RETAINED_NATIVE_IMAGES, MAX_NATIVE_GENERATIONS)
    }

    /// Create a [`NativeModuleHost`] with custom image and generation ceilings.
    pub const fn with_limits(max_retained_images: usize, max_generations: usize) -> Self {
        Self {
            max_retained_images,
            max_generations,
            base_ready: false,
            lifecycle_state: SdLifecycleState::Cold,
            readiness_flags: 0,
            retained_images: Vec::new(),
            generations: Vec::new(),
        }
    }

    /// Update the host's `base` Luau provider readiness and runtime lifecycle flags.
    pub fn set_runtime_readiness(
        &mut self,
        base_ready: bool,
        lifecycle_state: SdLifecycleState,
        readiness_flags: u32,
    ) {
        self.base_ready = base_ready;
        self.lifecycle_state = lifecycle_state;
        self.readiness_flags = readiness_flags;
    }

    /// Return the current host [`SdLifecycleState`].
    pub const fn lifecycle_state(&self) -> SdLifecycleState {
        self.lifecycle_state
    }

    /// Return the current readiness bitmask (`SD_READY_RUNTIME | SD_READY_MENU | SD_READY_CITY`).
    pub const fn readiness_flags(&self) -> u32 {
        self.readiness_flags
    }

    /// Return the count of retained native `.dll` images (never decremented before process exit).
    pub fn retained_image_count(&self) -> usize {
        self.retained_images.len()
    }

    /// Return the aggregate mapped image bytes across all retained native `.dll` images.
    pub fn total_mapped_bytes(&self) -> usize {
        self.retained_images
            .iter()
            .map(|img| img.mapped_image_bytes)
            .sum()
    }

    /// Return the current [`NativeModuleScopeState`] for `owner`, if known.
    pub fn generation_state(&self, owner: SdHandle) -> Option<NativeModuleScopeState> {
        self.generations
            .iter()
            .find(|g| g.owner == owner)
            .map(|g| g.state)
    }

    /// Load, verify, query (`QueryV2`), and stage (`PrepareV2`) a native module `.dll` generation.
    pub fn prepare_module(
        &mut self,
        reg: &NativeModuleRegistration,
        callbacks: Arc<dyn NativeModuleCallbacks>,
    ) -> Result<SdHandle, SdResult> {
        if !is_valid_mod_id(&reg.module_id)
            || reg.source_size_bytes == 0
            || reg.source_size_bytes > MAX_NATIVE_SOURCE_DLL_BYTES
            || reg.mapped_image_bytes == 0
        {
            return Err(SdResult::Invalid);
        }
        if !reg.native_trust_approved
            || !reg.expected_sha256.eq_ignore_ascii_case(&reg.observed_sha256)
            || reg.expected_sha256.len() != 64
        {
            return Err(SdResult::Denied);
        }

        if self.generations.iter().any(|g| {
            g.module_id == reg.module_id && g.state != NativeModuleScopeState::Drained
        }) {
            return Err(SdResult::Conflict);
        }

        if self.generations.len() >= self.max_generations {
            return Err(SdResult::RestartRequired);
        }

        let already_retained = self.retained_images.iter().any(|img| {
            img.module_id == reg.module_id && img.sha256.eq_ignore_ascii_case(&reg.observed_sha256)
        });
        if !already_retained {
            if self.retained_images.len() >= self.max_retained_images
                || self
                    .total_mapped_bytes()
                    .saturating_add(reg.mapped_image_bytes)
                    > MAX_NATIVE_MAPPED_BYTES
            {
                return Err(SdResult::RestartRequired);
            }
            self.retained_images.push(RetainedNativeImage {
                module_id: reg.module_id.clone(),
                sha256: reg.observed_sha256.to_ascii_lowercase(),
                mapped_image_bytes: reg.mapped_image_bytes,
            });
        }

        let mut descriptor = SdModuleDescriptorV2::new_for_query();
        let query_res = callbacks.query_v2(SD_U2A_HOST_VERSION, &mut descriptor);
        if query_res != SdResult::Ok {
            return Err(query_res);
        }
        let valid_res = descriptor.validate_canonical();
        if valid_res != SdResult::Ok {
            return Err(valid_res);
        }

        let gen_num = (self.generations.len() + 1) as u32;
        let owner = SdHandle {
            slot: gen_num,
            generation: gen_num,
        };
        let prep_res = callbacks.prepare_v2(owner);
        if prep_res != SdResult::Ok {
            let _ = callbacks.quiesce_v2(owner);
            let _ = callbacks.drain_v2(owner);
            self.generations.push(NativeGenerationRecord {
                owner,
                module_id: reg.module_id.clone(),
                state: NativeModuleScopeState::Drained,
                module_drained_once: true,
                callbacks,
            });
            return Err(prep_res);
        }

        self.generations.push(NativeGenerationRecord {
            owner,
            module_id: reg.module_id.clone(),
            state: NativeModuleScopeState::Prepared,
            module_drained_once: false,
            callbacks,
        });
        Ok(owner)
    }

    /// Admit a previously prepared native module generation (`AdmitV2`) once `base` and runtime are ready.
    pub fn admit_module(&mut self, owner: SdHandle) -> SdResult {
        if !self.base_ready || self.lifecycle_state != SdLifecycleState::Ready {
            return SdResult::NotReady;
        }
        let Some(idx) = self.generations.iter().position(|g| g.owner == owner) else {
            return SdResult::Stale;
        };
        if self.generations[idx].state != NativeModuleScopeState::Prepared {
            return SdResult::Invalid;
        }
        let module_id = self.generations[idx].module_id.clone();
        if self.generations.iter().enumerate().any(|(i, g)| {
            i != idx && g.module_id == module_id && g.state == NativeModuleScopeState::Active
        }) {
            return SdResult::Conflict;
        }

        let callbacks = Arc::clone(&self.generations[idx].callbacks);
        let res = callbacks.admit_v2(owner);
        if res == SdResult::Ok {
            self.generations[idx].state = NativeModuleScopeState::Active;
            SdResult::Ok
        } else {
            let _ = callbacks.quiesce_v2(owner);
            let drain_res = callbacks.drain_v2(owner);
            self.generations[idx].state = if drain_res == SdResult::Ok {
                NativeModuleScopeState::Drained
            } else {
                NativeModuleScopeState::Stopping
            };
            self.generations[idx].module_drained_once = drain_res == SdResult::Ok;
            res
        }
    }

    /// Close host admission and cooperatively quiesce an active or prepared generation (`QuiesceV2`).
    pub fn quiesce_module(&mut self, owner: SdHandle) -> SdResult {
        let Some(rec) = self.generations.iter_mut().find(|g| g.owner == owner) else {
            return SdResult::Stale;
        };
        if rec.state == NativeModuleScopeState::Drained {
            return SdResult::Ok;
        }
        rec.state = NativeModuleScopeState::Stopping;
        let callbacks = Arc::clone(&rec.callbacks);
        callbacks.quiesce_v2(owner)
    }

    /// Cooperatively drain a stopping generation (`DrainV2`).
    pub fn drain_module(&mut self, owner: SdHandle) -> SdResult {
        let Some(rec) = self.generations.iter_mut().find(|g| g.owner == owner) else {
            return SdResult::Stale;
        };
        if rec.state == NativeModuleScopeState::Drained {
            return SdResult::Ok;
        }
        if rec.state == NativeModuleScopeState::Active
            || rec.state == NativeModuleScopeState::Prepared
        {
            rec.state = NativeModuleScopeState::Stopping;
            let _ = rec.callbacks.quiesce_v2(owner);
        }
        if !rec.module_drained_once {
            let res = rec.callbacks.drain_v2(owner);
            if res != SdResult::Ok {
                return res;
            }
            rec.module_drained_once = true;
        }
        rec.state = NativeModuleScopeState::Drained;
        SdResult::Ok
    }
}
