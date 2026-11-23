# Developer tools and source SDK

The companion Simoder-DevTools repository composes the separately owned
Simoder-DevTools-Authoring compiler/editor package, Simoder-DevTools-MCP services and
the separate optional Simoder-DevTools-REA host wrapper. Conversion and resource inspection belong to the
OpenSCP fork. The retired standalone CLIs/importers and alpha migration workflows
are no longer distributed. Its README documents current builds and installation.

Simoder owns the integrated DLL, overlay, mod lifecycle, name resolver, package
parsers and runtime diagnostics. `src/dev/runtime_diagnostics.*`
publishes runtime evidence and stays in the loader. No developer executables or
.NET payload are built or installed by this repository.

`SC13_CORE_ONLY=ON` exposes the internal `Simoder::Core` source target with source API version 1.
DevTools pins a clean Authoring checkout in `cmake/simoder-authoring.lock.cmake`.
Authoring requires explicit prepared Simoder and Luau sources when loading Core.
Core sources are compiled directly and are never copied into developer packages.
The internal source target does not fetch ImGui/MinHook or install runtime files.
Public script declarations have a separate canonical Simoder-SDK repository.

Runtime JSON retains `schemaVersion: 1`, atomic snapshots, verified PID,
generation consistency and five-second freshness. Sharing an SDK version does
not establish gameplay acceptance. Incompatible evidence schemas must be rejected.

Install game-owned tools as the DevTools service mod under `mods/simoder.devtools`
and enable it explicitly in ModLoader. Broker/MCP processes belong to the game
job and inherit its integrity level. Registration scripts belong to that package;
the client-side stdio adapter has separate client ownership. See
[service mods](developer-service-mods.md) for the current package contract.

SDK pins must be updated deliberately after validation against the selected
revision. A successful local override build does not prove that the remote pinned
revision includes the same changes. Publication and remote verification are
separate from local development.

Authoring cache generations can exceed Windows `MAX_PATH` even when the source
project remains below that limit. Cache/staging filesystem operations use
extended-length native paths; logical project identities, source fingerprints
and the required sibling `mods/cache` location remain unchanged. Regression
fixtures cover long generation paths, immutable cache reuse, checksum rejection
and failed-publication cleanup without changing global Windows path policy.
This does not establish support for arbitrarily long source-project paths.

Both compilers publish a complete generation by a non-replacing atomic directory
rename. Publication retains the staging and parent directory identities and
rejects links or changed identities. Observed transient Windows access/share
denials may be retried at 50 ms intervals, with at most 11 attempts and a 500 ms
retry budget; system-call and scheduling time can exceed that budget. Other
errors fail immediately. Existing generations are never overwritten: a competing
publication must pass the normal full generation validation. Persistent failure
reports the Windows error and paths, leaves the prior generation intact, and
removes only owned unpublished staging where filesystem permissions allow it.
