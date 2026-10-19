# VM and scheduler publication reservations

Status: **Accepted U3f-l contract and U3f-l1 isolated implementation. U3f-l2 joint
implementation passes isolated host verification. Independent/human acceptance
is recorded with the exact candidate. No production activation.**
This refines [MOWN-03/04/05/06](manager-owner-integration-contract.md) using the
actual existing script lane, VM, scheduler and semantic bridge. The accepted
[captured candidate](captured-candidate-contract.md) remains disconnected from
ModManager. [Owner/permission requirements](permission-owner-lifecycle-contract.md)
continue to govern grants and exact sealed ownership. Internal conceptual names
below are not public SDK types or manifest fields.

## SPUB-01 — Observed source graph

Source references identify symbols rather than moving line offsets. Legacy
publication remains distinct from the isolated typed route qualified below.

| Current operation | Actual acquisition/effects | Source |
| --- | --- | --- |
| Lane selection/control | Lane mutex selects and retains owner; releases before work/Lua/tick; reacquires for completion | `script_runtime.cpp`, `ScriptLane` |
| VM construction | Fixed lane slot/generation reserved before synchronous initialization | `script_runtime.cpp`, `ScriptRuntime` constructors |
| Candidate initialization | Entry/onEnable execute on lane; internal running becomes true; ordinary committed visibility remains false | `ScriptRuntime::Initialize` |
| Scheduler Prepare | VM mutex then lane Bind mutex; no Lua; assigns host and weak published owner | `ScriptScheduler::Prepare`, `ScriptLane::Bind` |
| Legacy VM Commit | VM mutex then bridge Publish, then status mutex; sealed typed owners rejected | `ScriptRuntime::Commit` |
| Automatic tick | Original selected registration retained through actual VM entry; VM mutex spans semantic pump/Lua; sinks later run outside VM mutex | `ScriptRuntime::ScheduledTick`, `TickSelected`, `ExecutionEligible` |
| Base dispatch | Lua under VM mutex can reach original domain through a nonblocking writer attempt | `semantic_lua.cpp`, `semantic_bridge.cpp`, `runtime_scope.cpp` |
| Writer primitive | Default try-lock; explicit wait=true is a distinct legacy behavior | `RuntimeCommitDomain::ReserveWriter` |
| Legacy Put/Remove | Scheduler mutex swaps map owners/atomic scheduling flags; retained old owner released after unlock; sealed typed route rejected | `ScriptScheduler::Put`, `Remove`, `ReleaseReservation` |
| Typed publication | Shared scheduler reservation; standalone D/S/V/T or isolated joint D/S/V/R/B/Q/T; one revision and deferred reclamation | `SchedulerPublicationReservation`, `OwnerLifecycle::CommitPrepared`, `scheduler_publication.hpp` |
| Stop/Close | Sticky stop request at entry; off-lane control mutex and lane wait; on lane VM lock, independently owed onDisable/semantic cleanup/Lua close | `ScriptRuntime::Stop`, `Close`, `Fail` |
| Shutdown | Stops admission and unschedules under scheduler lock; releases it before WaitIdle/Stop/Flush/Close | `ScriptScheduler::Shutdown` |
| Manager publication | Legacy manager unlocks before waitful writer, relocks and publishes registry; later updates VM/scheduler/record | `src/mods/mod_manager.cpp`, legacy lifecycle methods |
| Isolated owner handoff | l2 joins actual scheduler/VM to domain, registry, context and canonical scopes; actual manager M remains unqualified | `src/mods/owned_publication.cpp`, `OwnerLifecycle::CommitPrepared` |

Existing callbacks therefore already have VM→bridge/scope edges and an explicitly
try-only VM→domain path. A universal claim that all present code follows a future
domain-first blocking hierarchy would be false. This proposal defines the new
publication protocol and its necessary consumer changes, not retroactive proof
of deadlock freedom or new authority for legacy publishers.

## SPUB-02 — One scheduler and exact prepared ownership

Keep the existing single ScriptLane, fixed128VM/control slots, scheduler owner map,
RuntimeScope grant store and PatchRegistry algorithm. No additional lane, dispatch
queue, permission database or independent current-owner map is introduced.

A native move-only publication plan retains the actual scheduler/host/lane,
original commit domain and exact canonical manager record revision; old and
candidate VM objects/generations; actual reserved lane-slot identities; native
scheduler-node identity/revision; captured content and endpoint/scope pins; purpose
(initial activation, handoff or removal); expected registry/domain revisions;
prebuilt status/retirement metadata and old owning references. Names or raw addresses
alone never identify a reservation. Retain objects until all guards release.
Lane-slot reuse after Close cannot satisfy an old plan even at the same index.

Prepare map capacity, candidate host binding and weak lane owner before the fence.
No scheduler activation is implied by Prepare or lane Bind. Empty map reservations
have a nonreused native revision/identity; remove/recreate of a textual owner invalidates
old plans, including empty-slot ABA. Textual mod ID and diagnostic VM generation are
not caller-issued capabilities. Unsupported/foreign domain, scheduler, host, lane,
record, candidate, purpose or slot is rejected before effects.

The plan never issues a RuntimeScope grant. Candidate ordinary operations/ticks
remain NotReady. Initialization may produce only the previously defined bounded
staged declarations; the existing initializer is not thereby declared pure.
CAP identity limits do not bypass the existing2–64MiB perVM,128MiB aggregate,
copied-value or registration budgets. Retiring VMs continue to consume their real
slot and memory until lane Close/Release, not merely until map replacement.

## SPUB-03 — Complete new fence order and wait rules

The combined proposed order is:

`D domain → M manager record → S scheduler → V canonical VM guards → R registry → B bridge context → Q canonical scopes → T canonical status guards`

| Participant | Reservation rule |
| --- | --- |
| D | Original domain, nonblocking ReserveWriter; recursive acquisition rejected |
| M | Future actual manager-wide mutex_, try-only; canonical record membership/lifecycle/content/revision recheck; absent from isolated l2 |
| S | Actual existing scheduler mutex, try-only; stopped/slot/revision recheck |
| V | Exact old/candidate VM mutexes, try-only, ordered by immutable native VM generation; generations must be distinct and objects retained |
| R | Existing registry publication try-guard and exact prepared plan |
| B | Original actual shared bridge context mutex, try-only; same bridge/epoch and current leases |
| Q | Actual scope guards ordered by std::less<RuntimeScope*> on retained objects, as in the existing isolated lifecycle |
| T | Only if publishing status fields: actual old/new status mutexes, try-only, same immutable VM order |

Release in reverse order. Any contention or stale/closed/exhausted participant
rejects before the marker, releasing reservations with no effects. Do not wait or
spin while retaining earlier locks; no automatic retry/replay. Preparation and
explicit deferred cleanup can use existing off-lane synchronous facilities only
after releasing every publication participant.

The lane mutex and VM control mutex are **not fence participants**. Do not call
Prepare, Commit, Put, Remove, Stop, Fail, Close, Status, Invoke, WaitIdle, Flush,
callback sinks, log formatting or arbitrary functions from inside the fence.
Use typed guard methods operating on already reserved native state. No script,
allocation, map insertion/erasure, Lua GC, file/network work, blocking join or final
shared-owner destruction is permitted after the last fallible check. Retain old
map nodes, weak/shared references, envelopes and candidate/retirement storage until
guards are gone; shared_ptr assignment alone is not proof of no reclamation.

The existing VM→domain semantic-dispatch attempt is a narrowly audited exception
to forward rank traversal: it must remain try-only, release on Busy and never
request writer(wait=true), sleep, wait for publication or automatically replay.
Publication itself likewise only tries V, so a VM-held callback and domain-held
publisher cannot wait on each other. No new reverse blocking edge is allowed.
The existing legacy waitful manager publisher cannot serve a sealed owner.

Lane selection must keep its current rule: retain a real VM owner while holding
only lane mutex, then release lane mutex before trying/entering VM execution.
Preparation's VM→lane Bind remains outside the fence; new publication never takes
lane mutex while holding V, R, B, Q or T. Status-only readers cannot acquire another
participant afterward. Multi-VM cross-calls are forbidden inside callbacks;
requests remain copied/queued rather than synchronous entry to another VM.

## SPUB-04 — Validation, marker and visibility

All fallible work precedes the marker: validate original domain can advance, exact
current manager/owner/content and scheduler-node stamps, lane/VM identities,
uncommitted healthy candidate, old retirement eligibility, bridge/base epoch,
current decision reservations and factual context, existing registry CanPublish,
and capacity already retained by the plan. Validate using the sole RuntimeScope
evaluator; its private candidate mode does not open ordinary operation admission.

The marker uses the original domain revision advance. Below it only typed scalar
changes and swaps of preallocated state are allowed. The joint transaction must:

1. Publish the direct prepared registry/manager ownership change, preserving
   activation order and unrelated owners, with no intermediate deactivate/activate.
2. Retire old operational admission/epoch and cancel eligible pending work with
   the existing accounting rules, without destroying its retained storage.
3. Clear the old VM's execution-visible committed/scheduled gate and replace the
   exact scheduler anchor with the candidate or prepared removal state.
4. Set candidate committed state and scope activation, then expose only that exact
   registration as scheduled. Publish fixed status running fields under T if used;
   no new diagnostic strings or callback entry occurs there.

These steps are hidden from qualified execution/record readers by retained guards;
the order of scalar writes does not license an unguarded reader to observe authority.
Initial activation/removal use their exact absent/current expectations; handoff
advances once. Any defensive post-marker inconsistency leaves admission closed
and records actual committed/cleanup-pending or Indeterminate state from prebuilt
metadata; it never activates an unvalidated candidate or replays a partial effect.

Internal initialized/running, VM committed, scheduler eligible, RuntimeScope
activation/readiness, Base readiness and operation permission are distinct. Running
status is diagnostic only. The complete guarded view promises old or successor;
raw registry snapshots, atomics sampled separately and Status() do not constitute
a cross-object authority proof. Runtime readiness may reflect only already verified
native preparation; semantic game context/permission providers remain closed.

Ordinary per-mod publication must not call SemanticBridge::Publish or republish
the Base singleton readiness lease. Base bootstrap Publish/Close is a separate
existing lifecycle. A per-mod retirement closes only that endpoint/scope; it cannot
close the shared bridge or erase the successor's Base epoch. Base close remains
sticky and invalidates dependent admission; a reservation cannot reopen it.

## SPUB-05 — Execution and legacy mutation participation

A lane may have selected and strongly retained an old VM just before unscheduling.
The selected dispatch therefore carries exact VM/slot/registration generation and
must recheck current execution eligibility under V immediately before any semantic
pump/Lua callback. scheduled=false alone is insufficient. Apply the same rule to
explicit native Tick/control execution routes used for a sealed owner. No lane
lock is retained while taking V. An already executing callback holds V: publication
returns Busy rather than waiting or interrupting/replaying it.

If a retained old selection reaches V after a successful handoff, it runs no Lua.
Checks include native retirement/stopped state, exact committed registration and
scope/domain lifetime; an old selection cannot adopt a successor stamp. A stop
winning after the marker can close further admission without rewriting a committed
result. Current operation permissions are still checked at their original task
commit/delivery boundaries; the scheduler gate is not another grant evaluator.

Commands already extracted before replacement retain their exact original VM,
owner/content and request identity. The outside-VM sink must revalidate those
under the same original domain/canonical record before publication. A stale batch
is denied/settled, never relabeled to the new owner or retried through the textual
legacy writer. Log/diagnostic sinks may drain outside fences but confer no authority.

Existing direct Commit/Stop/Fail and textual scheduler Put/Remove/ReleaseReservation
must either participate in the exact registration revision/closure protocol or
be unreachable for sealed owners. Do not leave a second mutator capable of undoing
the new visibility gate. Keep legacy unsealed behavior outside this qualified
route until the separately approved joint migration; never fall back after sealing.

## SPUB-06 — Retirement, abort, stop and reclamation

Preallocate a native retirement obligation independent of execution-visible
committed/running flags. It records the exact old generation and whether onDisable
is owed/consumed, plus cleanup state, not a new task or permission store. A committed
VM retired at the marker owes at most one onDisable even after committed is cleared
or Fail sets running=false. A never-committed candidate owes none.

After all publication guards release, existing lane/control facilities consume
that exact obligation once, execute permitted teardown callbacks, flush or discard
old copied messages, stop semantics/coroutines, close Lua and release slot/memory.
Adapt current Stop's early return so it cannot skip owed retirement. Aborting an
uncommitted candidate still requires full deferred Close; Stop alone does not free
its VM. Never perform final VM/lane owner release while holding publication locks
or from an unsafe lane-origin control path. Plans/retirement holders must be
released by an off-lane native owner outside all fences; failure retains a bounded
cleanup-pending holder rather than inventing a reaper/thread or leaking unlimited
retirements. Capacity exhaustion denies new preparation until genuine close/release.

Blocking or faulty onDisable cannot hold D/M/S/R/B/Q/T or cause revival. Native
callback blocking is not preempted by the Lua instruction interrupt; no hard wall
time guarantee is claimed. Cleanup failure retains diagnostic/closed ownership
and resources as needed; it cannot return an unsafe slot to the free pool.

Scheduler shutdown serializes its stopped latch/node retirement against S-held
publication. A shutdown winner invalidates prepared plans; a publication winner
is later closed by shutdown with a retained obligation, never reactivated.
Global domain stop retains existing sticky semantics; no AdvanceRollback bypass
is added. Set closure/unscheduling under the qualified native protocol, then
WaitIdle, lane invocation, Stop/Flush/Close and final joins only after every
publication lock is released. Shutdown must not wait while holding D or M for
an in-flight callback whose outside-VM sink needs that domain/manager.

## SPUB-07 — Proposed implementation units and negative oracles

Acceptance of this design does not itself authorize production integration.
**U3f-l1** implements isolated typed reservations/closure/retirement in
the existing scheduler and VM, with fixture producer only, no actual ModManager
cutover or production authority. **U3f-l2** qualifies joint publication
with existing registry/bridge/scope participants in the isolated host. Actual
canonical manager integration remains MOWN's U3f-m and needs separate approval.
Any record binding in l1/l2 is a closed native fixture model, not a qualified
canonical manager producer. Its later integration must use the actual manager
record, replacing model authority rather than keeping parallel current-owner state.

| Test family | Required observable oracle |
| --- | --- |
| Identity/replay | Foreign scheduler/host/lane/domain/record/VM/slot/purpose, reused empty node and consumed plan denied; current owner unchanged |
| Preparation/limits | Candidate not operational; storage/slot/VM-budget/retirement-capacity failure before effects; no fake grants or extra lane |
| Contention/reverse edge | Paused old Tick holding V causes bounded Busy; VM→occupied-D dispatch returns Busy; every participant lock independently contended with no partial effects |
| Selection gap | Pause after lane selection/before V; successful handoff then old dispatch performs no Lua; already executing callback makes writer Busy |
| Sink gap | Pause extracted old batch before outside-VM sink; handoff then stale batch cannot publish into successor |
| Atomic publication | Guarded old/new view coherent; once-only revision/order-preserving replacement; candidate eligibility, scope/record and scheduler anchor agree |
| Lifecycle failure | Failed candidate preserves still-approved old owner; old revoke/Fail/stop remains sticky; post-marker closure never reported as rollback |
| Retirement | Independent owed flag; once-only onDisable/Close after handoff/Fail; candidate abort no onDisable but full memory/slot release; no last-owner destruction at fence |
| Shutdown | Stop/close/reservation loss on each side of marker; selected/in-flight owner retained; shutdown waits outside fences; safe eventual close and no stale plan activation |
| Base/context | Base ready generation unchanged on per-mod publication; old endpoint retirement cannot close shared bridge; Base/context closure denies candidate without resurrection |
| Instrumentation | Detect allocations, frees/destructor callbacks, Lua entry, sinks and lane/control waits inside fence; typed methods alone below marker |
| Regression/distribution | Existing24hosttests retained, unchanged grant/registry/scheduler invariants, production TestAccess/fixture phase symbols absent; actual x86 DLL identity checked |

Do not test these by calling new legacy wrappers that bypass the typed path.
Tests must exercise actual lock/selection/dispatch/reclamation boundaries with
bounded deterministic barriers and real isolated Luau VMs. No game, OS VM, native
crash or environment installation is required. No assertion weakening or simulated
PASS can substitute for an unqualified consumer/interleaving.

## SPUB-08 — Verification status and responsibility

U3f-l1 adds a private move-only publication plan to the actual existing scheduler
and VM. Only the test-authority build can enroll its original domain and issue
plans. Its isolated fence is D/S/canonical V/canonical T; l1 alone does not qualify
joint registry/bridge/scope publication (see l2 below) or actual manager integration.
Prepared and retired
owners retain their actual lane slots until reclamation on the existing lane.
Typed command batches are denied rather than delivered to the legacy command
sink; this does not qualify a production command consumer. No RuntimeScope grant
is issued. Direct legacy mutation cannot activate a sealed typed owner.

The retained scheduler state owns the same map and lock, preserving plan safety
after the stack scheduler is destroyed without adding a second current-owner
authority. Reclamation, callbacks and lane waits remain outside publication
guards. Deterministic isolated tests qualify selection, contention, cancellation,
retirement and capacity boundaries; they do not establish arbitrary native callback
preemption or actual game behavior. Final test results and independent acceptance
must be read from the exact candidate evidence rather than inferred from this
contract. Historical U3f-k24/24tests prove only its accepted candidate. U3f-l design
evidence remains in `.analysis/u3fl-2026-10-16/`; current implementation hashes,
test evidence, preservation and verdict are in ignored `.analysis/u3fl1-2026-10-16/`.

U3f-l1 Win32 Release verification passed all25hosttests, including364scheduler
publication checks,96immutable candidate checks and1178semantic bridge checks.
Production archive inspection found zero test-authority/capture-phase/scheduler
fixture symbols; the fixture archive contains them. This qualifies only the
disconnected native route described above, not l2 joint publication or gameplay.

DLL Core owns this plumbing. Luau Base keeps high-level facades/presentation;
SDK and DevTools consumers/public schemas are unchanged. This adds no end-user
dependency, executable, service, compiler, installer, network access, persisted
consent or active instruction. Production authenticated ingress, native factual
observers, durable identity/recovery and actual game postconditions remain later
gates. VM, crash trials and OpenSCP PR #7 remain parked.

## SPUB-09 — Isolated joint publication boundary

U3f-l2 composes the actual scheduler/VM reservation with the existing private
OwnerLifecycle record, prepared PatchRegistry publication, original bridge/context
leases and canonical RuntimeScope guards. It shares typed scheduler validation and
scalar application with l1, and shares the lifecycle algorithm with the earlier
isolated owner model. It must not call two independent commits in succession.

One original D revision is the publication marker. The model has no canonical
manager M; its record stamp remains serialized by D. Reservations use S/canonical
V/R/B/canonical Q/late canonical T, released in reverse order. Candidate admission
still uses the sole existing RuntimeScope evaluator, with its current native
decision and factual leases. Initial activation, handoff and removal do not issue
grants or republish shared Base readiness.

Exact joint binding seals the retained record, scheduler node and originating
incarnation. Once bound, preprepared and later standalone paths cannot publish
one side independently. Binding metadata confers no execution eligibility or
permission. Retained nonreusable identity tokens must not create record/scheduler
ownership cycles. Rejected or abandoned candidates remain reclaimable outside
publication guards using the existing lane and real slot accounting.

Release-only removal validates exact ownership rather than requiring operational
permission or factual readiness after revoke/Fail/Base close. Exact repeated
tombstone removal has no second domain/registry effect. A defensive post-marker
inconsistency must close execution admission, retain cleanup ownership and expose
Indeterminate/cleanup-pending state without activating the candidate.

Only a guarded view of actual participants qualifies joint atomicity. Separate
raw snapshots, atomics and diagnostic Status calls do not establish that property.
L2 verification and final independent verdict are recorded under ignored
`.analysis/u3fl2-2026-10-16/`. Win32 Release passed26/26hosttests, including858joint
publication checks in that run and the retained364scheduler/96candidate/1178bridge
checks. Concurrent observation counts may vary; deterministic contention and
preparation-race barriers qualify their named boundaries rather than exhaustive
interleavings. Production archive inspection found zero fixture issuers or joint
preparation-barrier symbols. Actual
canonical manager integration, command-consumer cutover and production permission
ingress remain separately gated.

Preparation first retains both plans, then one D/S/V metadata reservation binds
record/origin/node together after exact revalidation, without advancing D. Failed
foreign or competing preparation cannot seal another origin or acquire its VM's
reclamation responsibility. A successfully prepared native candidate is retained
for off-fence reclamation; an input rejected before ownership remains caller-owned.
The qualified reader uses actual try-only D/S/V/R/Q/T and copied scalar scope/status
facts, not blocking Current/Status wrappers under a publication reservation.

Host C++ allocation/free and lifecycle probes supplement source review; they do
not intercept every C/realloc/Lua allocator. The defensive post-marker failure
branch is source-reviewed and closes execution admission, but has not been forced
by fault injection. Native callbacks are not preemptively isolated. No actual game
or production permission acceptance is inferred from these host results.
