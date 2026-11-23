# Simoder Rust Definition of Done (DoD)

Extends the base workspace standard ([definition-of-done.md](../../../.agents/plugins/simcity-ecosystem/skills/simoder-audit/references/definition-of-done.md)) for all crates inside `Simoder/crates/*`.

---

## Rust `MUST` Checklist (Blocking)

1. **Edition, Unsafe Policy & Runtime Panic Hygiene**
   - Target Rust `edition = "2024"`.
   - Enforce `#![forbid(unsafe_code)]` at crate root by default; only `simoder-d3d9` (Win32 DLL / COM / IAT/detour FFI boundaries) may isolate `unsafe` to minimal audited submodules with an explicit `// SAFETY:` invariant block immediately above every `unsafe` block/fn.
   - Zero `.unwrap()` / `.expect()` on untrusted, I/O, FFI, or user/mod-provided inputs inside `src/` (only test code, unreachable verified compile-time constants, or post-checked bounds with clear justification).

2. **Reuse (`Simoder-OpenSCP` + `crates.io` Catalog) & DRY**
   - Never reimplement parsers, path sanitizers, locale tags, synchronization wrappers, or arena keys already covered by `simoder-core`, `simoder-formats`, `Simoder-OpenSCP` (`PropertyFile`, `Rw4File::parse`, `LocaleItem`, etc.), or [rust-crate-catalog.md](../../../.agents/plugins/simcity-ecosystem/skills/simoder-implementation/references/rust-crate-catalog.md).
   - Unify parallel engines or backends behind shared wrappers/enums/helpers (`ScriptVm`, `LuauResultExt`/`JsResultExt`, centralized lock helpers via `parking_lot::Mutex`) rather than duplicating logic.
   - Enforce known crate safety invariants (`rquickjs` without `rust-alloc`/`allocator`, `set_memory_limit` outside `context.with`, x86 `bindgen`, and `mlua` `SerializeOptions` preserving Lua `nil`).

3. **Balanced Modularity, SOLID & Strong Domain Types**
   - Respect the anti-over-modularization rule: no tiny speculative files (< ~150 lines without a domain boundary like `config.rs`/`lib.rs`) and no single-impl traits without an active test/host boundary (`PropObserver`, `SchedulerTickSink`).
   - Split multi-domain modules or test suites exceeding ~500–600 lines along cohesive domain seams (`manifest/{mod,authoring,raw}.rs`, `vm/cross_vm/{mod,proxy}.rs`, `tests/common/mod.rs` + themed `tests/*.rs`).
   - Model finite state machines and variants as single-source `enum`s (`strum` where serialized/parsed) rather than coupled `Option<T>` flags (`make illegal states unrepresentable`).

4. **Contract-Respecting YAGNI & Distribution Boundary**
   - Remove genuinely dead functions, unused fields, unreferenced dependencies, and redundant legacy compatibility shims while preserving public APIs required by approved `docs/architecture/*.md` contracts and roadmap stages.
   - Production boundary intact: `Simoder` produces one integrated x86 `d3d9.dll` beside `SimCity.exe`; zero MCP server, CLI tools, or example mods inside `Simoder`.

5. **Rustdoc & Complete Verification Gate (`--offline`)**
   - Public modules, structs, enums, traits, functions, and errors documented with idiomatic Rustdoc (`//!` and `///` using `[`Type`]` intra-doc links); zero Doxygen tags or references to legacy `.cpp`/`.hpp`/`.py` helpers; zero `TODO`/`FIXME` placeholders.
   - All commands below MUST pass cleanly *after the final edit*:
     - `cargo test --workspace --offline`
     - `cargo check --workspace --target i686-pc-windows-msvc --offline` (plus `cargo test -p <crate> --target i686-pc-windows-msvc --offline` for runtime/FFI/VM crates)
     - `cargo doc --workspace --no-deps --offline` (0 warnings / 0 broken intra-doc links)

---

## `SHOULD` (Non-Blocking) & Review Verdict

- Stylistic cleanups, minor combinator rewrites, or opportunistic refactors below `MUST` threshold are logged as `SHOULD` only and **must not** fail an audit (`PASS`) or trigger unapproved code churn ("Good Enough" Stop-Line).
- Every review verdict explicitly states per-crate **`PASS`** or **`FAIL`**, citing `MUST` blockers vs `SHOULD` suggestions with exact `file:line` references.
