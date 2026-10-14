# Architecture

## Runtime flow

```text
EA App launches SimCity.exe
    -> Windows loads colocated d3d9.dll
    -> proxy forwards system Direct3D object creation
    -> owned bootstrap worker calls the integrated runtime
    -> exact-build validation before game hooks

SimCity PROP deserializer
    -> immutable PatchRegistrySnapshot lookup by exact TGI
    -> retained vanilla RuntimeResourceCache entry
    -> deterministic activation-order patch sequence
    -> transactional value-bit writes to the game-owned table
    -> cache commit only after every write succeeds
```

The normal unpatched hot path loads one atomic shared snapshot, performs one ordered-map TGI
lookup, and returns. It does not scan mods, parse manifests, access the filesystem, or acquire the
`ModManager` lock.

## Component boundaries

- `d3d9.dll`: system graphics forwarding and integrated process bootstrap, build gate, hooks, logging and UI lifetime.
- `mod_candidate`: bounded stable input acquisition and fingerprints, separate from lifecycle publication.
- `script_patch_reducer`: deterministic owner-scoped set/clear reduction with a bounded persistent field set; it does not publish registry state or mutate game memory.
- `mod_panel`: ImGui mod presentation and view-local state, independent of Win32 hooks and D3D9 device lifecycle.
- `ModManager`: discovery, typed definitions, lifecycle, persistence, and UI snapshots.
- `PatchRegistry`: active ownership, activation sequence, conflict metadata, and immutable
  copy-on-write snapshots.
- `RuntimeResourceCache`: vanilla/last-result retention, targeted invalidation epochs, rebuild
  planning, and controlled-detach restoration snapshots.
- `RuntimeDiagnostics`: owned polling worker for pending-definition detection and atomic JSON
  publication; stops before hook/service shutdown and performs no hot-path filesystem work.
- `simoder-dev`: offline source validation, ordered dry-run, JSON explanations, catalog search
  and new-mod scaffolding using the same typed parser/arithmetic as runtime.
- `Simoder overlay`: rendering and input only; callbacks invoke `ModManager`.
- `AsyncLogger`: the only file/console sinks. Producers emit semantic events.
- `sc13_core`: game-independent TGI, hash, DBPF, RefPack, PROP, TOON, patch, and state logic.
- Optional MCP and DevBridge executables belong to explicitly enabled service mods. ModLoader owns their process jobs; they inherit the game process integrity level. DevBridge restricts operations to verified windows and capture-bound input.
- A client-side MCP stdio adapter is a separate client-owned process; gameplay operations must relay to the game-owned service.

`ModManager` does not parse PROP memory, implement hooks, render ImGui, or own log sinks.

## Patch and cache invariants

Activation sequence determines deterministic patch order. Every registered resource block carries
its stable owner ID and sequence. Same-TGI/same-property changes are recorded as conflicts;
different properties of the same resource are not conflicts.

Disabling never reverses arithmetic. Each result is recomputed as:

```text
retained vanilla + currently active ordered patches
```

Targeted invalidation occurs both before and after a registry publication. Per-TGI invalidation
epochs prevent an in-flight old plan from committing across an ON/OFF transition. Existing cache
entries retain owned vectors only; the loader never lends these buffers to the game, so invalidation
cannot create dangling game pointers.

Runtime writes touch only `valueBits` after verifying sorted table structure, record identity,
metadata/type, bounds, alignment, and finite arithmetic. A multi-property update uses compare/
exchange and rolls back earlier writes if any later write fails. Detach restores vanilla only when
the live resource still exactly matches the last committed result.

ON/OFF and active-mod removal reuse the same compare/exchange transaction to rebuild retained live
PROP instances immediately from vanilla plus the newly published registry generation. ON first
rereads stable source bytes, allowing an OFF-state `mod.toml` edit to take effect without a
manual Refresh.

## Threading and ownership

- `ModManager` serializes discovery and lifecycle with its own mutex.
- `PatchRegistry` serializes writers and atomically publishes immutable reader snapshots.
- `RuntimeResourceCache` has an independent mutex; the hook never takes the manager lock.
- game-memory value transitions use a small exclusive SRW lock separate from registry/cache locks.
- `AsyncLogger` copies events into a bounded queue and drains batches on one worker thread.
- ImGui state stays on the D3D9/window thread; UI snapshots are copied before rendering.

Explicit runtime shutdown first stops diagnostics, then removes hooks and the
overlay, stops the patch signal, releases mod services, and flushes the logger.
Hook rollback is shared with failed initialization. Process exit also releases
OS job handles; orderly C++ shutdown and abrupt process teardown are distinct
paths and both need lifecycle coverage.

## Graceful degradation

- unknown game fingerprint or ambiguous signature: no resource hook;
- malformed/duplicate mod: that entry is `Failed`; other mods continue;
- UI hook/context failure: runtime mods and game continue without UI;
- DeveloperConsole failure: file logging continues;
- unsafe resource layout/type/value: that resource remains unchanged;
- existing objects that cannot be recreated safely: only future loads receive the new generation.

OpenSCP property-export interchange and registry dictionaries are offline adapters.
Package migration also belongs to DevTools. Supported scripts run in the sandboxed
Luau VM. Arbitrary machine-code patching and unverified resource registration are
not supported runtime capabilities.

## Optional AI DevBridge boundary

DevBridge is a separate developer-service process, not linked into d3d9.dll.
The broker validates same-user pipe access, executable identity and Windows session.
EA/SimCity targets must pass the configured identity checks. Coordinate actions
require a fresh capture identity and unchanged client geometry. The service
inherits the owning game's integrity level; it is not a separately elevated watcher.

UI Automation invocation is preferred when the pointed element exposes `InvokePattern`; CEF and
DirectX surfaces use foreground input subject to bridge policy. UAC secure-desktop
interaction and arbitrary process targeting remain unavailable. Game-close actions
must target the verified game session through the explicit bridge command. Closing
the game releases ModLoader's job ownership and terminates its developer services.
Readiness negotiation and graceful service shutdown remain separate acceptance work.

## Deployment and optional authoring

The runtime is one production DLL with statically linked dependencies, including
Luau compilation. No executable bundle is extracted. Examples and developer
toolchains belong to DevTools. Optional TypeScript/native-resource authoring tool
registration is still being completed; it must not introduce required sibling
executables into an ordinary runtime installation.

Installed current manifests use mod.toml. Retired manifest migration is offline
DevTools work. Raw-resource integrity validation does not imply registration or
replacement capability in the game. See [resource contracts](resource-projects.md)
and [stable input validation](mod-input-validation.md).
