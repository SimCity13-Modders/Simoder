# Live-game runtime acceptance

> **Historical checklist.** This procedure describes a broader pre-Rust runtime
> and DevTools workflow. It has not been requalified for the current Alpha.
> Use it only as a checklist of potential observations; verify every step and
> mod against current repositories before treating it as an acceptance run.

Synthetic tests and compilation do not replace live-game acceptance. Record
current binary hashes and results under ignored .analysis/.

## Prepare and launch

Build the x86 Release target with Cargo. Close the game, preserve any existing
foreign graphics proxy, copy d3d9.dll beside SimCity.exe, and launch through
EA App. Review bootstrap and runtime logs, record the executable identity and
DLL SHA-256, and verify ordinary graphics and city loading.

The example-mod steps below refer to projects formerly distributed through
Simoder-DevTools. Verify that those examples are available from a current
source before attempting them. New mods should remain inactive until explicitly
enabled. Preserve user state and use isolated fixtures rather than deleting
existing settings.

## Declarative and scripted behavior

The BetterSanitizer example and its historical expected value (300 to 345)
require confirmation against a current example project and game build. Verify
the selected object, retained resource diagnostics and simulation effect rather
than relying only on displayed aggregate values.