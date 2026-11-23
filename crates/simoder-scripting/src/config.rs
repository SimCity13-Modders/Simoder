//! Authoritative specification constants, resource budgets, and binary protocol addresses for `simoder-scripting`.

/// Host API version advertised to sandboxed script VMs via `simoder.apiVersion`.
pub const SCRIPT_API_VERSION: u32 = 1;

/// DBPF property-list resource type identifier (`PROP`, `0x00B1B104`) targeted by `simoder.props`.
pub const PROP_RESOURCE_TYPE_ID: u32 = 0x00B1_B104;

/// Reserved mod identifier exclusively assigned to a bootstrap base script runtime.
pub const RESERVED_BASE_MOD_ID: &str = "simoder.base";

/// Minimum configurable per-VM heap limit (`2 MiB`) enforced by [`crate::types::ScriptLimits`].
pub const MIN_VM_MEMORY_BYTES: usize = 2 * 1024 * 1024;

/// Default per-VM heap limit (`8 MiB`) matching the default `[scripts] memoryMiB = 8` manifest setting.
pub const DEFAULT_VM_MEMORY_BYTES: usize = 8 * 1024 * 1024;

/// Maximum configurable per-VM heap limit (`64 MiB`) inside the 32-bit game process.
pub const MAX_VM_MEMORY_BYTES: usize = 64 * 1024 * 1024;

/// Aggregate declared heap cap (`128 MiB`) shared across all live and staged VMs in x86 address space.
pub const MAX_AGGREGATE_SCRIPT_MEMORY_BYTES: usize = 128 * 1024 * 1024;

/// Minimum per-callback execution time budget (`1 ms`).
pub const MIN_CALLBACK_BUDGET_MS: u32 = 1;

/// Default per-callback execution time budget (`2 ms`) matching `[scripts] budgetMs = 2`.
pub const DEFAULT_CALLBACK_BUDGET_MS: u32 = 2;

/// Maximum configurable per-callback execution time budget (`20 ms`).
pub const MAX_CALLBACK_BUDGET_MS: u32 = 20;

/// Default QuickJS call-stack byte limit (`256 KiB`), bounding script recursion depth.
pub const DEFAULT_JS_MAX_STACK_BYTES: usize = 256 * 1024;

/// Maximum permitted entry script or module source size (`8 MiB`).
pub const MAX_SCRIPT_SOURCE_BYTES: usize = 8 * 1024 * 1024;

/// Maximum number of concurrent VM scheduler owner slots (`128`).
pub const MAX_VM_SLOTS: usize = 128;

/// Target automatic `onTick` cadence (`20 Hz`, corresponding to [`SCHEDULER_TICK_INTERVAL_MS`]).
pub const SCHEDULER_TARGET_HZ: u32 = 20;

/// Nominal minimum interval (`50 ms`) between consecutive automatic `onTick` invocations per VM.
pub const SCHEDULER_TICK_INTERVAL_MS: u64 = 50;

/// Soft aggregate wall-clock budget (`5 ms`) per scheduler round-robin cycle.
pub const SCHEDULER_SOFT_CYCLE_BUDGET_MS: u64 = 5;

/// Maximum byte length (`2048 B`) accepted per `simoder.log` / `print` message and diagnostic string.
pub const MAX_LOG_MESSAGE_BYTES: usize = 2048;

/// Maximum number of unread log messages (`128`) retained per VM before incrementing `dropped_logs`.
pub const MAX_BUFFERED_LOGS: usize = 128;

/// Maximum UTF-8 byte length (`2048 B`) retained in live/published VM status diagnostics.
pub const MAX_DIAGNOSTIC_BYTES: usize = 2048;

/// Maximum byte length (`128 B`) accepted for `simoder.props` resource or alias references.
pub const MAX_PROP_REF_BYTES: usize = 128;

/// Maximum queued `setFloat` / `clearFloat` commands permitted within one callback execution (`256`).
pub const MAX_PROP_COMMANDS_PER_CALLBACK: usize = 256;

/// Maximum persistent scripted PROP field overrides retained per mod (`256`).
pub const MAX_PROP_OVERRIDES_PER_SCRIPT: usize = 256;

/// Maximum synchronous cross-VM hook re-entry stack depth (`8`), bounding A-to-B-to-A cycles.
pub const MAX_CROSS_VM_REENTRY_DEPTH: usize = 8;

/// Maximum combined pre-hook and post-hook fan-out subscribers executed for one intercepted call (`64`).
pub const MAX_CROSS_VM_FANOUT: usize = 64;

/// Maximum materialized nodes (`512`) traversed when projecting an object graph across VMs.
pub const MAX_PROXY_GRAPH_NODES: usize = 512;

/// Maximum subscribers (`128`) permitted per custom namespaced `RegisterHook` event.
pub const MAX_EVENT_SUBSCRIBERS: usize = 128;

/// Maximum serialized payload size (`64 KiB`) accepted per emitted event.
pub const MAX_EVENT_PAYLOAD_BYTES: usize = 65_536;

/// Maximum concurrently pending `DelayedCalls` entries owned by a single mod generation (`128`).
pub const MAX_DELAYED_CALLS_PER_OWNER: usize = 128;

/// Maximum due `DelayedCalls` dispatches executed in one scheduler turn (`32`) to prevent catch-up bursts.
pub const MAX_DELAYED_CALLS_PER_TURN: usize = 32;

/// Maximum byte length (`512 KiB`) accepted per mod translation dictionary file.
pub const MAX_LOCALE_FILE_BYTES: usize = 512 * 1024;

/// Maximum key-value entries (`8192`) accepted within a single mod translation dictionary.
pub const MAX_LOCALE_ENTRIES_PER_FILE: usize = 8_192;

/// Maximum explicit fallback chain length (`16`) evaluated during localization lookup.
pub const MAX_LOCALE_FALLBACK_DEPTH: usize = 16;

/// Maximum translation dictionary files (`32`) mounted per mod localization catalog.
pub const MAX_LOCALE_FILES_PER_MOD: usize = 32;

/// Base-game `0x0A98EAF0` locale table instance IDs from `SimCity-OpenSCP` (`Locale/<lang>/Data.package`).
///
/// Overwriting these IDs outside `Locale/` has no effect in retail DBPF loading, so custom mods must
/// allocate isolated table IDs via [`simoder_formats::mod_locale_table_id`].
pub const VANILLA_LOCALE_TABLE_IDS: [u32; 3] = [0x6C96_9DEE, 0x50AA_0BEA, 0x4B54_417A];

/// Retail `SimCity.exe` virtual address (`0x5EB640`) of `EcoGame::FindLoadedScripts`, whose script cache key
/// combines `(game_id, current_version, add_on_count, ordered_addon_ids, ordered_addon_versions)`.
pub const ECOGAME_FIND_LOADED_SCRIPTS_VA: u32 = 0x005E_B640;

/// Retail `SimCity.exe` virtual address (`0x5ED4E0`) of `EcoGame::SwitchToProductionVersions`.
pub const ECOGAME_SWITCH_TO_PRODUCTION_VA: u32 = 0x005E_D4E0;

/// Retail `SimCity.exe` virtual address (`0x899EC0`) of the JS-to-C++ `window.Client` command dispatcher.
pub const UI_COMMAND_DISPATCHER_VA: u32 = 0x0089_9EC0;

/// Retail `SimCity.exe` virtual address (`0x8C73F0`) resolving UI command names via 32-bit FNV-1 hash.
pub const UI_COMMAND_HASH_DISPATCH_VA: u32 = 0x008C_73F0;

/// Maximum UTF-8 byte length (`16 KiB`) permitted per UI bridge command string.
pub const MAX_UI_COMMAND_BYTES: usize = 16 * 1024;

/// Default provenance tag attributed to dynamic overrides created via Luau `simoder.props`.
pub const LUAU_DEFAULT_SOURCE_ORIGIN: &str = "Luau host API";

/// Default provenance tag attributed to dynamic overrides created via JavaScript `simoder.props`.
pub const JS_DEFAULT_SOURCE_ORIGIN: &str = "JS host API";
