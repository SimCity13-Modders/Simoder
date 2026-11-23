# Repository ownership

This repository is the Rust runtime for SimCity (2013). The current source and
release workflows are authoritative for implemented behavior; proposal and
qualification documents elsewhere in `docs/architecture/` do not imply that a
feature is active.

| Repository | Current responsibility |
| --- | --- |
| [Simoder](https://github.com/SimCity13-Modders/Simoder) | Rust Cargo workspace and integrated x86 `d3d9.dll`; owns bootstrap, game-facing validation, mod/scripting runtimes, diagnostics, and the runtime APIs exposed to Base. |
| [Simoder-Base](https://github.com/SimCity13-Modders/Simoder-Base) | Luau base mod: lifecycle and semantic services, UI composition, and mod-facing abstractions. Simoder's CI checks out a pinned commit for integration tests. |
| [Simoder-OpenSCP](https://github.com/FluffyChi-Xing/fluffy-open-scp) | External DBPF/property/resource codecs consumed by the Rust workspace through Cargo revision pins. |
| Simoder-DevTools repositories | Separate optional authoring and diagnostics tools; these are not required to build or install the runtime DLL. |

Simoder and Simoder-Base are separate GitHub repositories under the
`SimCity13-Modders` organization. Their histories and release cycles are
independent. The alpha runtime does not promise compatibility with earlier
prototype formats; the first Beta release is intended to establish the
compatibility baseline. See the [compatibility policy](compatibility.md).

Separate repositories and passing CI do not establish live-game compatibility,
native-mod safety, or public Beta readiness. These require their own evidence
and acceptance.
