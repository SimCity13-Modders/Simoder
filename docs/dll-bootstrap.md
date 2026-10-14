# Integrated D3D9 bootstrap

Simoder's production artifact is one x86 d3d9.dll. Copy it beside SimCity.exe;
see [installation](single-file-installation.md). Windows loads the proxy through
normal graphics startup. Original graphics calls forward to the system D3D9 DLL,
including ordinal exports. The runtime is linked directly into the proxy; there
is no separate runtime DLL, extractor, launcher or injection ABI.

Successful graphics factory creation requests bootstrap once on an owned worker.
DllMain records module ownership but does not initialize services or wait for
threads. The worker passes explicit game/data roots to the integrated runtime.
Exact executable and loaded-code validation precedes resource hook installation.
An unsupported build is rejected while graphics forwarding remains available.

## Diagnostics and disablement

Read simoder/logs/proxy-bootstrap.log and simoder/logs/sc13modloader.log.
Bootstrap logging falls back to the local application-data directory when needed.
Create simoder/proxy.disabled and restart to prevent runtime initialization while
retaining graphics forwarding. Remove the flag and restart to restore bootstrap.
Native DLL updates require the game to be closed. Uninstall by removing only the
copied DLL; preserve user data and foreign graphics proxies.

## Ownership and limitations

The runtime owns hooks, overlay, lifecycle workers and optional enabled service
jobs. Explicit shutdown stops diagnostics before hook and mod-service cleanup.
OS job lifetime also terminates owned services on abrupt process exit. Services
are optional DevTools mods rather than extracted runtime payloads.

Background startup can race early resource deserialization. Exact-build gates
and successful bootstrap do not prove that every game resource was intercepted.
Test actual resource effects and diagnostics after loading a city. No pre-launch
compatibility dialog is offered by this direct DLL path.

The integrated synthetic bootstrap test verifies system graphics forwarding,
safe rejection of an unsupported host and absence of extracted executable
dependencies. These checks are distinct from [live-game acceptance](first-test.md).
Do not reuse historical split-runtime results as evidence for a newly built DLL.
