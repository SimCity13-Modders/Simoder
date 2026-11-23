# Live-game runtime acceptance

This procedure verifies the installed DLL, explicit mod lifecycle and service
ownership in an actual EA-launched game. Synthetic tests and successful compilation
are prerequisites, not substitutes for these observations. Record current binary
hashes and results under ignored .analysis/.

## Prepare and launch

Build the x86 Release target and run CTest. Close the game; preserve an existing
foreign graphics proxy. Copy the built d3d9.dll beside the actual SimCity.exe and
launch through EA App. No launcher or BAT is required. Read bootstrap/runtime logs
to confirm successful exact-build hook initialization. Record the game executable
identity and DLL SHA-256. Verify ordinary graphics and city loading.

Install selected examples from Simoder-DevTools into the installation's mods/
directory. Use the actual IDs and metadata in each current mod.toml. New mods
should remain inactive until explicitly enabled. Open the overlay and confirm that
Refresh detects newly installed packages without enabling them. Preserve user
state; use isolated fixtures rather than deleting existing settings.

## Declarative and scripted behavior

For BetterSanitizer, verify the manifest's canonical TGI/property and baseline
before enabling. The known scalar operating-cost patch changes 300 to 345; the
displayed aggregate may include other costs. Verify the actual selected object,
retained resource diagnostics and simulation effect rather than assuming a total.

Enable and disable the mod. Confirm state persistence, deterministic ownership
and restoration from vanilla plus remaining active owners. Edit mod.toml while
inactive and confirm enabling rereads it. While active, preview a valid change
and reload; reject invalid edits without replacing the active generation. Recreate
objects or panels when the game has copied an earlier property value.

For a Luau example, verify lifecycle logs, execution isolation, pending revision
and disable behavior. Run PROP API Example against an observed resource and check
its owner-scoped override and restoration. Record any unverified gameplay effects.

## Developer services and exit

Install the DevTools service mod and explicitly enable it. Confirm its service
processes are children of the game and that MCP can observe the current session.
Disable the mod and verify its captured process identities terminate; reenable
and verify a new owned set. Normal game exit must terminate owned services.
Use only an isolated test host for forced termination regression unless a live
forced-exit test has explicit authorization; avoid risking a user's unsaved city.

Restart through EA App and check intended persisted activation. Confirm no duplicate
services, stale listeners or independent watcher remain. The external client-side
MCP adapter has separate client ownership and must not be mistaken for a game child.

Record each result separately: bootstrap, overlay, patches, scripts, service control
and process teardown. Missing observations remain unverified acceptance items.
