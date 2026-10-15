# Developer tools and source SDK

The companion Simoder-DevTools repository composes the separately owned
Simoder-DevTools-Authoring compiler/editor package, current MCP/DevBridge and
REA integration. Conversion and resource inspection belong to the
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
