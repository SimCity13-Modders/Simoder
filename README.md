# Simoder

Simoder is an in-game mod manager and runtime for SimCity (2013) and Cities of
Tomorrow written in Rust (`edition = "2024"`). Its production distribution is one
integrated 32-bit x86 `d3d9.dll` placed beside `SimCity.exe`. It forwards
Direct3D 9 calls to Windows and initializes Simoder after graphics creation,
outside `DllMain`. Exact PE32 game-build validation precedes game hooks.

## Crate Architecture

| Crate | Responsibility |
| --- | --- |
| `simoder-core` | Core identifiers (`Tgi`), GlassBox FNV-1 hashing, IDA byte-signature scanner, runtime PROP table patching, producer-side sensitive path redaction, and the bounded `2048`/`64` `DiagnosticLogPipeline`. |
| `simoder-formats` | Binary and text codecs reusing `Simoder-OpenSCP` (`dbpf`, `sc-properties`, `rw4`, `sc-format`), `serde_toon` (`config.toon` / `state.toon`), and `oxilangtag` BCP-47 locale fallback resolution. |
| `simoder-mods` | Mod discovery, Schema 2 (`simoder.authoring`) and Schema 3 (`simoder.raw-resources`) manifest validation, `PatchRegistry`, DBPF overlay packaging, `ABI v2` `NativeModuleHost`, decoupled `HostToolRegistry`, and `Simoder-OpenSCP` debug-tools overlay builder. |
| `simoder-scripting` | Dual-engine `ScriptVm` (`Luau` via `mlua` and `JavaScript` ES2023 via `rquickjs`), `ScriptScheduler`, `PreHook`/`PostHook`/`OverrideFunction`, cross-VM proxies, `DelayedCalls`, and `Utils`. |
| `simoder-crash` | Controlled Beta crash handler, SHA-256 build-verified stack symbolication, full-dump consent gating, 14-channel Backend <-> Frontend `EAWebKit` bridge observer (`BridgeTime`), and `SLDelta` v4 / `cEcoGame` rule telemetry. |
| `simoder-d3d9` | 32-bit x86 `d3d9.dll` proxy export table (`ordinals 16..=38`), PE32 retail build verifier (`goblin`), COM vftable/shader validation, single-owner bootstrap coordinator, and `IntegratedSimoderRuntime`. |

## Installation

Close SimCity and copy the built 32-bit `d3d9.dll` (`target/i686-pc-windows-msvc/release/simoder_d3d9.dll` renamed to `d3d9.dll`) beside `SimCity.exe` (`<installation>/SimCity/SimCity.exe`). Launch normally through EA App.

```text
<installation>/
  SimCity/SimCity.exe
  SimCity/d3d9.dll
  mods/<user-mod>/mod.toml
  simoder/config.toon         # generated settings
  simoder/state.toon          # generated activation order
  simoder/logs/               # diagnostics
  simoder/crashes/            # redacted crash reports
```

## Build & Verification

Simoder targets Rust `edition = "2024"` and compiles for both 64-bit host verification (`x86_64-pc-windows-msvc`) and the 32-bit game runtime target (`i686-pc-windows-msvc`). GitHub Actions checks out the pinned [Simoder-Base](https://github.com/SimCity13-Modders/Simoder-Base) source for integration tests. See [CI and releases](docs/releases.md):

```powershell
cargo clippy --workspace --all-targets -- -D warnings
cargo clippy --workspace --all-targets --target i686-pc-windows-msvc -- -D warnings
cargo test --workspace
cargo test --workspace --target i686-pc-windows-msvc
cargo build --release --target i686-pc-windows-msvc -p simoder-d3d9
```

## Repository Policy

The runtime lives at [SimCity13-Modders/Simoder](https://github.com/SimCity13-Modders/Simoder); its Luau base mod lives at [SimCity13-Modders/Simoder-Base](https://github.com/SimCity13-Modders/Simoder-Base). Optional authoring and developer tools are maintained separately and are not needed to build or install the runtime DLL. Audits, exploratory reports, screenshots, and raw investigation evidence belong in ignored `.analysis/`; maintained contracts and guides belong in `docs/`.

Authored Rust code uses idiomatic Rustdoc (`//!` and `///` with `[`Type`]` intra-doc links) and enforces `#![forbid(unsafe_code)]` across internal modules (see [Definition of Done](docs/ai/definition-of-done.md)). Original source is under the [MIT License](LICENSE); dependencies retain their [own notices](THIRD_PARTY_NOTICES.md).
