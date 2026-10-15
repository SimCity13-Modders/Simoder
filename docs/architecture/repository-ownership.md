# Repository ownership and extraction status

The accepted architecture separates runtime, high-level API, developer mods,
authoring tools, SDK declarations, examples and workspace composition. A folder
rename does not establish an independent repository or compatible package.

| Repository | Responsibility and current extraction status |
| --- | --- |
| Simoder | Existing single x86 runtime DLL, native game adapters and enforced low-level primitives. Internal implementation headers are not a stable native-mod ABI. |
| Simoder-OpenSCP | Separate upstream-compliant application/ModStudio fork; owns conversion and resource inspection. Its current branch/PR is excluded from this cleanup; ownership does not claim complete replacement feature parity. |
| Simoder-Base | Entirely Luau, project-authored public lifecycle/UI/API abstractions; not yet extracted as an implemented standalone package. |
| Simoder-DevTools-MCP | Separate source owner of the existing coherent C# MCP/DevBridge/Core stack and tests. DevTools consumes an explicit clean pinned checkout. Native MCP mod migration remains unimplemented; physical extraction does not establish the target lifecycle/transport/plugin architecture. |
| Simoder-DevTools-Authoring | Separate existing authoring compiler, TypeScript-to-Luau adapter, authoring tests and declarative snippets. DevTools consumes a clean pinned checkout; internal Core and prepared external compiler dependencies remain explicit. |
| Simoder-DevTools-Diagnostics | Diagnostic mod target; current MCP/REA paths provide existing diagnostic operations. Standalone PE/memory probe products are retired; no claim of complete MCP PE feature parity. |
| Simoder-DevTools-REA | Separate optional host wrapper, reviewed evidence schema and integration guide for externally installed REA. Provider-neutral source ownership only; not third-party REA itself, a provider-specific adapter, or the native Diagnostics mod. DevTools consumes a clean pinned checkout and preserves ordinary `Reverse/` installation. |
| Simoder-SDK | Existing Luau and legacy TS declarations now have a separate canonical repository and versioned manifest. Hash-verified vendored compatibility artifacts preserve current editor/compiler paths. Public native ABI remains unimplemented; broad Simoder::Core implementation is not this SDK. |
| Simoder-Mod-Examples | First physical extraction: four existing example projects, new local Git history, independent source/package tests and original notices. No RoadBuilder/BusLine implementation is claimed. |
| Simoder-Mod-Templates | Separate simple cloneable templates; existing examples are not silently relabeled templates. |
| Simoder-Engineering | Separate canonical source repository for seven shared engineering policy documents. Machine-readable schema, composition and conformance tooling remain planned; the documents do not implement those tools. |
| Simoder-Workspace | Planned Rust workspace CLI consuming Engineering; no new CLI is introduced by cleanup. |
| Simoder-Adapter-Codex | Separate canonical source repository for nineteen Codex files (eight skills, eight agent definitions and three routing/provider documents), plus seven read-only Engineering projections bound by an exact dependency lock. |
| Simoder-Adapter-Claude | Planned provider-specific integration; no implemented or qualified Claude adapter is claimed. |

Original DevTools history and published `dirty` checkpoints remain recoverable
reference. Extracted components start new histories. Active DevTools consumes the
example package only through an explicit prepared checkout pinned by
`cmake/simoder-examples.lock.cmake`; no implicit sibling lookup or fetch. Omitting
that optional dependency omits examples from the DevTools assembly rather than
copying a second authoritative source tree.

Authoring and Examples retain exact SDK-derived declaration artifacts for offline
source-checkout editor/compiler compatibility. Their SDK locks identify one
canonical revision and payload hashes; these copies must not evolve separately.
Authoring validates vendored hashes during CMake configuration and optionally
checks an explicit clean canonical SDK checkout. Examples runs the equivalent
provenance check in its package tests. This is prepared-source integrity, not
signed publisher trust. SDK refreshes require compatibility regression and audit.

Package/source extraction does not prove game compatibility, native mod loading,
HTTP/TLS interoperability or public release readiness. Further cutovers need
dependency closure, regression/parity evidence and independent audit. Game data,
private converted projects and investigation evidence must not enter examples,
templates or SDK packages. The original mod notices remain required under MIT.

The prototype/alpha does not require format/API migration support. The first
real Beta release establishes the backwards-compatibility baseline; see
[compatibility policy](compatibility.md). Retaining current input validation,
integrity and permission checks is independent of historical format support.

Engineering and Adapter-Codex compose 26 source files. Their initial inactive
snapshots and semantic correction ancestry remain historical provenance. The
reviewed 35-file activation in Simoder, including consumer-specific projections,
was explicitly accepted by the user. That acceptance does not activate another
consumer or approve future payload changes: each needs exact-content review and
explicit human acceptance. Source manifests, provenance and Engineering locks
identify content; they do not grant instruction trust or permissions. This
ownership reference changes no active policy or agent configuration.
