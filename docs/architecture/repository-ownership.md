# Repository ownership and extraction status

The accepted architecture separates runtime, high-level API, developer mods,
authoring tools, SDK declarations, examples and workspace composition. A folder
rename does not establish an independent repository or compatible package.

| Repository | Responsibility and current extraction status |
| --- | --- |
| Simoder | Existing single x86 runtime DLL, native game adapters and enforced low-level primitives. Internal implementation headers are not a stable native-mod ABI. |
| Simoder-OpenSCP | Separate upstream-compliant application/ModStudio fork; excluded from the current cleanup. |
| Simoder-Base | Entirely Luau, project-authored public lifecycle/UI/API abstractions; not yet extracted as an implemented standalone package. |
| Simoder-DevTools-MCP | Optional native MCP mod target. Current coupled C# executables remain legacy migration input, not a completed native package. |
| Simoder-DevTools-Authoring | Authoring/compiler coordination and editor integration; existing host implementation still in original DevTools pending extraction. |
| Simoder-DevTools-Diagnostics | Diagnostic mod target. Existing host utilities require classification; EA launcher compatibility stays host tooling, not runtime-mod responsibility. |
| Simoder-SDK | Existing Luau and legacy TS declarations now have a separate canonical repository and versioned manifest. Hash-verified vendored compatibility artifacts preserve current editor/compiler paths. Public native ABI remains unimplemented; broad Simoder::Core implementation is not this SDK. |
| Simoder-Mod-Examples | First physical extraction: four existing example projects, new local Git history, independent source/package tests and original notices. No RoadBuilder/BusLine implementation is claimed. |
| Simoder-Mod-Templates | Separate simple cloneable templates; existing examples are not silently relabeled templates. |
| Simoder-Engineering | Shared engineering policy/schema/composition/conformance responsibility, not duplicated into every product. |
| Simoder-Workspace | Planned Rust workspace CLI consuming Engineering; no new CLI is introduced by cleanup. |
| Simoder-Adapter-Codex / Simoder-Adapter-Claude | Provider-specific integration responsibility, separate from product runtime. |

Original DevTools history and published `dirty` checkpoints remain recoverable
reference. Extracted components start new histories. Active DevTools consumes the
example package only through an explicit prepared checkout pinned by
`cmake/simoder-examples.lock.cmake`; no implicit sibling lookup or fetch. Omitting
that optional dependency omits examples from the DevTools assembly rather than
copying a second authoritative source tree.

DevTools and Examples retain exact SDK-derived declaration artifacts for offline
source-checkout editor/compiler compatibility. Their SDK locks identify one
canonical revision and payload hashes; these copies must not evolve separately.
DevTools validates vendored hashes during CMake configuration and optionally
checks an explicit clean canonical SDK checkout. Examples runs the equivalent
provenance check in its package tests. This is prepared-source integrity, not
signed publisher trust. SDK refreshes require compatibility regression and audit.

Package/source extraction does not prove game compatibility, native mod loading,
HTTP/TLS interoperability or public release readiness. Further cutovers need
dependency closure, regression/parity evidence and independent audit. Game data,
private converted projects and investigation evidence must not enter examples,
templates or SDK packages. The original mod notices remain required under MIT.
