# Private base runtime

U3e implements private Core machinery and the separately authored Luau services in
Simoder-Base. It does not promote SDK facets, enable a production authority provider,
register game operations, install packages, or migrate MCP. The production native
descriptor catalog is empty. A ready base does not authorize an operation.

## Ownership and identity

Core captures and verifies the explicitly supplied package pin, compiles those
immutable sources, and creates the dedicated base VM on the existing script lane.
The real Luau graph contains values, tasks, lifecycle, registry and runtime-manager.
Core captures the actual frozen service objects after graph startup, checks that the
runtime manager is the registry's singleton, and exercises the actual value and
lifecycle adapters. Only native postcommit can publish that captured VM generation's
private readiness lease. A script Boolean or descriptive mod ID cannot publish it.

Each mod receives a native endpoint bound exactly once to its actual VM generation.
The endpoint retains its original RuntimeScope. A second VM cannot reuse an endpoint.
The public presentation is the pin-verified Base Luau facade, compiled and copied
into a reserved module namespace in the mod's independent VM. It contains no second
task state machine, authorization store, registry or compiler. Native capabilities
are immutable closure groups; the private base capability table is absent from mods.

Requests reserve one native task and a copied invocation. A later base turn calls
the actual Luau runtime-manager dispatch with an unforgeable invocation userdata.
The semantic dispatch resolves the Lua catalog and operates on the same reserved
task. There is no synchronous cross-VM entry from a mod callback and no inner task.
Original scope, VM generation, runtime/base epochs and typed context lease survive
the queue, callback and await paths. A producer remains a reviewed native function,
never arbitrary script evaluation.

## Context and permissions

RuntimeScope remains the sole operation permission store and shared writer domain.
Admission, preparation, commit and result delivery recheck its original issuing
context. A narrow atomic-only factual context fence runs at its commit boundary.
Base shutdown stops dependent scopes; an already committed effect may finish and
is never represented as rolled back.

Decoded references are syntax, not permission. Native resolution checks the typed
runtime/region/city/entity slot, generation and nonreused private epoch. Lua copied
wrappers containing resolvable references retain that factual lifetime when first
wrapped; an unresolved/foreign reference stays untrusted. Reusing a wrapper after
the same numeric slot/generation returns does not rebind it. Runtime-only leases
remain valid across unrelated city changes. Result reference wrappers retain the
original task's captured context; an extra, uncaptured reference cannot become a
new live view through result decoding. Current production game context remains
unknown and mutations unavailable; synthetic SinglePlayer observations exist only
in the independently compiled test host.

## Tasks, continuations and publication

Task terminal state is immutable and separate from its commit flag. Preparation can
be cancelled; cancellation of any committed task returns Busy, including after
success. Missing/failed precommit dispatch terminalizes the reserved task without
replay. An unverified postcondition, malformed output or exception after commit is
Indeterminate. Revocation suppresses delivery without changing the stored outcome;
owner-bound release still works.

Observers fire at most once on a later turn. Completed tasks retain outcomes while
callback delivery is delayed. Await admits only a host-created, registry-retained
coroutine; raw coroutine.create cannot acquire this status. All create/resume/yield,
reference and close operations remain on the single existing lane. Stop drops
callbacks and suspended coroutines without resuming old code. DEV isolates a failed
semantic function; REGULAR stops its mod. Any base failure closes the base epoch.
Failures enter the existing bounded diagnostic/log path using host codes.

The native source publisher journals 128 exact sequenced publications. Snapshot
reads do not invent a new sequence or timestamp. Subscribe captures the current
publication and its cursor atomically, then retains each ordered subsequent record
in the generation's shared 128-event mailbox. A source or mailbox overflow faults
and closes that observer explicitly. Unsubscribe returns Busy for an executing
delivery and closes it after completion. Callback, coroutine and subscription
delivery categories and slots rotate; one eligible delivery executes per VM turn.

## Bounds and cleanup

All base epochs share the same copied-value storage pool: 16 MiB globally and 4 MiB
per scope owner. Inputs, reserved result arenas, retained results, transport/control
capacity and presentation bookkeeping use that pool. Result wire and decoded arena
capacity are reserved before effects; result completion does not allocate an
emergency result buffer after commit. Envelopes retain their exact charged arena
until the final lease disappears.

Per-generation limits remain 128 retained task/operation/invocation records,
64 continuation/resource registrations shared with native dependencies, 128 mailbox
events and 1 MiB of mailbox payload. The shared writer domain admits eight pending
operations. The bridge's fixed request queue also bounds copied wire to 1 MiB.
VM allocations use the existing 2–64 MiB caps and combined 128 MiB declaration
budget, including base's 8 MiB. Storage bounds describe requested native capacity,
not total process/private bytes or a hard wall-clock latency guarantee.

Native dispatch wrappers can retain large external arenas while occupying little
Lua heap. The base performs collection at the safe end of a dispatch turn so
unreachable wrappers cannot indefinitely consume native quota. Retained semantic
values remain charged normally. Stop closes admission before onDisable, returns
registration slots, and releases references on the live lane before Lua closes.

## Beta diagnostic seam and qualification

The native host can copy a bounded 64-record local correlation ring. Records contain
the Core source/toolchain fingerprint, exact native VM/scope/runtime/base/context
identities, operation sequence, phase, monotonic time, bounded reason and dropped
count. No arbitrary mod ID, path, payload, credential or script exception text is a
trusted field. This fingerprint is not an exact binary hash or a crash-symbol match.
The ring neither exports data nor installs crash hooks. Crash UI, report/dump format,
symbol capture and the helper-executable comparison remain D1/D2 work.

Synthetic issuers and descriptor producers are compiled into a separate,
non-installed test Core from the identical source files. They are absent from the
production Core and d3d9.dll. U3e conformance uses real isolated Luau VMs and the
actual pinned Base graph, plus native cancellation/revocation/context/storage fault
fixtures. Actual game qualification and selective existing-operation adapters remain
U3f/U6; a successful source build does not prove game behavior.
