# Developer tools and source SDK

The companion repository is `../Simoder-DevTools`. It owns MCP/DevBridge, the
four native developer CLIs, the VS Code extension, OpenSC5/OpenSCP importers,
REA integration and their tests. Its README documents standalone builds and installation.

Simoder owns the DLL, launcher, overlay, mod lifecycle, name resolver, package
parsers, installer and runtime diagnostics. `src/dev/runtime_diagnostics.*`
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

Install tools in a separate directory. Register MCP from its `AI` directory with
`-GameRoot` pointing to the SimCity installation. The server receives
`SIMODER_GAME_ROOT`; its location no longer determines the game path. Existing
installed developer tools can remain until registration is migrated explicitly.

The extraction retained file history in a filtered local clone. Original commit
mapping is available in the DevTools `.git/filter-repo/commit-map`. Neither
repository was pushed as part of extraction.
