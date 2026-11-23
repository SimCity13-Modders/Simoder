# ADR-0002: JavaScript Runtime Selection (`rquickjs` / QuickJS-NG)

- **Status:** Accepted
- **Date:** 2026-11-21
- **Tracks:** `MiamiCenter/Simoder#2` (JS Runtime Selection & PoC Qualification), `MiamiCenter/Simoder#10`

## Context

`simoder-scripting` must host sandboxed ECMAScript mod scripts alongside Luau (`mlua`) inside the 32-bit x86 (`i686-pc-windows-msvc`) `d3d9.dll` payload while preserving:

- Strict per-mod VM isolation (separate heap, global scope, and CommonJS module cache per mod generation).
- Hard C-level per-VM heap caps (`2 MiB..=64 MiB`), an aggregate `128 MiB` host pool ceiling, bounded call stack (`>= 64 KiB`), and sub-frame monotonic wall-clock interrupt enforcement (`1..=20 ms`).
- `#![forbid(unsafe_code)]` inside `simoder-scripting`, frozen read-only `simoder` host tables/objects, and clean lifecycle parity (`onEnable`, `onTick`, read-only `onDisable`, transactional PROP rollback on callback failure).

## Decision

Adopt **`rquickjs 0.14.0`** (wrapping **QuickJS-NG** via `rquickjs-sys 0.14.0` with MSVC compilation) as the embedded JavaScript engine backend in [`crate::vm::js`](../../../crates/simoder-scripting/src/vm/js.rs), configured in workspace manifests as:

- `rquickjs = { version = "0.14", default-features = false, features = ["std", "classes", "parallel"] }`
- Target-specific `i686-pc-windows-msvc` override: `rquickjs = { workspace = true, features = ["bindgen"] }`

Key configuration invariants verified during integration:

1. **Do NOT enable `rust-alloc` or `allocator` features on `rquickjs`**: doing so turns `Runtime::set_memory_limit` into a no-op and defeats hard per-VM OOM protection.
2. **Call `runtime.set_memory_limit(...)` outside `context.with(...)`**: under `rquickjs`'s `"parallel"` feature, both `Context::with` and `Runtime::set_memory_limit` lock the underlying `Runtime` mutex; calling `set_memory_limit` inside `context.with` self-deadlocks.
3. **32-bit Windows (`i686-pc-windows-msvc`) bindings**: `rquickjs-sys v0.14.0` omits prebuilt `i686-pc-windows-msvc` bindings on disk, so the target-gated `bindgen` feature generates x86 bindings cleanly during build via Visual Studio LLVM (`libclang.dll`).

## Candidates Evaluated

| Candidate | Isolation & Hard Caps | x86 Integration & Footprint | Verdict |
| --- | --- | --- | --- |
| **`rquickjs 0.14.0` (QuickJS-NG)** | Native `JS_SetMemoryLimit`, `JS_SetMaxStackSize`, `JS_SetInterruptHandler`, per-VM `Runtime` | Compiles cleanly with MSVC + `bindgen` on `i686-pc-windows-msvc`; pre-LTO `quickjs.lib` = `1,917,336` bytes | **Accepted** |
| **`boa_engine`** | Pure-Rust VM without drop-in C `JS_SetMemoryLimit` / `JS_SetInterruptHandler` parity | Larger pure-Rust interpreter/compiler dependency tree | Rejected |
| **Retail UI JS (`SimCity.exe`)** | Shared UI context tied to `window.Client` (`0x899EC0` / `0x8C73F0`); zero mod isolation | Unavailable outside full UI host and unsafe for per-mod reload/teardown | Rejected |
| **V8 (`rusty_v8`)** | Full JIT/isolate runtime | Excessive binary size, GN/ninja toolchain weight, and 32-bit x86 address-space pressure inside `d3d9.dll` | Rejected |

## Observed Measurements (`i686-pc-windows-msvc`, Release Artifacts Before Final DLL Link)

- `rquickjs-sys v0.14.0` static archive (`quickjs.lib`): **`1,917,336` bytes (~1.83 MiB)**
- `simoder-scripting` combined release crate (`libsimoder_scripting.rlib`, pre-LTO strip): **`6,634,226` bytes (~6.33 MiB)**
- Cross-target verification (`--offline`):
  - `cargo test --workspace` (`x86_64-pc-windows-msvc`): **51/51 passed** (`simoder-core`: 8, `simoder-crash`: 2, `simoder-d3d9`: 7, `simoder-formats`: 7, `simoder-mods`: 7, `simoder-scripting`: 20).
  - `cargo test -p simoder-scripting --target i686-pc-windows-msvc`: **20/20 passed**.
  - `cargo check --workspace --target i686-pc-windows-msvc` and `cargo doc --workspace --no-deps`: exited **0** with **0** warnings.

> Note: Full `simoder-d3d9` final stripped `d3d9.dll` size impact with `[profile.release]` (`lto = true`, `strip = true`) has not yet been linked in this stage.

## Repeatable PoC Qualification Suite

All PoC assertions live in [`crates/simoder-scripting/tests/vm_js.rs`](../../../crates/simoder-scripting/tests/vm_js.rs) and [`crates/simoder-scripting/tests/beta_extensions.rs`](../../../crates/simoder-scripting/tests/beta_extensions.rs):

- `js_vm_lifecycle_isolation_and_frozen_host_api`: verifies cross-VM global isolation (`globalThis` non-leakage across two JS VMs), frozen `simoder` API tamper trapping in strict mode, relative CommonJS `require`, and cyclic import rejection.
- `js_vm_props_observations_rollback_and_on_disable_guard`: verifies frozen `getFloat` snapshots, transactional `setFloat` batching, rollback on JS callback exception, and write blocking during `onDisable`.
- `js_vm_enforces_memory_cap_stack_limit_and_interrupt_budget`: verifies `2 MiB` heap limit enforcement via `Uint8Array` allocations (`MemoryLimitExceeded`), call-stack exhaustion guard (`InvocationFailed`), and `1 ms` interrupt timeout (`BudgetExceeded`).
- `scheduler_runs_mixed_luau_and_js_vms_with_fault_isolation_and_128_mib_pool`: verifies mixed Luau + QuickJS round-robin scheduling, fault isolation, and `128 MiB` host memory pool accounting.

## Consequences & Re-evaluation Criteria

- **Toolchain requirement on x86 builds**: building `i686-pc-windows-msvc` requires `libclang.dll` (provided by Visual Studio LLVM tools) until upstream `rquickjs-sys` ships `i686-pc-windows-msvc` bindings or Simoder checks in generated bindings.
- **Re-evaluate if**:
  1. CI/contributor Windows environments fail x86 builds due to missing `libclang.dll`.
  2. Final linked `d3d9.dll` size under `strip = true`, `lto = true` exceeds release footprint budgets.
  3. Cross-VM synchronous `CrossVmHookHub` proxy marshalling (currently exercised across isolated Luau VMs in [`crates/simoder-scripting/tests/cross_vm_hooks.rs`](../../../crates/simoder-scripting/tests/cross_vm_hooks.rs)) must add direct JS object `Proxy` / `UserData` interop when exposing cross-language JS<->Luau hooks.
