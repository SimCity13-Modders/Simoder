# Source mods and Luau scripting

A folder containing `mod.toml` is an editable source mod. Place it under
`SimCity/mods/<name>/`, enable it in the overlay, edit its files in Mod Studio or
another editor, review the pending generation and choose **Apply validated reload**.
The current implementation executes script-only source mods. A project containing
native resource additions/replacements is compiled and validated, but activation
of the entire project remains blocked until the game's resource-manager API is verified.

```toml
schemaVersion = 2
format = "simoder.authoring"

[metadata]
id = "author.example"
name = "Example"
author = "Author"
version = "1.0.0"
description = "Editable source mod"

[scripts]
language = "luau" # or "typescript"
entry = "scripts/main.luau" # scripts/main.ts for TypeScript
memoryMiB = 8
budgetMs = 2
```

The schema is shared with Simoder-OpenSCP. Script-only projects require no resource
lock. Resource projects retain `.simoder/resources.lock.toml` and originals.
Mod Studio lists `.luau`, `.ts` and `.d.ts` files and saves them with the same
revision checks as property documents. **Build native resources** exports the
classic native package. The loader compiles Luau in-process. TypeScript compilation
uses the compiler and toolchain registered by an enabled developer service mod;
Node.js must be available separately. Disabling that service mod revokes its
authoring tools. Already committed gameplay scripts retain their own VM lifecycle.

## Lifecycle and host API

An entry returns a table with optional `onEnable()`, `onTick(deltaSeconds)` and
`onDisable()` functions. TypeScript entry modules export those functions.
Initialization and `onEnable` run in a candidate VM. Their messages are buffered
until the mod is committed. Syntax errors, initialization errors, budget exhaustion,
stale previews and changed inputs preserve the existing active generation.
Successful reload creates a fresh VM: local variables and module state reset.
There is no automatic persistent-state migration.

Each mod owns a separate Luau VM. Local Luau modules use `require("./helper")`,
relative to the requiring file. They are compiled together, loaded from memory,
cached per VM and checked for cyclic imports. Traversal outside `scripts/` and
filesystem/package/native-library loading are rejected. TypeScript modules are
bundled before Luau compilation.

| API | Meaning |
| --- | --- |
| `simoder.apiVersion` | `1` |
| `simoder.modId` | Current mod identity |
| `simoder.log(message)` | Attributed mod log, at most 2,048 bytes per message |
| `simoder.time()` | Elapsed monotonic wall time, **not** game simulation time |
| `simoder.hasCapability(name)` | Script capabilities; `game.props.float` when bound to the loader's registry/cache host |
| `simoder.props.getFloat(resource, property)` | Copied historical scalar-float observation or nil |
| `simoder.props.setFloat(resource, property, value)` | Queued owner-scoped absolute float override |
| `simoder.props.clearFloat(resource, property)` | Queued removal of this mod's scripted override |

`simoder` and builtin library tables are read-only. `io`, OS process execution,
`package`, `loadstring`, `getfenv`, `setfenv` and unrestricted `debug` are absent.
Luau is a new Simoder scripting layer; vanilla JavaScript remains an ordinary
native game resource and is not translated into Luau.

The background scheduler targets 20 Hz. `onTick` receives time elapsed since that
mod's previous callback. A round-robin cycle has a five-millisecond soft aggregate
budget; one callback can exceed that aggregate by its individually configured
limit. Default callback limit is 2 ms (configurable 1–20 ms), enforced by Luau's
interrupt hook. It does not preempt a currently running native library function.
VM allocations are capped at 8 MiB by default (2–64 MiB), and the sum of declared
budgets, including staged reload VMs, cannot exceed 128 MiB. Host messages and
module inventories have separate bounds. A failed tick stops that script and
appears in the overlay/DevBridge snapshot; it does not roll back unrelated PROP patches.
Callbacks run on a loader-owned thread. PROP commands publish after successful
callbacks, outside the VM lock, through the existing verified float patch path.
See [PROP scripting API](script-prop-api.md) for names, transactions and observation limits.

## Compilation, cache and installation

The DLL statically links the Luau compiler and interpreter. Script-only Luau
mods compile in-process on the owned lifecycle worker, outside the render thread.
No external Luau worker or Node.js installation is required for those mods.

TypeScript and native-resource authoring belong to Simoder-DevTools. Its
simoder-build worker uses the pinned TypeScriptToLua toolchain and optional
OpenSCP converter. Current external tool discovery still needs integration with
enabled developer-mod ownership; do not deploy worker executables beside the game
as a permanent runtime requirement. No Node VM executes gameplay scripts.

```text
mods/MyMod/mod.toml
mods/MyMod/scripts/...
mods/cache/<hash-of-source-path-and-ID>/<generation-hash>/
  generation.toml
  script.luau.bin
  modules/...
  script.luau          # TypeScript bundle, when applicable
  script.luau.map      # source map, when emitted
  runtime/mod.toml
  runtime/assets/...
```

Generation keys include declared inputs, local scripts, manifest-declared catalogs, technical originals, the
worker binary, optional native compiler binary, compiler profile and TypeScript
toolchain sources/lock. Cache is reusable and generations are immutable. A private
staging directory is atomically published only after input hashes are rechecked.
The classic `mod.package` is removed from this direct source-mod cache to prevent
accidental duplicate loading. A damaged generation fails checksum validation;
remove that generated directory and refresh to rebuild it. Do not distribute
bytecode caches as mod sources: the VM accepts only compiler-produced generations.

Filesystem watching uses `ReadDirectoryChangesW`, ignores `cache` case-insensitively
and coalesces writes at the diagnostics interval. A five-second rescan is used only
when the native watch is unavailable. Overlay operations are queued to a worker;
rendering uses the last complete snapshot while compilation is in progress.
Compiler processes receive only their explicit standard handles and are contained
in a kill-on-close Windows job with a 60-second outer timeout. Compilation does
not execute mod-owned `tsconfig` plugins, transformers or package scripts.

`simoder-build.exe --check <mod-folder>` explicitly executes script-only lifecycle
callbacks in a bounded standalone VM for diagnostics. Normal compilation does not
execute scripts. The standalone check does not provide a game simulation or asset APIs.

Build the runtime using its normal x86 CMake presets. Install authoring
toolchains and examples separately from Simoder-DevTools; none are embedded or
extracted by the runtime DLL. Copy selected examples from DevTools into mods/
and enable them explicitly. Toolchain setup and external compiler configuration
belong to the DevTools build/install guide.

TypeScript follows TypeScriptToLua semantics and supported features; it is not a
JavaScript/Node runtime. Source maps are retained in cache, but automatic remapping
of runtime traceback lines to TypeScript source is not implemented. Additional
source languages can supply an external compiler producing Luau under this same
runtime contract; this release supports only Luau and TypeScript.

## Verification and remaining integration boundary

Runtime acceptance tests execute embedded compilation and the x86 VM without
external helpers. Separate DevTools authoring tests execute its actual worker:
modules, TypeScript bundles and helper libraries, cache reuse, initialization and
disable logs, sandbox isolation, memory/time limits, tick errors, stale previews,
syntax errors and preservation of the active VM after rejected reload.
The converter retains its native-resource tests and adds Studio script-project
round trips. CI uses synthetic fixtures exclusively.

The six Project Agua resources compile into `mods/cache` and pass payload hash
validation. This confirms source compilation and preservation of their native
data. It does not confirm resource registration, renderer ownership, asset cache
invalidation or game script reinitialization. The verified game hook still mutates
only existing scalar float PROP values. Full PROP structure edits, added resources,
textures, models and native JavaScript need verified stream/resource registration,
allocator/destructor, cache release and safe-thread boundaries. No guessed ABI is
called and no partial source project is reported as active.

References: [Luau sandbox and interruption](https://luau.org/sandbox/),
[TypeScriptToLua configuration and Luau target](https://typescripttolua.github.io/docs/configuration/).
