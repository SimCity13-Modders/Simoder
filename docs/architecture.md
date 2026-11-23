# Architecture

Simoder is a Rust 2024 Cargo workspace that builds one integrated 32-bit x86
`d3d9.dll`. The [README](../README.md) lists current crate responsibilities;
the [repository ownership guide](architecture/repository-ownership.md) describes
the separate Base source repository.

## Runtime flow

```text
EA App launches SimCity.exe
    -> Windows loads the colocated d3d9.dll proxy
    -> proxy forwards Direct3D 9 exports to the system library
    -> graphics creation requests bootstrap on an owned worker
    -> integrated Rust runtime validates the game before installing hooks
    -> Simoder-Base and discovered mods run through the managed script lifecycle
```

Bootstrap avoids service initialization inside `DllMain`. If game validation
fails, graphics forwarding remains available and runtime hooks are not installed.
See [DLL bootstrap](dll-bootstrap.md) for the boundary and diagnostics.

## Workspace boundaries

- `simoder-core` contains shared identifiers, hashing, scanning, patch data and
  diagnostic event plumbing.
- `simoder-formats` owns data codecs and locale fallback resolution, reusing
  Simoder-OpenSCP for game formats.
- `simoder-mods` discovers and validates mods, coordinates patch state, and
  contains the native module host.
- `simoder-scripting` embeds Luau and JavaScript VMs and schedules script work.
- `simoder-crash` owns crash and game web-bridge diagnostics.
- `simoder-d3d9` integrates the runtime with the Windows proxy, bootstrap and
  game-facing hooks.
- Simoder-Base is a separate Luau repository that consumes the host's script
  APIs and supplies the base mod's services and UI.

## Verification boundary

CI runs Clippy and workspace tests for host and x86 Windows targets. Tests use
synthetic game data and fixtures where necessary. They do not establish live
game compatibility, full native-mod safety or Beta readiness. See
[CI and releases](releases.md) and keep live-game evidence separate from build
results.

Several files under [architecture notes](architecture/repository-ownership.md)
are accepted designs or historical qualification records. Their status headers
specify whether they describe an implemented behavior, proposal or dated
snapshot; design approval alone does not enable a runtime capability.
