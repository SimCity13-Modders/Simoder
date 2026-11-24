# Simoder

[![Download Latest (Nightly)](https://img.shields.io/github/actions/workflow/status/SimCity13-Modders/Simoder/ci.yml?branch=main&label=Download%20latest%20(nightly)&logo=githubactions&logoColor=white)](https://nightly.link/SimCity13-Modders/Simoder/workflows/ci/main/simoder-windows-x86.zip)
[![Download Latest Release](https://img.shields.io/github/v/release/SimCity13-Modders/Simoder?label=Download%20latest%20release&logo=github)](https://github.com/SimCity13-Modders/Simoder/releases/latest)
[![Star](https://img.shields.io/github/stars/SimCity13-Modders/Simoder?style=social)](https://github.com/SimCity13-Modders/Simoder/stargazers)

**Simoder** is an in-game mod loader, runtime host, and declarative UI composition platform for **SimCity (2013)** and **Cities of Tomorrow**, written in Rust (`edition = "2024"`). Its production distribution is a single integrated 32-bit x86 `d3d9.dll` proxy placed beside `SimCity.exe` — forwarding Direct3D 9 calls to Windows and initializing Simoder safely after graphics creation, outside `DllMain`, after validating the exact PE32 retail game build.

## Dependencies & Requirements

- **Game**: SimCity (2013) / Cities of Tomorrow (`10.3.6.0`, 32-bit x86 Windows retail build)
- **Required Base Mod**: [Simoder-Base](https://github.com/SimCity13-Modders/Simoder-Base) (`mods/simoder.base`) — pure-Luau semantic layer & in-game `Mods` UI (required for Mods UI and `ABI v2` native module readiness)
- **Building from source (optional)**: Rust toolchain with `x86_64-pc-windows-msvc` (host tests) and `i686-pc-windows-msvc` (32-bit `d3d9.dll` runtime target)

## Installation

1. Close SimCity completely.
2. Download `d3d9.dll` from the **Release** or **Nightly** badge above (or build `target/i686-pc-windows-msvc/release/simoder_d3d9.dll` and rename it to `d3d9.dll`).
3. Copy `d3d9.dll` next to `SimCity.exe` (`<SimCity Install>\SimCity\d3d9.dll`).
4. Install [Simoder-Base](https://github.com/SimCity13-Modders/Simoder-Base) into `<SimCity Install>\mods\simoder.base\`.
5. Place any user mods into `<SimCity Install>\mods\<mod-folder>\` and launch SimCity normally.

```text
<SimCity Install>/
  SimCity/SimCity.exe
  SimCity/d3d9.dll            # Simoder integrated x86 proxy runtime
  mods/simoder.base/          # Required Simoder-Base Luau UI & services
  mods/<user-mod>/mod.toml    # Installed user mods
  simoder/config.toon         # Generated runtime settings
  simoder/state.toon          # Generated mod activation order
  simoder/native_trust.toon   # SHA-256 trust decisions for native DLL / dev_ui mods
  simoder/logs/               # Diagnostic logs
  simoder/crashes/            # Redacted crash reports
```

---

## Crate Architecture

| Crate | Responsibility |
| --- | --- |
| `simoder-core` | Core identifiers (`Tgi`), GlassBox FNV-1 hashing, IDA byte-signature scanner, runtime PROP table patching, producer-side path redaction, and the bounded `DiagnosticLogPipeline`. |
| `simoder-formats` | Binary and text codecs reusing `Simoder-OpenSCP` (`dbpf`, `sc-properties`, `rw4`, `sc-format`), `serde_toon` (`config.toon` / `state.toon`), and `oxilangtag` BCP-47 locale resolution. |
| `simoder-mods` | Mod discovery, Schema 2 (`simoder.authoring`) and Schema 3 (`simoder.raw-resources`) validation, `PatchRegistry`, DBPF overlay packaging, `ABI v2` `NativeModuleHost`, `HostToolRegistry`, `KeybindRegistry`, and `NativeTrustStore`. |
| `simoder-scripting` | Dual-engine `ScriptVm` (`Luau` via `mlua` and `JavaScript` ES2023 via `rquickjs`), `ScriptScheduler`, `PreHook`/`PostHook`/`OverrideFunction`, cross-VM proxies, `DelayedCalls`, and `Utils`. |
| `simoder-crash` | Controlled Beta crash handler, SHA-256 build-verified stack symbolication, Win32 crash report dialog, 14-channel Backend <-> Frontend `EAWebKit` bridge observer (`BridgeTime`), and `SLDelta` v4 / `cEcoGame` rule telemetry. |
| `simoder-d3d9` | 32-bit x86 `d3d9.dll` proxy export table (`ordinals 16..=38`), PE32 retail build verifier (`goblin`), COM vftable/shader validation, Win32 UI dispatch & `EAWebKit` bridge, `Simoder-Base` host, and `IntegratedSimoderRuntime`. |

## Build & Verification

Simoder targets Rust `edition = "2024"` and verifies both 64-bit host tests and the 32-bit Windows game target:

```powershell
cargo clippy --workspace --all-targets -- -D warnings
cargo clippy --workspace --all-targets --target i686-pc-windows-msvc -- -D warnings
cargo test --workspace
cargo test --workspace --target i686-pc-windows-msvc
cargo doc --workspace --no-deps
cargo build --release --target i686-pc-windows-msvc -p simoder-d3d9
```

## Ecosystem & Repository Policy

- **[Simoder-Base](https://github.com/SimCity13-Modders/Simoder-Base)** — Required pure-Luau semantic layer and in-game `Mods` / `Keybinds` UI.
- **[Simoder-SDK](https://github.com/SimCity13-Modders/Simoder-SDK)** — Canonical Luau, TypeScript/JS, and C/C++ `ABI v2` declarations.
- **[Simoder-MCP](https://github.com/SimCity13-Modders/Simoder-MCP)** — Optional native `ABI v2` MCP DevTool mod (`simoder_mcp.dll`) for AI-assisted live inspection and automation.
- **[Simoder-DevTools](https://github.com/SimCity13-Modders/Simoder-DevTools)** — Offline workspace CLI (`simoder-workspace`) and packager tools.
- **[Simoder-Mod-Examples](https://github.com/SimCity13-Modders/Simoder-Mod-Examples)** — Reference example mods for Luau, Base API, PROP, and TOML.

Authored Rust code enforces `#![forbid(unsafe_code)]` outside minimal audited Win32 FFI boundaries and follows [docs/ai/definition-of-done.md](docs/ai/definition-of-done.md). Original source is licensed under [MIT](LICENSE); third-party dependencies retain their notices in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
