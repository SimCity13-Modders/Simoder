# Simoder

Simoder is an experimental in-game mod manager for SimCity (2013) and Cities of
Tomorrow. Its production runtime is one integrated x86 `d3d9.dll`. It forwards
Direct3D calls to Windows and initializes Simoder after successful graphics
creation, outside `DllMain`. Exact game-build validation precedes game hooks.

## Installation

Close SimCity and copy `d3d9.dll` beside the actual `SimCity.exe`, usually
`<installation>/SimCity/SimCity.exe`. Launch normally through EA App. Updates
replace the same DLL while the game is closed. Remove it to uninstall automatic
startup; user settings and mods remain. Preserve any existing foreign graphics
proxy: this runtime does not implement proxy chaining.

There is no launcher, injector, binary extractor or bundled Node.js. The Luau VM
and compiler are statically linked. Examples, developer executables, optional
authoring toolchains and IDE support belong to Simoder-DevTools.

The internal [native module boundary](docs/architecture/native-module-boundary.md)
is qualified with separate DevTools fixtures. Production native admission remains
closed until the required base provider is available.

For the supported EA directory layout:

```text
<installation>/
  SimCity/SimCity.exe
  SimCity/d3d9.dll
  mods/<user-mod>/mod.toml
  mods/cache/                 # generated immutable source generations
  simoder/config.toon         # generated settings
  simoder/state.toon          # generated activation order
  simoder/logs/               # diagnostics
```

The runtime creates mutable data; one installation DLL does not mean zero data
files. Installation in Program Files and writing settings may require appropriate
permissions. See [installation details](docs/single-file-installation.md).

## Mods

The overlay manages discovery, explicit enable/disable, changed-on-disk previews
and transactional reload. Refresh does not enable new mods. Current installed
manifests use `mod.toml`; retired TOON mod manifests and `project.toml` require
offline migration in DevTools. Configuration/state TOON remains a separate
supported format.

Declarative patches modify supported scalar floats on existing game resources.
Catalog aliases retain canonical IDs and explicit provenance; a local alias is
not a verified original EA name. See [declarative patches](docs/declarative-patches.md)
and [stable input validation](docs/mod-input-validation.md).

Luau source mods compile in-process into immutable generations. TypeScript and
native-resource authoring require optional DevTools tooling; their final tool
registration contract is still being completed. Raw-resource manifests validate
payloads and full-width IDs, but activation requiring `resource.register` or
`resource.replace` remains blocked. See [resource contracts](docs/resource-projects.md).

Developer services are explicitly enabled service mods under `mods/`. ModLoader
owns their process job; disable or game exit terminates owned services. The
actual MCP/DevBridge package is distributed separately. See
[developer service mods](docs/developer-service-mods.md).

## Build

Use MSVC, the Windows SDK, CMake and Python. The game is x86:

```powershell
cmake --preset vs2022-x86
cmake --build --preset vs2022-release
ctest --preset vs2022-release
cmake --install build/vs2022 --config Release --prefix build/staged
```

VS2026 presets are also available. The runtime target is `simoder-runtime` and
its Release artifact is `build/<generator>/bin/Release/d3d9.dll`. Installation
staging includes legal notices separately; only the DLL goes beside the game EXE.
Runtime scripting tests exercise embedded Luau without authoring helpers.
TypeScript authoring tests belong to DevTools and require its worker/toolchain.
A passing incremental build is not proof of clean-checkout or live-game acceptance.
CI is being updated to reflect the runtime/DevTools boundary.

## Repository policy

Examples belong to Simoder-DevTools. Audits, exploratory reports, screenshots and
raw evidence belong to ignored `.analysis/`; maintained contracts and guides
belong to `docs/`. Proprietary game binaries, packages, local mods and generated
outputs are excluded. Tests use synthetic redistributable fixtures.

Authored code uses Doxygen comments. Original source is under the [MIT License](LICENSE);
dependencies retain their [own notices](THIRD_PARTY_NOTICES.md). Imported game and
catalog data are not relicensed by Simoder.
