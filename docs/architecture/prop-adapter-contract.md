# Existing PROP adapter contract

Status: **U3f-b/U3f-c accepted**; U3f-d adds a closed same-task publication seam
for isolated host qualification, not a production provider or public SDK.
This specifies migration of the existing float32 read/override/clear operations.
It adds no game opcode, arbitrary native call, resource registration, MCP transport,
permission setter or production authority. Current supported bindings remain as
documented in [the PROP API guide](../script-prop-api.md) until an explicit cutover.

The accepted [Base design](base.md), [private runtime](base-runtime.md),
[scheduler](script-scheduler.md) and [native boundary](native-module-boundary.md)
remain authoritative for their respective layers. SDK Native/joint-contract.md and
value-contract.md remain canonical for published declarations and copied values.
Names and records introduced below describe proposed internal contracts; none is
an exported symbol or a callable SDK facet today.

The proposed [permission provenance and owner lifecycle contract](permission-owner-lifecycle-contract.md)
specifies ordinary-script versus agent authority, closed decision adapters and the
exact-owner removal/handoff gates following U3f-f. It does not enable production
permissions or claim a qualified native lifecycle producer.

## PROP-B01 — Ownership and unchanged scope

| Layer | Sole responsibility |
| --- | --- |
| Core | Verified origin, permissions, factual context, bounded copied values/tasks, low-level cache/patch primitives and the single native publication fence. |
| ModManager and PatchRegistry in Core | Existing owner-local patch reduction, activation order/conflicts, immutable effective registry and lifecycle transactions. Their algorithm is reused, not copied into Base. |
| Base, authored Luau | Mod-bound semantic presentation, batch construction, friendly names, managers and task/callback/await presentation using the existing native task. |
| SDK | Canonical promoted schemas, declarations, identity and conformance evidence after implementation qualifies. |
| DevTools | MCP protocol, agent session integration and tools consuming qualified interfaces. No MCP parser or tool catalog enters Core/Base. |

Native resource memory/cache commits keep their own verified resource-write guard.
It is not another script scheduler or another mod-permission store. Base never
owns game pointers or reimplements registry conflict ordering. Native DLL trust is
separate from script permissions and agent consent; a trusted DLL is not sandboxed.

## PROP-B02 — Origin and permission provenance

An ordinary script origin is issued only by Core from the actual registered VM,
native scope, committed owning ModManager record and source generation. It carries
the runtime/base epochs and the owner's current permission revision. Source digest,
VM generation, registry generation and writer revision are distinct identities.
Matching a textual mod ID after reload does not transfer the old origin.

The provider must qualify a typed private distinction between script origin and
authenticated agent origin. Ordinary mods do not require an agent login or receive
a fabricated agent session. Agent-originated calls additionally retain their real
authenticated principal, channel, session and applicable consent; routing through
Base cannot downgrade them into ordinary mod calls. These private changes require
reviewed implementation and canonical promotion before any ABI/layout change.

`hasCapability('game.props.float')` currently reports an attached cache callback.
It is **not** an approved permission, SP observation, source trust or live readiness.
A manifest declaration is a request, not an authoritative grant. The future script
provider may bind an operation only after Core reads a user-approved permission
decision from the qualified permission service. There is no such production issuer
qualified today: absent permission provenance/provider remains Denied/NotReady.
Do not adopt the fixture issuer, add an unconditional grant on Base startup, or
create a second grant database in ModManager/Base.

For an ordinary mod, effective access intersects its explicit read/write capability,
active native owner, descriptor allowlist and mandatory context/lifetime gates.
For an agent, also intersect its routing role and session-wide OR entity-specific
consent. The OR never bypasses mandatory restrictions. Recheck at reservation,
prepublication and result delivery. Revocation before commit prevents effects;
revocation afterward prevents disclosure without rewriting the stored outcome.
Owner-bound cleanup remains possible without disclosing revoked data.

## PROP-B03 — Context and historical reads

A cached float observation is immutable historical evidence, not a live game read.
Reads use already copied cache data, never wait for Base/game VM execution and
never follow a game address on the script lane. Return an explicit historical
snapshot with its actual source observation time/generation and status. No data,
ambiguous instances and unavailable/denied access are distinct outcomes; do not
invent vanilla or a successful live read from a missing cache entry.

A historical-only read needs a qualified disclosure policy and native owner/runtime
lease, but it need not invent an SP/city fact when it never enters the live game.
If the snapshot contains a bound region/city/entity reference, preserve and check
that original typed lease; do not relabel a stale observation as a current entity.
Cross-mod reads/cooperation require the explicit narrowed cooperation API, not
another mod's facade or a caller-supplied origin.

New privileged writes require native runtime/region/city facts appropriate to the
method, a qualified SP/MP observer and the applicable target/context lease. Unknown
or Multiplayer context denies this scoped mutation route. Cache contents, a window
handle, script metadata or readiness alone cannot supply those facts. Observers
must qualify invalidation versus publication races, not merely check an epoch once.
A retired context never binds to a replacement with identical visible IDs.

## PROP-B04 — Values and bounds

Use the existing single copied-value codec; Base does not parse a second wire
format. Normalize aliases from the validated catalog before descriptor admission,
retain their provenance and reject unsupported/high instance bits without truncation.
Untrusted reference syntax does not establish authority. Resource/property IDs have
their existing exact ranges; a new resource representation needs separate scope.

Float input remains finite binary64 within float32 range, converted with the
existing float32 rounding semantics. Preserve signed zero and compare float bits,
not numeric equality, when deciding whether an override changed. Float32 results
can be represented exactly as finite binary64. Identity/timestamp/generation/count
fields use exact native integer wrappers/decimal encodings, never implicit doubles.
The current legacy metadata shape is not silently changed in place.

An atomic proposed batch contains at most the existing 256 commands. The shared
codec's 1 MiB envelope, depth/node/container bounds and shared 16 MiB global/4 MiB
owner storage apply together; a batch may hit a stricter bound first. Reserve input,
prepared registry/publication data, task/control retention and at most 4 KiB of
result wire plus its decoded arena before effects. Per-method 4 KiB is proposed,
not an already deployed SDK cap. Do not allocate an emergency result after commit.

## PROP-B05 — Startup and operational API semantics

Keep startup declarative preparation distinct from an operational task. Existing
onEnable PROP batches are reduced and validated in the candidate, with no registry
publication/live mutation before activation. Failure retains the previous committed
generation. A future Base presentation must expose this same pure preparation
through its initialization capability; it cannot open task mutation admission
before native activation postcommit. General operational methods return NotReady
during initialization and onDisable. This preparation is not an authority issuer.

For active callbacks, define an explicit atomic override batch over the existing
set/clear operations. Base constructs the batch; Core owns its one task and native
plan. Single-command helpers compose this operation, not a second task/state machine.
For the lifecycle migration fixture, collect a callback's legacy commands into one
batch, preserving its all-or-discard behavior and owner-local ordering. Do not
replace legacy void/synchronous calls with asynchronous tasks under identical
signatures and claim unchanged semantics.

Before admitting batches produced by a callback for execution, the native scheduler
must qualify that callback's successful completion boundary. A throwing callback
discards its uncommitted batch. A registered coroutine yield is only an admission
boundary if the actual host scheduler explicitly qualifies it; an unregistered
coroutine or unsupported yield is not success. A completion cannot await its own
unpublished callback batch; reject the cycle before suspension. No synchronous
cross-VM wait is introduced. These receipt/failure rules need actual fixtures
before promotion; current task machinery alone does not prove lifecycle parity.

At cutover, update the explicit consumers/examples and remove superseded bindings
in the same accepted increment. There is no hidden legacy fallback, second tick
loop or pre-Beta backward-compatibility subsystem. Until that increment, the current
supported PROP path remains unchanged and the new provider remains unavailable.

## PROP-B06 — One writer protocol, not a wrapper around Replace

At the U3f-b baseline, legacy mutations did not enter RuntimeCommitDomain and
PatchRegistry::Replace performed fallible preparation inside its publication API.
U3f-c introduces the registry primitive and shared legacy writer reservation
described below. Generic CommitValue still sets its conservative committed marker
before calling a native callback. U3f-d instead connects a closed typed registry
primitive to the same task fence; it does not use that generic callback as the
publication primitive. Do not call the manager from the atomic-only generic
precommit guard or infer production authority from either internal mechanism.

The proposed Core prepared-publication coordinator must enroll **all** competing
registry writers: enable, disable, reload, load-order changes and dynamic callback
batches as well as the semantic adapter. A domain revision advanced only by the
new adapter is insufficient. Preserve separate source, registry and domain revisions.

1. Prepare copied candidate data, owner-local reduction, effective registry snapshot,
   complete result storage and publication/control capacity. Preparation may lock
   existing stores briefly, but releases them before VM execution/waits and before
   commit. Do not publish/invalidate live evidence as a preparation side effect.
2. At commit, use one documented acquisition order: domain writer, lifecycle writer
   if applicable, manager, registry publication guard, then short scope-state access.
   New operational acquisition uses nonblocking reservations; Busy has no effects.
   Refactor every competing writer to that order before activating the adapter.
   Never hold a later lock while waiting for an earlier lock or joining the lane.
3. Revalidate the actual originating runtime object/generation, candidate/source,
   permission revision/lease, Base and factual contexts, runtime stop and expected
   registry/domain revisions. A context invalidator must participate in the same
   linearization protocol or supply an equivalent proven native reservation; a
   racy observation-only fence is insufficient. Game/render callbacks must not wait
   on these locks; native live application rechecks its original context token.
4. Establish a single cancellation-versus-publication winner. After validated native
   reservations, mark the one task irreversible and advance the domain revision
   immediately adjacent to publishing the prepared registry snapshot. This internal
   critical section performs no allocation, script/foreign callback, disk I/O,
   logging or waiting. Retain old snapshots for destruction outside locks. The
   committed bit denotes the conservative point of no return, not proof that a
   live game value changed. Cancellation before it prevents publication; afterward
   it returns Busy. An exception/uncertain effect after it is never Cancelled.
5. Publish an immutable registry result identifying the real registry generation and
   domain revision; do not let readers assemble a fictitious revision tuple from
   separate unsynchronized reads. Release reservations before delivery on the script
   lane. A stale owner receives an explicit failure, not legacy ignored-success.

Lifecycle persistence remains a separate fallible boundary. Existing enable writes
enabled-order state before registry activation; dynamic script overrides are not
persisted through that path. This contract does not pretend disk + registry + native
memory form one atomic transaction. Native ownership of a durable intent/recovery
protocol must qualify before claiming atomic lifecycle persistence or migrating
those lifecycle operations into a broader task success condition.

## PROP-B07 — Registry and live postconditions

| State | Permitted interpretation |
| --- | --- |
| Prepared, not published | No override effect. Cancel/revoke/stale failure releases reservations; previous committed state survives. |
| Registry committed | Verified owner patch set/reduction/ordering at the exact published generation. Does not establish native resource refresh. |
| No change | Validated current owner-local bitwise state already matches; no false revision increment or claim a competing owner's effect changed. |
| Live pending/unavailable | Registry remains committed. Explicit separate status; never call this verified gameplay success. |
| Live verified | Qualified native observer matched the exact requested resource-instance/context and registry generation. Partial sampling does not verify all targets. |
| Live partial/failed/unknown | Preserve committed registry state and diagnostic facts. No automatic rollback/replay; an unverified promised postcondition is Indeterminate. |

A future registry-only descriptor may succeed only if its declared postcondition is
the verified registry publication, and its result explicitly identifies live status
as pending/unavailable/not requested. It must not be exported or named as a verified
gameplay mutation. A descriptor promising live application requires a qualified
bounded game executor and verified postcondition. Until its asynchronous completion
can settle the **same** retained native task without holding writer locks, that
descriptor remains unavailable. Do not synchronously wait on game work from the
script lane or invent a second inner task to hide the missing completion seam.

Current manager refresh logs failure after registry success; current native refresh
can partially update resources and rollback can fail. These facts prohibit a blanket
all-target success or rollback guarantee. Explicit retry is a new validated operation
against current state, never automatic replay of an indeterminate committed request.

## PROP-B08 — Result delivery and promotion

Reserve a bounded copied result describing native owner generation, actual registry
and domain revisions, publication/no-change status and separate live status. Include
only descriptor-approved observation data; no raw addresses, paths, tokens or arbitrary
error text. Revalidate result access separately from immutable stored outcome. All
task callbacks/await/cleanup remain on the existing scheduler with the existing
generation fence, quotas and DEV/REGULAR failure policy.

Promotion requires the executable matrix below, independent correctness/security/
contract audit and human acceptance. Ordinary source/fixture tests are not proof
of actual game context, native resource parity, background interaction or persistence
recovery. Do not populate the optional native semantic/dispatch/credential tables
because Base is ready. Canonical SDK schemas/declarations and actual direct consumers
are promoted explicitly only after each facet qualifies; no hand-edited generated
headers or unrelated MCP/REA/Engineering pin changes.

## Implementation qualification matrix

| ID | Required oracle before production activation | Present evidence boundary |
| --- | --- | --- |
| PROP-T01 | Real production scope rejects caller-supplied authority; script role needs no fictitious agent login and cannot impersonate agent/Base/another mod. | RuntimeScope/semantic host negatives cover absent provider and fixture origin; no production script issuer exists. |
| PROP-T02 | User-approved operation grant, revocation before queue/commit and after success-before-delivery; cleanup without disclosure. | Generic and U3f-d prepared-publication fixtures cover the retained grant/revocation mechanism, not a production permission source. |
| PROP-T03 | Historical snapshot exact64 metadata, nil vs ambiguity, permission narrowing, stale typed context and signed zero. | Existing cache/script tests cover current shape; new exact64 facade shape not implemented. |
| PROP-T04 | Init/tick/throw/disable/reload/stale generation preserve callback batches; no task effect before activation, failed callback or qualified receipt. | Existing legacy lifecycle/script tests; new facade receipt and explicit cutover not implemented. |
| PROP-T05 | Every competing legacy/new writer advances the same domain; revision conflict rejects before effects. No locks across Lua or scheduler waits. | U3f-c integrates legacy manager publications with the shared domain; registry/generic-task host fixtures cover the seam. Operational provider cutover remains absent. |
| PROP-T06 | Deterministic cancel/revoke/context retirement races immediately before and after the single publication point. No stale new-city effect. | U3f-d joins the actual prepared registry primitive to the fixture task/context reservation. Ordered before/after cases and bounded concurrent host races are distinct evidence; actual game observer and lifecycle timing remain unqualified. |
| PROP-T07 | Allocation/persistence/publication failure before fence preserves prior state; no post-fence result allocation or false rollback. | U3f-c covers registry preparation/publication; U3f-d uses the same task's reserved result capacity before its marker. Production plan accounting and durable recovery remain unqualified. |
| PROP-T08 | Registry success plus per-resource refresh partial/failure/rollback failure never becomes blanket live success. No automatic replay. | Native resource fixtures and diagnostic source evidence; production semantic postcondition descriptor absent. |
| PROP-T09 | Exact256 boundary, one-over rejection, float32/range/unsupported identifiers, bitwise no-op, owner-local clear and activation-order conflicts. | Current script/reducer/resource fixtures; new descriptor-specific negative vectors required. |
| PROP-T10 | Actual canonical SDK generator/identity and clean pin drift/dirty-root tests; consumer and offline layout parity. | Existing declaration integrity tests; no declaration/pin promotion in U3f-b. |

## Next bounded implementation

### U3f-c internal seam and remaining gates

Core separates fallible `PatchRegistry::Prepare` from guarded publication of a
move-only candidate. The candidate captures its exact registry and generation,
owns its prebuilt snapshot and retains replaced stores until outer locks release.
The immutable snapshot pairs registry generation with the domain revision assigned
at its publication. A stale, foreign, consumed or discarded candidate cannot publish.
Discarding an unpublished candidate has no registry effect; this alone does not
qualify cancellation of a native semantic task.

Existing manager mutations share `RuntimeCommitDomain` with generic native commits.
Their reservation order is domain, manager, registry; the existing lifecycle atomic
flag remains an admission token across candidate creation, not a held mutex. Legacy
synchronous script drains may wait for domain admission only after releasing the
manager and VM locks. They prepare their native registry candidate after reacquiring
the manager under that reservation, before the irreversible section. This preserves
the existing Boolean command sink under contention without rerunning Lua or adding
a retry queue. New operational task admission remains nonblocking and unavailable.

The irreversible section advances the domain revision and publishes preallocated
state without allocation, callbacks, persistence, logging or explicit waits. Cache
invalidation and resource refresh occur afterward, outside the domain reservation.
Only the two existing activation-recovery paths may advance a cleanup publication
after the sticky stop gate; this does not admit an ordinary write or a new task.
Stopping after the last native fence may lose the race to publication. No claim is
made that an asynchronous stop always retracts an already committed operation.

Standalone registries used for local validation retain serialized legacy methods
over the same preparation/publication algorithm. They are not the game manager's
permission store or an alternative game writer domain. The production manager uses
the shared-domain seam for activation, removal, dynamic overrides, reload and their
existing recovery publications.

U3f-c does not connect the prepared plan to a task's cancellation/authority/context
fence. PROP-T06 integration, production permission provenance, factual SP/MP context,
live postcondition completion, Base facade cutover and SDK promotion remain separate
gates. Generic task tests do not establish these missing integrations. Registry
publication continues to be distinct from live resource application and persistence.

### U3f-d same-task native publication

The private typed route uses the existing opaque task and RuntimeScope grant store.
Its reservation order is actual scope writer domain, registry publication guard,
SemanticBridge context mutex, then short scope-state access. The domain, registry
and scope reservations are try-only. Context observation and Base retirement use
the same bridge mutex, so they cannot invalidate the captured context between its
validation and the typed publication. Scope cancellation and grant revocation use
the same scope mutex as the marker/publication. Same numeric context slots and
generations do not revive an earlier captured epoch.

The native route checks original scope/task issuer, input, schema, runtime epoch,
captured Base/context lease and prepared registry generation before effects. A
private native descriptor flag selects this route; generic dispatch refuses that
flag, and typed dispatch refuses an ordinary callback descriptor. The production
catalog remains empty. A native invocation is a trusted Core record, not a public
script/MCP message or an additional security sandbox for native DLLs. This unit
does not qualify a production owner/source-bound plan issuer or permission grant.

The existing task's reserved wire and arena encode/decode a bounded receipt before
the irreversible marker. The internal receipt schema is
`simoder.registry-publication.v1`, with exact `u64` registry generation and domain
revision plus `registry-committed` and `live: not-requested`. It is not a promoted
SDK declaration. A hidden prepared receipt is not delivered if a precommit stop or
failure wins. Advancing the domain, marking the same task and publishing the typed
plan occur together under the scope reservation. Only actual publication exposes
the success receipt. An unexpected failure after the marker is Indeterminate.

Input and reserved/abandoned result storage remain charged until later owner
cleanup; no reclamation is introduced under the irreversible publication guards.
The caller retains the plan's retired registry stores until all guards release.
Registry-only success does not claim native live refresh, disk persistence or
rollback. Revocation after success can hide delivery without changing the outcome.
The existing Lua presentation independently revalidates captured context on result
access; calling the lower-level native task store is not a replacement for it.

Host tests qualify isolated before/after cases and bounded concurrent context races;
the latter do not guarantee that every run schedules both winners. No deliberate
game fault, exact in-game lifecycle stop interval, real SP/MP observer or production
permission provider is qualified here. Atomic endpoint/domain stop may lose after
the last native fence; it is never presented as retracting a committed operation.

After U3f-d acceptance, the next proposed unit is U3f-e: discover and specify the
production PROP plan issuer, owner/source identity, user-approved permission
provenance and actual context observer, then independently audit the implementation
plan before enabling a provider. Base facade cutover and SDK promotion depend on
those gates. VM and deliberate crash tests remain parked; MCP authentication/cutover
and actual game qualification are later gates.

### U3f-f closed native owner/source contract

Typed semantic dispatch accepts only a move-only `OwnedPublication`; the earlier
raw `PreparedPublication` overload is removed. The wrapper prepares its registry
candidate internally through the existing owner-local reducer. It never seals a
caller-prepared foreign-owner plan. Its native origin derives the descriptive owner
ID, VM generation, endpoint and immutable loaded definition from the actual retained
ScriptRuntime, rather than accepting a caller's owner string or definition claim.

Semantic-bound runtimes copy their complete ScriptDefinition before initialization:
entry bytecode, local modules, aliases, revision and execution limits. Initialization
uses that pinned definition. The additional host-side copy is separately charged to
the existing copied-storage pool using bounded payload plus conservative metadata
accounting; the VM allocator budget does not cover host storage. The existing 4 MiB
owner and 16 MiB shared copied-storage limits still apply. Legacy runtimes without
a semantic endpoint retain no additional definition copy. This process-local pin
is not a durable installation identity, publisher signature, canonical security
digest or immutable snapshot of raw resource files. The captured source fingerprint
is correlation only. External raw-resource/dependency identity remains unqualified.

An origin starts Candidate. Trusted isolated fixtures may prepare startup data but
cannot publish an ordinary operation until the actual runtime is committed/running,
the original scope is committed and unstopped, and the original endpoint is open.
Active/Retired transitions use the same scope writer domain as publication; retirement
is irreversible. These isolated predicates do not qualify ModManager record commit
or a production lifecycle producer. No production issuer, Base binding, grant source,
human-decision adapter or SP/MP observer is enabled by this unit.

Each owned plan retains the exact original endpoint, runtime, loaded snapshot,
task, method, schema/input, operation identity and full factual context lease.
Publication establishes the original domain before reading mutable origin state,
then rejects altered, foreign, candidate, retired or stale-baseline plans. Existing
RuntimeScope authority, registry generation and context fences still apply: an owned
origin is not a grant. The existing task and receipt complete without a replacement
task, permission store or scheduler.

Each registry-owned patch set additionally retains a separate opaque incarnation
key, not an origin/runtime pointer. Existing preparation and publication algorithms
preserve this key atomically. Another legitimate same-ID VM cannot replace or clear
that set using its own request and plan; legacy unsealed writers cannot adopt sealed
sets, and owned origins cannot adopt unsealed legacy state. Ownership transfer and
full lifecycle removal are not introduced here. Property clear remains an ordinary
grant-checked owner-local reduction, not an exemption for privileged cleanup.

Native declarative patches remain separate from the mutable scripted baseline.
Preparation captures the immutable current scripted baseline under a short domain
reservation, then reduces outside it. Successful publication swaps the preallocated
next baseline under the same writer; a stale competing plan fails, and failure never
advances the baseline. Retired vectors and registry stores remain in the plan until
all reservations release. Set/clear applies only to the native origin's patch set;
declarative fields and other owners survive. Fixture command vectors are trusted
synthetic native producer output: this is not qualification of production payload
decoding, manifest requests, permission decisions or live-game application.

Only separately compiled test authority can issue or transition origins and prepare
owned publications. Production retains an empty catalog and no origin producer.
Legacy PROP bindings remain operational outside the semantic grant route until the
explicit joint cutover; this unit does not claim universal permission enforcement.
The next dependency is typed provenance/lifecycle qualification with closed adapters,
followed by independently qualified native context observation and public cutover.
