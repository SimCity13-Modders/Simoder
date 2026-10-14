# Third-party components and provenance

Simoder's original code is licensed under the root MIT LICENSE. That license
does not replace the licenses or copyright notices of external components.

| Component | Use | License / source |
| --- | --- | --- |
| MinHook v1.3.4, including Hacker Disassembler Engine | Downloaded by CMake and linked into the loader | BSD-2-Clause; retain the complete upstream LICENSE.txt, including HDE notices. https://github.com/TsudaKageyu/minhook |
| Dear ImGui v1.92.9 and Win32/DX9 backends | Downloaded by CMake and linked into the overlay | MIT, copyright Omar Cornut. https://github.com/ocornut/imgui |
| Luau 0.695 (`1f6ff5d6f0072f2fab2adf35d1ffbaae7d848ad0`) | VM linked into the core; compiler linked only into `simoder-build.exe` | MIT; full `LICENSE.txt` installed as `simoder/licenses/luau-LICENSE.txt`. https://github.com/luau-lang/luau/tree/0.695 |
| TypeScript 6.0.2 and TypeScriptToLua 1.37.1 | Optional external TypeScript toolchain, pinned in `tools/scripts/typescript/package-lock.json` | Apache-2.0 (TypeScript), MIT (TypeScriptToLua); all dependency licenses retained in the installed toolchain. https://github.com/TypeScriptToLua/TypeScriptToLua |
| Node.js | Optional single-file authoring bundle only (`SC13_BUNDLE_TYPESCRIPT=ON`); excluded from the default runtime distribution | Preserve the complete official LICENSE for the executable's exact release as `licenses/node-LICENSE.txt`. https://github.com/nodejs/node |

The native installer copies the full MinHook and Dear ImGui licenses into
`simoder/licenses`. Developer tooling dependencies and external catalog importer notices are maintained in Simoder-DevTools.

The OpenSC5 and OpenSCP integrations are independently implemented importers
for external catalogs and JSON exports. Their source trees and catalogs are
not bundled as tracked project sources. Imported names, descriptions, comments,
and other data retain their source's terms; provenance hashes do not grant
redistribution rights. Users must check those terms before sharing imported data.

TOON parsing is a local implementation based on the public format specification,
not a bundled third-party parser dependency.

EA game binaries, package files, and game assets are excluded from the repository
and are not covered by Simoder's MIT license. Tests use synthetic fixtures.

Node.js is an external requirement for TypeScript authoring and is not bundled.
The optional `sc-converter-cli.exe` comes from Simoder-OpenSCP, built independently;
its full license bundle must be supplied through `SC13_RESOURCE_COMPILER_LICENSES`
when configuring its installation. No OpenSCP engine sources are copied here.
# toml++

The schema-three TOML parser includes the unmodified single-header toml++ v3.4.0
from https://github.com/marzer/tomlplusplus/tree/v3.4.0, under MIT.
Its license is preserved in `third_party/tomlplusplus/LICENSE` and installed with the runtime.
