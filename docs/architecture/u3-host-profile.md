# U3 Base Luau & TypeScript/JS host profile

> **Historical qualification snapshot (2026-11-21).** The statements and repository
> revisions below describe the evidence assembled on that date. They are not
> a verification report for the current `main` branch, current Alpha artifact,
> or current organization repositories. See the current [README](../../README.md)
> and [CI/release workflow](../releases.md).

Status on 2026-11-21: Pure-Rust host runtime migration (Etap A) and pure-Luau `Simoder-Base` / `Simoder-SDK` / `Simoder-DevTools-Authoring` / `Simoder-DevTools` (Etap B) were reported complete and verified offline on both `x86_64-pc-windows-msvc` and production `i686-pc-windows-msvc`. All prior C++23, CMake, Python, and .NET 8 legacy trees were reported retired under the **Strictly NO Legacy** contract ([release hygiene](release-hygiene.md)).
This profile covers **Simoder SDK 0.2.0** (`base-luau-alpha-v1`, `base-ts-beta-v1`, `native-abi-v2`, Catalogue Contract `2`) as recorded on that date. Explicit human whole-U3 sign-off and live U6 game qualification were distinct acceptance gates at review time.

## Historical contracts and ownership

The canonical SDK declarations and Base service/DAG contracts lived in their
separate repositories. The paths and pins below are historical and do not
describe current checkouts.
`Simoder` (`crates/simoder-d3d9`, `simoder-core`, `simoder-formats`, `simoder-mods`, `simoder-scripting`, `simoder-crash`) owns native identity, the single `RuntimeScope` evaluator/task record, copied-value transport, dual `Luau` + `QuickJS` scheduling, and crash/bridge telemetry.
At profile time, DevTools owned offline workspace assembly and packaging, the
Authoring repository owned `simoder-build`, and a separate MCP repository owned
an optional native `simoder_mcp.dll` mod.
The production runtime remains one integrated x86 `d3d9.dll` beside `SimCity.exe` ([repository ownership](repository-ownership.md)).

## Supported presentation

The verified Base factory creates one frozen mod-local facade with seven keys. All public functions use dot calls (Luau) or standard object methods (TS/JS); the SDK remains the detailed reference.

| Key | Supported behavior |
| --- | --- |
| `permissions` | Read-only capability inspection via `query(name)` returning a frozen `{ available, selected, granted, effective, restricted }` view or `"UnsupportedCapability"` / sanitized denial without acting as a reusable authorization token. |
| `values` | Native strict copied-value validation and canonical wire encoding through `validate` and `wire`. Values are immutable original-VM userdata; serialization grants no authority. |
| `request` | Queue one native-owned task through the actual Base dispatcher; return a frozen task or `nil, code`. Admission is distinct from completion and commitment. |
| `run` | Admit a host-registered coroutine on a later script-lane turn; return `true` or `nil, code`. A raw coroutine does not acquire registered authority. |
| `snapshot` | Read a frozen lifecycle observation. Epochs, generations, revisions, sequence, time and context identities use exact decimal strings; observations are not permission leases. |
| `subscribe` | Atomically capture the current observation and ordered subsequent changes. Return a frozen unsubscribe handle or `nil, code`; overflow is explicit `MailboxOverflow`, and unsubscribe returns a native status. |
| `descriptors` | Read frozen reviewed metadata (`catalogueContract = 2`), possibly empty, or `nil, code`. Discovery does not authorize execution. |

A host may expose no `base`; preparation may expose the facade while operations return `NotReady` and restricted-mode `permissions.query` reports `effective = false, restricted = true`. Invalid callbacks, ownership violations, and invalid await boundaries throw; exceptions are distinct from `nil, code` returns.
Task outcomes and the `committed` flag are independent. Cancellation after commitment returns `Busy` and cannot undo effects. Callback and registered `await` delivery occur on the original lane, with access rechecked and stopped-generation continuations suppressed.

Each native endpoint binds once to its real VM generation and original scope. Caller-supplied mod IDs cannot change that identity. Runtime/Base epochs, factual context leases, and owner generations remain native. Separate VM globals never share mutable Luau/JS objects, closures, or raw native pointers; native DLL mods remain a distinct trust category (`ABI v2`) from script isolation.

## Pinned revisions and release artifacts

| Repository / Artifact | Branch / Target | Exact Revision / SHA-256 Digest & Size |
| --- | --- | --- |
| Historical Simoder source | `u4-u5-development` | `88a45df1c7a1248d67c2f0691b4350e301d1e14b` |
| `SimCity13-Modders/Simoder-Base` | `u4-u5-development` | `fe10140311a653984c689fa652b2a0a09c7c265a` |
| Historical Simoder-SDK source | `u4-u5-development` | `cc0394578e7ecc45736e2e8980f095d58879b0d2` |
| Historical Simoder-DevTools-Authoring source | `main` | `c924801ed8c8afa59da46a9e3937e41c2be78fad` |
| Historical Simoder-DevTools source | `refactor/repository-cleanup` | `7d6aa1f2dcefc4eefa387b545d16674d15e3a840` |
| Historical Simoder-DevTools-MCP source | `u4-u5-development` | `860e51882314e9bee068f1e0bcfb12ccc650553c` |
| `simoder_d3d9.dll` (production `d3d9.dll`) | `i686-pc-windows-msvc --release` | `1,113,088 B` — `264429d74ae56ea399932d783467659ae2c0dd29fb20902155acfc373a4f9b08` |
| `simoder_mcp.dll` (optional `ABI v2` DevTool mod) | `i686-pc-windows-msvc --release` | `120,320 B` — `a1456843a914cd3d95d054c3ed47c807051b4983bfa4686b432e3060d7d884c8` |

## Qualification and remaining verification

| Verification area | Current result and scope |
| --- | --- |
| `Simoder` Rust workspace (`simoder-core`, `formats`, `mods`, `scripting`, `crash`, `d3d9`) | **PASS**: `cargo clippy --workspace --all-targets -D warnings`, `cargo test`, `cargo doc -D warnings`, `missing_docs` on `x86_64` and `i686-pc-windows-msvc`, plus integrated `d3d9_tests` uniting all subsystems. |
| `Simoder-DevTools-MCP` (`simoder_mcp.dll` `ABI v2` native mod) | **PASS**: `cargo clippy -D warnings`, `cargo test`, `cargo doc -D warnings`, `missing_docs` on `x86_64` and `i686`, plus native 32-bit DLL build (`120,320 B`). |
| `Simoder-Base` pure-Luau DAG, `catalogueContract = 2`, `observeSession`, and `permissions.query` | **PASS**: `tests/runner.luau` and `tests/services.luau` executed inside real Luau 0.654 VM via `Simoder-DevTools` `base_tests` (`3/3` PASS on `x64` and `x86`); deterministic USTAR packaging + `SelectedBasePins` tamper checks verified. |
| `Simoder-SDK` Beta matrix & automated `Simoder-Base` conformance | **PASS**: `sdk_tests` (`3/3` PASS on `x64` and `x86`) confirms zero missing/surplus declarations for both `Luau/base.d.luau` (`base-luau-alpha-v1`) and `TypeScript/base.d.ts` (`base-ts-beta-v1`), plus `Native/identity.json` SHA-256 (`00c98e36f669c6c4f16f7ffd9073130ae9798543b3d0759d7c1937f443e6e236`). |
| `Simoder-DevTools-Authoring` (`simoder-build` + offline `TS -> JS`) | **PASS**: `authoring_tests` (`4/4` PASS on `x64` and `x86`) and `node --test compiler.test.mjs` (`3/3` PASS, `ModuleResolutionKind.Bundler`, no suppressed deprecations), starter templates, and `node_modules` exclusion on staging. |
| `Simoder-DevTools` (`simoder-workspace` offline CLI) | **PASS**: `workspace_tests` (`3/3` PASS on `x64` and `x86`), offline revision/SHA-256 recording, clean-clone determinism, and game-root write refusal. |
| Declaration IDE analysis (`Luau.Analysis`) | **NOT_RUN**: grammar parsing and runtime VM conformance pass; full IDE analyzer inference remains separate. |
| Regular-file symbolic links (`Win32 1314`) | **NOT_RUN / SOURCE_ONLY**: non-privileged environments lack `SeCreateSymbolicLinkPrivilege`; ordinary directory/reparse containment is enforced in Rust. |
| Game lifecycle qualification (`SimCity.exe`) | **PENDING_U6** (`gamePid = 0` at review time): live process/UI/reload/restart/crash/SP-backup-restore scenarios were not run. |
| Whole-U3 acceptance | **PENDING_OWNER_SIGNOFF**: the recorded technical artifacts, pins, and deviation dispositions awaited explicit owner acceptance. |

## Lifecycle and migration limits

1. Legacy debt retirement: The earlier C++23 `legacySaveEnabledState` path, TS-to-Luau transpiler, CMake/Python build scripts, and standalone .NET MCP/DevBridge executables documented in prior 2026-10-19 drafts have been completely removed under the **Strictly NO Legacy** cutover.
2. Cooperative native lifecycle (`E5-OWNER-CAPTURE-001`): Native `ABI v2` DLL images (`simoder_mcp.dll`) remain pinned until process exit via `BootstrapCoordinator` / `NativeModuleHost` without forced `FreeLibrary` unload while draining, matching the Windows `LdrUnloadDll` safety model.
3. Separate acceptance gates: Core semantic issuer, `ABI v2` MCP DevTool transport, and live `SimCity.exe` gameplay acceptance (`U6`) remain independent verification gates.
