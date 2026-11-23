# Single-DLL installation

The production target `simoder-runtime` emits one integrated x86 `d3d9.dll`.
The proxy, supported game hooks, mod lifecycle, Luau compiler/VM and overlay are
linked into this file. It does not extract executable dependencies or examples.
Node.js and developer services are not part of the runtime distribution.

## Install, update and remove

1. Close SimCity.
2. Copy `build/vs2026/bin/Release/d3d9.dll` beside the actual `SimCity.exe`,
   usually `<installation>/SimCity/SimCity.exe`.
3. Launch normally through EA App.

VS2022 builds use the corresponding `build/vs2022` directory. Preserve a foreign
`d3d9.dll`, such as a graphics mod: proxy chaining is unsupported. Updates replace
the DLL while the game is closed. Removing it stops automatic startup and retains
user mods/settings. Program Files installation can require administrator access.

## Generated data

For the supported EA layout, the installation root is the parent of the EXE
directory. `<installation>/simoder/` stores settings, state and logs;
`<installation>/mods/` contains user packages and immutable source caches.
These directories need write access for changes and compilation.

`simoder/proxy.disabled` disables runtime bootstrap on the next start while
graphics forwarding remains active. Bootstrap writes `simoder/logs/proxy-bootstrap.log`,
with a local application-data logging fallback. No bundle extraction directory
is used. Failure to validate the game executable leaves ordinary graphics
forwarding available rather than installing unsupported hooks.

## Build and checks

```powershell
cmake --preset vs2026-x86
cmake --build --preset vs2026-release --target simoder-runtime
```

Build all tests before running CTest. The integrated bootstrap test starts a
synthetic host with only the DLL beside it and verifies graphics forwarding,
safe unsupported-build rejection and absence of extracted DLL/EXE dependencies.
Desktop Direct3D access is required. These checks do not prove behavior in an
actual EA-launched city; live-game acceptance must be recorded separately.

Example mods and optional developer services are installed from DevTools. Luau
gameplay requires no external compiler. Optional TypeScript/native-resource
authoring remains separate from the one-DLL runtime installation.
