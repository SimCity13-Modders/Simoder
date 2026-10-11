# REA workspace integration

REA is an optional, local reverse-engineering assistant. It is used only by
developers and is never linked into `sc13modloader.dll`, the launcher, the
runtime package, or the Simoder MCP broker.

REA is MIT licensed upstream. Its current repository describes it as an MCP and
CLI for local analysis of native binaries, .NET assemblies, JavaScript/Electron
applications, websites, and runtime behavior. REA requires Node.js 22.19+, 24.11+
or 26+. The current host has Node 22.12.0, so `doctor` reports that prerequisite
until Node is updated. Native analysis also needs Hopper, Ghidra, or IDA; Ghidra
support on Windows is experimental.

## Commands

Run these commands from the repository root:

```powershell
.\tools\reverse\rea.ps1 doctor
.\tools\reverse\rea.ps1 setup
.\tools\reverse\rea.ps1 analyze-native `
  'C:\Program Files\EA Games\SimCity\SimCity\SimCity.exe' `
  -Provider ghidra -Json
.\tools\reverse\rea.ps1 analyze-dotnet `
  'build\vs2026\ai-devtools\Release\Simoder.McpServer.dll' -Json
```

`setup` changes agent configuration and installs REA workflow instructions;
review its proposed changes before accepting them. The wrapper uses `npx -y`,
so REA is fetched into the npm cache instead of being committed to this repo.
Use `-Latest` only when deliberately testing the newest upstream release.

## Resolver evidence contract

Do not paste an REA result directly into `symbols.toon`. Preserve the complete
raw JSON under an ignored evidence directory and create a reviewed catalog entry
with at least:

```toon
name: sanitizerOperatingCost
id: 0x09AE19D7
type: float
source: REA report <sha256>
origin: rea_static
confidence: reviewed
buildFileSha256: <exact SimCity.exe hash>
buildTextSha256: <exact loaded .text hash>
```

The loader accepts only the existing typed alias fields. `confidence`, evidence
references, and build hashes are review metadata until a maintainer promotes the
entry into a catalog. REA output is evidence, not permission to patch a resource.

## Scope and safety

Analyze only binaries and data for which the project has authorization. REA runs
locally, but findings are returned to the configured agent. Do not include EA
packages or proprietary game assets in commits. Do not use REA to bypass loader
fingerprints or to write process memory.
