//! Verified retail `SimCity.exe` and `EAWebKit.dll` diagnostic constants, bridge sampling quotas,
//! and crash-report retention bounds.

pub use simoder_core::{LOG_DRAIN_BATCH_SIZE, MAX_CAPTURED_LOG_BYTES, MAX_LOG_QUEUE_CAPACITY};

/// Maximum number of stack frames recorded in a single [`crate::CrashReport`].
pub const MAX_CRASH_STACK_FRAMES: usize = 64;

/// Maximum number of recent log lines attached as the diagnostic log tail in a [`crate::CrashReport`].
pub const MAX_CRASH_LOG_TAIL_LINES: usize = 64;

/// Maximum serialized byte size of a shareable [`crate::CrashReport`] (`256 KiB`).
pub const MAX_CRASH_REPORT_BYTES: usize = 256 * 1024;

/// Maximum number of historical crash report files retained on disk before pruning oldest files.
pub const MAX_RETAINED_CRASH_REPORTS: usize = 16;

/// Default relative log file path under `simoder/` (`docs/logging.md`).
pub const MAIN_LOG_RELATIVE_PATH: &str = "logs/sc13modloader.log";

/// Default relative bootstrap diagnostic log path under `simoder/` (`docs/logging.md`).
pub const BOOTSTRAP_LOG_RELATIVE_PATH: &str = "logs/proxy-bootstrap.log";

/// Default relative crash report directory under `simoder/`.
pub const CRASH_REPORTS_RELATIVE_DIR: &str = "crashes";

/// Per-channel capacity for bounded UI/native bridge correlation queues (`Simoder-OpenSCP` `native_agent.js`).
pub const MAX_BRIDGE_CHANNEL_QUEUE: usize = 128;

/// Default per-window admission quota for ordinary `PostGameCommand` and general bridge channels (`80/s`).
pub const DEFAULT_BRIDGE_COMMAND_QUOTA: usize = 80;

/// Default per-window admission quota for high-frequency `RequestGameData` polling queries (`40/s`).
pub const DEFAULT_BRIDGE_QUERY_QUOTA: usize = 40;

/// Dedicated per-window admission quota for `periodTimes/0` and `gamespeed` clock queries (`12/s`).
pub const DEFAULT_BRIDGE_CLOCK_QUOTA: usize = 12;

/// Dedicated per-window admission quota for `RegionTemplates` export queries (`2/s`).
pub const DEFAULT_BRIDGE_TEMPLATE_QUOTA: usize = 2;

/// Dedicated per-window admission quota for region lifecycle queries (`CreateRegion`, `CreateBox`, `GetRegionDetails`, `LatestBox`, `activeBoxId`, `ActiveCityUID`, `RegionID`) (`4/s`).
pub const DEFAULT_BRIDGE_REGION_QUOTA: usize = 4;

/// Dedicated per-window admission quota reserved for state-mutating `RequestGameData` operations
/// (`SaveLocalGame`, `SaveRegionAs`, `UpdateBoxName`, `UpdateRegionName`, `CreateRegion`, `CreateBox`) (`80/s`).
pub const DEFAULT_BRIDGE_MUTATION_QUOTA: usize = 80;

/// Default per-window admission quota for `ReceiveGameEventFromClient` reverse events (`80/s`).
pub const DEFAULT_BRIDGE_EVENT_QUOTA: usize = 80;

/// Maximum age in milliseconds (`5000 ms`) within which a `periodTimes/0` sample annotates bridge events (`BridgeTime`).
pub const BRIDGE_CLOCK_MAX_AGE_MS: u64 = 5_000;

/// Minimum stationary duration in milliseconds (`3000 ms`) before `BridgeTime::advancing` reports `Some(false)`.
pub const BRIDGE_CLOCK_STALL_MS: i64 = 3_000;

/// Retail UI event ID (`182429322`) emitted on simulation speed / pause state changes (`BridgeTime`).
pub const UI_EVENT_SPEED_CHANGED_ID: u64 = 182_429_322;

/// Retail speed enum value (`182362487`) representing a paused simulation in `gamespeed` and `SPEED_CHANGED`.
pub const UI_SPEED_VALUE_PAUSED: u64 = 182_362_487;

/// Retail speed enum values representing active (unpaused) simulation speeds (turtle / llama / cheetah).
pub const UI_SPEED_VALUES_RUNNING: &[u64] = &[182_362_527, 182_362_531, 182_362_534];

/// Verified retail `SimCity.exe` SHA-256 fingerprint (`10.3.6.0`).
pub const RETAIL_SIMCITY_EXE_SHA256: &str =
    "4e5f136a7bf58a52c4b97ed9c9a615a118d4a696e13aa8caa6a0a51fd4516d35";

/// Verified retail `EAWebKit.dll` (`12.4.2.2`) SHA-256 fingerprint (`9,608,808` bytes, PE32 x86).
pub const RETAIL_EAWEBKIT_DLL_SHA256: &str =
    "72d76cbff60aac162ee80845ebb7a6957c22f2ccefaa43ede235ed633f212146";

/// Retail `SimCity.exe` virtual address (`0x00921115`) where the native crash handler pushes `"DbgHelp.dll"`
/// before calling `LoadLibraryA` and `MiniDumpWriteDump` (return address `0x00921122`).
pub const RETAIL_MINIDUMP_HANDLER_VA: u32 = 0x0092_1115;

/// Retail `SimCity.exe` return virtual address (`0x00921122`) inside the native minidump writer path.
pub const RETAIL_MINIDUMP_RETURN_VA: u32 = 0x0092_1122;

/// Retail `EAWebKit.dll` RVA (`0x005AC490`) of `WebCore::ChromeClientEA::addMessageToConsole`.
///
/// Runtime hooking uses String-XREF dynamic resolution (`eawebkit_debuglog_resolver`) rather than
/// relying on this hardcoded RVA.
pub const EAWEBKIT_ADD_MESSAGE_TO_CONSOLE_RVA: u32 = 0x005A_C490;

/// Retail `EAWebKit.dll` RVA (`0x005AC560`) of the verified console message formatting/dispatch window.
pub const EAWEBKIT_CONSOLE_DISPATCH_WINDOW_RVA: u32 = 0x005A_C560;

/// Vtable byte offset (`+0x0C`, slot 3) of `EAWebKitClient::DebugLog(DebugLogInfo&)`.
pub const EAWEBKIT_CLIENT_DEBUGLOG_VTABLE_OFFSET: u32 = 0x0C;

/// Retail `SimCity.exe` RVA (`0x00964D70`, VA `0x00D64D70`) of the host `EAWebKitClient` vtable.
///
/// Runtime hooking resolves `gEAWebKitClient` dynamically via String-XREF (`eawebkit_debuglog_resolver`)
/// rather than hardcoding this RVA.
pub const RETAIL_EXE_EAWEBKIT_CLIENT_VTABLE_RVA: u32 = 0x0096_4D70;

/// Retail `SimCity.exe` RVA (`0x0066BC90`, VA `0x00A6BC90`) of the shared `C2 04 00` (`ret 4`) no-op stub.
///
/// Must NEVER be hooked directly via code detour because unrelated callbacks (`GetLocalizedString`,
/// `NetworkError`, `CursorMoved`, `ViewProcessStatus`, `JSMessageBox`) share this exact address.
pub const RETAIL_EXE_SHARED_RET4_NOOP_RVA: u32 = 0x0066_BC90;

/// Retail `SimCity.exe` virtual address (`0x008EB580`) of `EA::Trace::TraceHelper::TraceFormatted`.
pub const RETAIL_EATRACE_TRACE_FORMATTED_VA: u32 = 0x008E_B580;

/// Retail `SimCity.exe` virtual address (`0x00917380`) of `EA::Trace::Server::Trace`.
pub const RETAIL_EATRACE_SERVER_TRACE_VA: u32 = 0x0091_7380;

/// Retail `SimCity.exe` virtual address (`0x0091C040`) of `anonymous::cLogReporterFile::Report`.
pub const RETAIL_EATRACE_FILE_REPORTER_VA: u32 = 0x0091_C040;

/// Retail `SimCity.exe` virtual address (`0x0046BE30`) of `anonymous::cLogReporterDebugConsole::Report`.
pub const RETAIL_EATRACE_CONSOLE_REPORTER_VA: u32 = 0x0046_BE30;

/// Retail `SimCity.exe` virtual address (`0x0046B9E0`) of `SP::cCheatManager::Output`.
pub const RETAIL_CHEAT_MANAGER_OUTPUT_VA: u32 = 0x0046_B9E0;

/// Retail `SimCity.exe` virtual address (`0x0091BAB0`) of `EA::Trace::GetServer`.
pub const RETAIL_EATRACE_GET_SERVER_VA: u32 = 0x0091_BAB0;

/// Retail `SimCity.exe` virtual address (`0x00D1B708`) of the `cEcoGameScripts` vtable.
pub const RETAIL_ECOGAME_SCRIPTS_VTABLE_VA: u32 = 0x00D1_B708;

/// Retail `SimCity.exe` virtual address (`0x005CFB00`) of `cEcoGame::LogAction`.
pub const RETAIL_ECOGAME_LOG_ACTION_VA: u32 = 0x005C_FB00;

/// Retail `SimCity.exe` virtual address (`0x005CFE20`) of `cEcoGame::LogGlobalRule` (`action 5`).
pub const RETAIL_ECOGAME_LOG_GLOBAL_RULE_VA: u32 = 0x005C_FE20;

/// Retail `SimCity.exe` virtual address (`0x005D47F0`) of `cEcoGame::LogUnitRule` (`action 2` + `action 1`).
pub const RETAIL_ECOGAME_LOG_UNIT_RULE_VA: u32 = 0x005D_47F0;

/// Retail `SimCity.exe` virtual address (`0x005F76B0`) of `cEcoGameScripts::UnitRuleFromIndex` (vtable `+0xD0`).
pub const RETAIL_ECOGAME_UNIT_RULE_FROM_INDEX_VA: u32 = 0x005F_76B0;

/// Retail `SimCity.exe` virtual address (`0x005F6AE0`) of `cEcoGameScripts::GlobalRuleFromIndex` (vtable `+0xD8`).
pub const RETAIL_ECOGAME_GLOBAL_RULE_FROM_INDEX_VA: u32 = 0x005F_6AE0;

/// Offset (`+0x17A0`) inside `cEcoGame` storing total simulation ticks participating in rule logging.
pub const ECOGAME_TOTAL_TICKS_OFFSET: u32 = 0x17A0;

/// Offset (`+0x1908`) inside `cEcoGame` caching the currently active logged `unitSlot` (`action 2`).
pub const ECOGAME_CURRENT_LOG_UNIT_SLOT_OFFSET: u32 = 0x1908;

/// Stream version threshold (`27` / `0x1B`) where `DecodeActionHeader` (`0x5CC1D0`) switches from legacy
/// `0x3E`/`0x3F` data2 markers to retail `0xFE`/`0xFF` markers.
pub const RULE_STREAM_RETAIL_VERSION_THRESHOLD: u32 = 27;

/// Retail `SLDelta` file outer container version (`4`, little-endian `u32` at offset `0`).
pub const SLDELTA_RETAIL_CONTAINER_VERSION: u32 = 4;

/// Retail `SLDelta` record header magic (`0xFF020300`, big-endian `u32` at each record boundary).
pub const SLDELTA_RECORD_MAGIC_BE: u32 = 0xFF02_0300;
