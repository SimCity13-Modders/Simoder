# Developer tools and source SDK

The companion repository is `../Simoder-DevTools`. It owns MCP/DevBridge, the
four native developer CLIs, the VS Code extension, OpenSC5/OpenSCP importers,
REA integration and their tests. Its README documents standalone builds and installation.

Simoder owns the integrated DLL, overlay, mod lifecycle, name resolver, package
parsers and runtime diagnostics. `src/dev/runtime_diagnostics.*`
publishes runtime evidence and stays in the loader. No developer executables or
.NET payload are built or installed by this repository.

`SC13_CORE_ONLY=ON` exposes the `Simoder::Core` source target with SDK API version 1.
DevTools locks a precise Simoder Git commit in `cmake/simoder-core.lock.cmake`.
A deliberate `SIMODER_SOURCE_DIR` override permits development against a local
checkout. Core sources are compiled directly and are never copied into DevTools.
The SDK does not fetch ImGui/MinHook or install runtime files.

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
