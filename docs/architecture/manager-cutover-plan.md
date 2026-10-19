# Canonical ModManager cutover plan

Status: **Plan, implementation and audit accepted through U3f-m4b; U3f-m5a plan
and audit accepted. Isolated U3f-m5b1 close foundation passes host verification;
its exact audit and human acceptance remain separate.
Exact audit/acceptance is recorded separately.
No production route activation.**
This refines [MOWN](manager-owner-integration-contract.md) using accepted
[SPUB/l2](scheduler-publication-contract.md). Acceptance of this plan authorizes
only the next explicitly approved implementation unit. Native identifiers below
are private conceptual interfaces, not SDK exports or a new manifest schema.

## MCUT-01 — Source-verified entry gates

| Gate | Source fact at the m1 baseline | Required change before cutover |
| --- | --- | --- |
| Canonical owner | `ModManager::Record`, value in `records_`, guarded by manager-wide `mutex_`; no lifecycle stamp | One retained native entry identity and one current immutable stamp; no parallel fixture record |
| Resources | Manager borrows registry/cache; loader resets manager before cache/registry | Retained manager-state anchor plus closed/drained native operation leases before borrowed-resource destruction |
| Candidate | Refresh uses `LoadStableCandidate`; manager does not consume CapturedCandidate | Retain exact captured inputs/output, installation binding and actual Base pin; revalidate pins, never reopen source paths for execution |
| Initialization | Entry/onEnable run before publication; manager readFloat reads live cache | Explicit restricted preparation phase; bounded staged declarations only, no live reads/effects |
| Publication | `PublishPrepared` publishes R before separate scope/VM/scheduler changes | One typed D/M/S/V/R/B/Q/T transaction, shared with l2 |
| Commands | Constructor installs `ApplyScriptCommands`; typed batches currently deny its sink | Keep denial until an exact canonical-owner consumer is separately qualified |
| Persistence | Enabled-ID file is replaced before registry commit with compensating writes | Explicit disk-intent/compensation policy outside all publication locks; no claim of atomic disk+memory commit |
| Production | Authenticated decisions, native facts and trusted installed Base binding are not qualified | Keep production issuance/catalog/context ingress closed; fixtures never supply production consent |

Primary implementation sources are `src/mods/mod_manager.hpp/.cpp`,
`src/mods/owned_publication.hpp/.cpp`, `src/mods/captured_candidate.hpp/.cpp`,
`src/scripting/script_runtime.cpp`, `src/scripting/base_host.cpp`,
`src/scripting/semantic_lua.cpp`, `src/mods/mod_state_store.cpp` and
`src/bootstrap/loader.cpp`. Exact line/hash evidence is in the private m1 record.

## MCUT-02 — One actual record and resource lifetime

Retain the existing canonical map and its manager-wide M in a native shared
manager-state anchor, rather than adding a second map or a per-record mutex.
Each actual entry has a nonreusable native incarnation. A plan retains that
entry/stamp and the original manager anchor; under M it verifies exact map
membership and expected revision. Erasing/recreating the same textual ID cannot
validate an old plan or borrowed iterator. Old immutable stamps are observations
or retirement holders, never independently writable current records.

Factor the l2 model's stamp/reservation into a shared typed participant. The
actual Record owns the sole current stamp; do not attach another independently
mutable `OwnerLifecycle::Record` beside it. A closed fixture adapter may reference
that actual participant, without copying authority. The common lifecycle/registry/
scheduler algorithm remains one implementation, with typed scalar swaps instead
of arbitrary callbacks at the marker. Sealed VM/scope/patch facts come from its
single stamp; any legacy fields serve only the disjoint unsealed route or derived
read-only projections, never a second writable sealed current state.

The shared domain/process owner does not own borrowed registry/cache storage.
Use native active-operation leases for preparation, transaction readers/writers,
delayed sinks and postcommit work. Closing the manager rejects new leases;
shutdown drains existing users before loader resets borrowed resources. An atomic
stop check alone is insufficient: a check-then-dereference race must be tested.
Waiting/draining occurs outside D/M/S/V/R/B/Q/T and outside the script lane.
Plans retained past shutdown inspect the retained closed anchor before touching
borrowed resources and deny safely; their actual owned VM/capture cleanup remains
possible. Do not wrap borrowed pointers in no-op shared_ptr ownership.

Replace or revoke raw `this` callback reachability, including ScriptHost and the
scheduler command sink, under the same lease protocol. Preserve current loader
order (manager before cache before registry) and action-worker/scheduler quiescence.
Record map growth/erase and final storage destruction stay outside the marker.

## MCUT-03 — Captured input and restricted initialization

First eligible sealed owners are dependency-free, script-bearing Luau mods with
supported declarative patches and a retained actual Base package. Reject services,
raw/native payloads, external tools, unsupported languages and unresolved dependency
closure before VM startup. Declarative-only owners need a separate native identity
unit; do not fabricate a VM or an operation grant for them. An unsupported candidate
cannot use the legacy route after its owner is sealed. Never adopt an active legacy
owner merely because its text ID matches; first admission starts from an explicitly
quiesced/inactive canonical entry with a fresh reviewed incarnation.

Retain CapturedCandidate's consumed definition, module bytes/bytecode, content
digest and installation binding. Bind the actual BaseHost package and original
endpoint/scope; a structurally valid Base pin is not publisher trust. The current
capture qualification is not an authenticated durable installation identity or a
complete cross-mod dependency resolver. Changed pins reject/review the candidate;
they cannot silently reuse consent.

Add a native, non-manifest-selectable preparation phase to the existing VM. It
permits only bounded candidate-local module loading, existing bounded diagnostics
and staged PROP set/clear declarations, reduced by the existing patch reducer
before publication. Validate their normal target/value/count budgets. Initialization
cannot tick, submit operational semantic tasks, read live cache/game values, start
services/native code, access file/network APIs or issue grants. Unsupported calls
fail with diagnostics; no fake successful adapter or fallback hides rejection.

Current setters require `host_.readFloat` presence even though they stage commands;
BaseHost::Bind supplies no such reader. Split declaration admission from live-read
capability explicitly. Do not install a dummy reader just to pass the old check or
relabel existing entry/onEnable as pure. Tests must prove reads never reach the
live cache, valid set/clear still stage bounded output, and ordinary operation
admission remains NotReady. Publication still requires the sole RuntimeScope
decision evaluator; producing declarations does not grant permission to apply them.

## MCUT-04 — Complete canonical publication

Acquire the original D nonblocking, then actual manager-wide M, S, immutable-
generation-ordered V, R, original B, pointer-ordered Q and late V-ordered T.
All acquisitions are try-only and release in reverse order. The fixture record
had no M; canonical integration must test the actual manager mutex, not invent a
record mutex. No waitful D acquisition is allowed under M/V; the only qualified
reverse edge is the existing VM-to-try-D semantic attempt, without wait/retry
inside it. Canonical publication obtains D before M and V.

Before effects validate native entry/membership/stamp, capture/Base pins, original
domain and revisions, scheduler token/slots, VM generations, registry preparation,
current decision/factual leases, stop state and retained retirement/order capacity.
Metadata binding seals canonical stamp/origin/node together under one D/M/S/V
reservation; competing legacy preparation cannot win between their assignments.
One D advance publishes prebuilt canonical record/order/registry/scheduler/scope/
status state. No allocations, Lua, callbacks, file work, formatting, waits, map
insertion/erase or final-owner release follows the last fallible gate.

Initial activation, same-ID handoff and exact removal use this one algorithm.
Handoff preserves unrelated owner order without deactivate/activate. Exact repeated
tombstone removal has no second revision. Release-only removal after revoke/Fail/
Base close checks exact ownership, not operational permission. Global domain stop
does not acquire an AdvanceRollback exception. Defensive post-marker inconsistency
closes execution and retains cleanup-pending ownership, never revives the predecessor.

Guarded canonical readers retain actual participant guards and use scalar try-only
reads; raw snapshots/Status are diagnostics. Base readiness is not republished by
per-mod handoff. A prepared candidate is not ordinarily operational before commit.

## MCUT-05 — Legacy separation and retirement

Audit every mutation entry: Initialize/Refresh/SetEnabled/Reload, missing-directory
cleanup, queued actions, ApplyScriptCommands, PublishPrepared including Rollback,
direct Commit/Put/Remove/ReleaseReservation, native refresh, stop and destruction.
Once an incarnation is sealed, none can mutate its current state through the
legacy algorithm or adopt an old textual request. Classification is explicit;
changed unsupported content fails closed and leaves the approved old owner or
closed cleanup state as appropriate. Unsealed legacy behavior is preserved within
its declared scope; fixture success cannot silently switch production routing.

Keep typed command batches denied until their original VM/capture/request and
canonical stamp are checked under the same original domain. Reuse owned reduction
and registry preparation; no relabeling a stale batch to its successor. Qualify
this consumer in its own bounded step before promising running PROP parity.

Preallocate bounded retirement holders. Stop/onDisable, lane flush/Close, actual
slot release, scope drain, service teardown, cache invalidation/native refresh,
diagnostics and final destruction happen outside publication locks. Candidates
rejected before preparation remain caller-owned; a prepared candidate is explicitly
reclaimable, never onDisabled. Retired committed callbacks execute at most once.
Capacity exhaustion rejects before publication; blocked cleanup cannot resurrect
old authority or silently free unsafe storage.

## MCUT-06 — Disk intent, failure and startup

Proposed first policy preserves the existing enabled-ID file as **requested order**,
not proof of in-memory commit, scope authority or durable consent. Prepare previous/
candidate order and serialized intent outside fences; write candidate intent off M
under the closed/drained native operation lease and lifecycle sequencing. Revalidate
canonical versions before the memory marker. A pre-marker rejection compensates to
previous intent off-fence only while the exact write/version is still owned. Failed
or superseded compensation is reported as a persistence mismatch, not success or
permission to overwrite somebody else's newer state.

After the marker, callback/refresh/diagnostic or persistence bookkeeping failure is
committed/pending or Indeterminate, never a legacy rollback/re-enable of old admission.
No disk I/O is added to the no-fail transaction. Existing atomic file replacement is
not a disk+memory transaction: a crash after intent write can leave requested IDs
whose activation never committed. Startup must recapture/revalidate content, Base,
route classification and actual consent/facts; persisted IDs cannot recreate grants.

Durable installation/route identity and crash-recovery policy are production gates,
not supplied by the process-local capture token or this ID file. In particular,
restart/changed manifests cannot turn a previously sealed owner into a permissive
legacy owner. Before production enablement, qualify a durable native route policy
or an explicitly approved deployment policy which cannot infer that fallback.
Stronger journal/schema changes require their own reviewed scope; this plan claims
neither a new journal nor crash-proof atomic persistence.

The [MPS persistence/shutdown contract](manager-persistence-shutdown-contract.md)
refines this proposal using the accepted m4b source. It specifies the cooperative
single-writer limitation, exact receipt/absence compensation, bounded volatile
cache settlement and request/poll/completed-close certificate. These are proposed
implementation requirements, not capabilities of the existing state store or
blocking destructor.

## MCUT-07 — Ordered implementation units and required evidence

| Unit | Bounded result | Required tests before acceptance |
| --- | --- | --- |
| **m2** canonical participant/lifetime | Retained actual map/M/entry stamp and native operation lease; share l2 participant; test-only producer on actual manager; production route unchanged | Actual M contention; entry erase/recreate ABA; stale queued plan; close-vs-borrowed-access lease; retained plan after manager destruction; raw callback revocation; no second current record; existing26regressions |
| **m3** captured preparation | Actual captured candidate/Base binding and restricted existing-VM initializer; still closed production | Source/module/bytecode/Base mismatch; no source reopening; live-read and semantic-task rejection; bounded set/clear parity; initializer exception/budget/allocation; early unsupported eligibility; no fake grants |
| **m4a** canonical joint lifecycle | Actual manager initial activation/handoff/removal using shared transaction; isolated harness only | D/M/S/V/R/B/Q/T contention; one marker/old-or-new view; all standalone/duplicate-record bypasses; stale captures/context/revocation; callback-once/abort/capacity; Base epoch preservation |
| **m4b** exact command consumer | Qualified delayed batch/current-owner participation, with reducer/preparer reuse; no production issuer | Selected/queued/extracted old batch; actual sink pause; same-ID successor; close/revoke/context loss; denied payload/result and no legacy fallback; running PROP parity only within the qualified fixture scope |
| **m5** persistence/shutdown gates | Off-fence intent/compensation and committed/pending results; closed/drained teardown; explicit production eligibility checklist | Write/rename/compensation failure; superseding state; interruption before/after marker; stopped-domain retention; refresh failure after commit; no consent from IDs; borrowed-resource destruction order and bounded retirement |

Each unit requires bounded ownership, source/fixture/artifact preservation, focused
tests, full relevant regressions and independent audit with explicit human acceptance.
Fixtures are compiled separately; production symbol checks must remain zero for
test issuers/barriers. No new external program or end-user dependency is proposed.
M2, m3, m4a and m4b were separately implemented, audited and accepted. The current
implementation unit is **isolated m5b2b2 current-record VM retirement** after independent
plan PASS; m5b2a exact scope retirement has independently passed. MPS-09/11/12
divide the remaining units and document the user's temporary
full-U3 intermediate gate delegation. Each candidate still requires an independent
PASS before continuation; final U3 acceptance remains human. FoundationReady is
not the full retirement/release certificate. Loader routing and production
deployment remain closed.

## MCUT-08 — Closed production gate and verification limits

Only after these isolated gates and separate explicit approval may actual bootstrap
or manager routing be enabled. Trusted installed Base, authenticated native decision
producer, factual SP/MP/lifetime observer, durable route/content identity and reviewed
descriptor catalog remain mandatory. No fixture grant/provider enters production.
Live verification then needs approved sandbox/context; it cannot be inferred from
host builds. DLL owns low-level plumbing, Luau Base owns presentation/high-level APIs,
SDK describes supported boundaries and DevTools owns authenticated tooling ingress.

The m1 unit changed design documentation only. It did not rebuild or rerun runtime
tests: accepted l2 evidence (26/26,858joint checks) is historical for its exact candidate.
Source/reference/preservation checks and the independent plan verdict live under
ignored `.analysis/u3fm1-2026-10-17/`. VM, crash trials and OpenSCP PR #7 stay parked.

## MCUT-09 — Isolated m2 structural qualification

`ModManager::State` now retains the same canonical value-map and manager-wide M;
manager members alias those objects while the manager lives. Entries embed a
private `OwnerParticipant`, sharing stamp/binding representation with the l2 model.
The actual entry's nonreusable identity and current stamp can be retained and
revalidated against actual map membership under try-M by fixture-only observers.
No second OwnerLifecycle::Record is attached to the canonical entry.

This observer validates identity/stamp only. Legacy Refresh may change definition/
fingerprint in the same entry without replacing that stamp. It is not a content,
full legacy-state, decision or publication proof. The l2 transaction algorithm
does not yet validate actual manager M/entry; capture/version binding and sealed
legacy bypass guards remain m3/m4 work. No actual canonical joint lifecycle or
command-consumer cutover is claimed.

A separate native lease gate counts resource users, not permissions. Public
borrowed-resource methods and weak-state host/scheduler/action callbacks acquire
at execution time. Destructor sets sticky runtime stop, closes admission/revokes
manager reachability, waits for current users outside publication locks and lane,
then stops scopes/workers/scheduler. The closed canonical map is detached under M
and destroyed off-M before loader frees registry/cache. Retained observations
cannot keep actual record services/scopes running. Runtime stop remains distinct
from destructor lifetime closure so ordinary scope-drain queries still work.

Native teardown must be called by an external owner off active leased callbacks,
script lane and D/M/other publication guards. A blocked native callback can delay
drain indefinitely. External owners keep borrowed registry/cache alive through
manager destruction; raw object calls require valid object lifetime. No hostile
native-code isolation, self-destruction support or process termination behavior is
added. Weak callback captures avoid state/map/VM-host ownership cycles.

Win32 Release verification passed27/27hosttests, including57actual manager lifetime
checks and retained364scheduler/96candidate/1178bridge/156scripting checks. The
unchanged joint suite passed818checks in this run; its concurrent-observation
count varies and does not represent reduced coverage. Named m2 cases include actual
M contention, stale stamp, erase/recreate ABA, admitted callback/public-reader vs
teardown, M/D availability during drain, closed/post-destruction callback denial,
anchor expiration and prompt actual scope retirement despite a retained plan.
Production archive checks found zero test issuers or manager fixture/barrier
symbols. Exact source/artifact/test evidence and independent verdict belong in
ignored `.analysis/u3fm2-2026-10-17/`. No new end-user dependency or production
provider is introduced; game/VirtualBox/crash/PR actions remain outside qualification.

## MCUT-10 — Isolated m3 captured preparation

M3 prepares an inactive actual manager entry using its own opaque installation
identity, immutable captured Luau bytes and the original retained Base host. The
entry identity/stamp and legacy fingerprint are separate freshness witnesses;
the fingerprint is not the capture digest and does not certify byte equivalence.
The captured package pin must match the original Base package. Preparation retains
native Base generation/epoch, endpoint, scope and meaningful context witnesses.
Any stale or closed participant rejects the prepared observation.

The native-selected preparation phase allows bounded local computation and staged
PROP set/clear declarations through the existing reducer. It denies live PROP
reads and host-facing semantic operations before callbacks or context inspection.
Reduced output contains script declarations only; captured authored patches remain
owned by the capture. Combining the complete effective patch set is future m4 work.
Ordinary commit and scheduler admission cannot activate a preparation VM. Abort
does not owe onDisable. Initialization and resource release occur outside manager
and publication guards, with a manager lifetime lease across preparation.

These observations and reduced declarations are not authority or a publication
proof. M4a must bind the actual canonical manager participant into the joint
transaction and revalidate under its ordered reservations. Production issuance,
registry/domain effects and actual scheduler publication remain closed. Exact
qualification and independent audit evidence belong in ignored
`.analysis/u3fm3-2026-10-17/`; source/build tests do not establish game acceptance.

Win32 Release build passed. The complete 28-test host suite passed in 33.35 seconds,
including 118 preparation checks, using an independently reviewed temporary
AboveNormal Windows job for the test driver and descendants only. Existing runtime
budgets and guards were unchanged. Normal-priority runs under saturated host CPU
failed the Base 2 ms callback deadline (one measured 5118 microseconds); earlier
legacy candidate-staleness failures were also retained. This qualification does
not establish reliable startup at normal priority under that load. No other
process priorities or services were changed. Production archive checks found zero
symbols in every fixture-only issuer/barrier group. The original dependencies
and all 256 unrelated baseline files remained unchanged.

## MCUT-11 — Actual canonical joint adapter

M4a uses a private retained binding for either the existing standalone fixture
record or the actual manager State and its sole OwnerParticipant. Both bindings
enter the same lifecycle algorithm. The actual route holds a counted native lease
and resolves map membership under D then actual try-M; it does not attach a second
record or keep an unlocked map pointer. Metadata sealing and the publication
transaction include M in their established order.

Qualified fixture preparation selects the existing fixture RuntimeScope before
Base binding and VM initialization, then keeps that original scope and endpoint.
Ordinary m3 preparation remains closed. Existing decisions are still evaluated by
the sole RuntimeScope mechanism. A closed native proof binds promotion to the
exact consumed preparation; the preparation role stays closed until successful
joint application. This is not a production issuer or a public SDK interface.

Active ownership retains immutable capture/Base/VM resources without retaining
State. External binding/plans retain the original State and resolve actual map
membership under its guard and a counted lease. All borrowed-resource access remains
leased. Global order and actual entry witnesses are revalidated; prebuilt scalar
or container swaps publish projections together, with retired resources released
after every reservation is gone. Legacy paths cannot mutate sealed owners.

Initial metadata sealing may be abandoned and retried by a fresh qualified
candidate while the exact empty entry has never committed. A removed sealed
tombstone stays closed to legacy and unqualified reactivation. Release-only removal
works after revoke, VM failure, scope stop or Base close; repeated exact removal
has no second marker. Canonical admission also observes the existing manager
lifecycle exclusion under M, preventing metadata sealing across an in-flight
legacy initializer or refresh. M-only preparation observations may allocate a
copied order outside D; compound publication reservations and the final marker
use prebuilt storage. Off-fence retirement remains explicit.

Win32 Release build and all 29 host tests passed at normal process priority in
14.32 seconds, including 2031 manager-publication checks and the deterministic
legacy-initializer race. Concurrent observation counts may vary. Existing 57
lifetime, 364 scheduler, 96 capture, 118 preparation, 1178 bridge and 156 scripting
checks remain passing. All nine production fixture-symbol groups are zero. The
exact candidate preserves 256 unrelated files and the existing Base/SDK inputs.
These results do not repair or disprove the historical 2 ms scheduling sensitivity
under saturated load reported for m3; no budgets or priorities were changed here.

Allocation/free and lifecycle probes cover the existing native test fence, not
every C/realloc/Lua allocation or arbitrary file/wait behavior. The defensive
post-marker Indeterminate branch remains source-reviewed rather than deliberately
fault-injected. Exact artifacts, limitations and independent audit are recorded
under ignored `.analysis/u3fm4a-2026-10-17/`. Production issuers, real readiness
observers, operational semantic request-context issuance, delayed command consumer,
persistence, game, operating-system VM and crash-test acceptance remain outside
this qualification. No new end-user dependency is introduced.

## MCUT-12 — Exact delayed command qualification

M4b qualifies only existing staged PROP Set/Clear operations through the actual
manager and scheduler in an isolated native harness. Live GetFloat remains closed:
declaration admission must not require a reader or install a dummy reader. The
legacy unsealed route remains separate. Production permission issuance and actual
game application are not established by this qualification.

An opaque native batch retains the original selected VM, scheduler registration,
owner node, immutable stamp, scripted baseline, Base binding and factual context.
Capture precedes externally exposed selection and extraction delays. An existing
reviewed PROP decision supplies a fresh bounded operation reservation before the
batch is exposed; this reservation cannot be renewed after a delay. Exact current
ownership and authority are rechecked before consumption. Looking up a successor
by textual mod ID cannot retarget old work or its diagnostics.

The command path reuses the publication reducer and registry preparation without
fabricating a semantic invocation. A single domain marker publishes the registry,
scripted baseline and canonical stamp/projection while retaining the same VM,
scheduler registration and order. Even an unchanged payload must validate the
original authority and current owner. Every outcome consumes the envelope once;
fresh operation reservations retire exactly on success, denial, contention,
exception or abandonment, outside compound guards. An identical complete scripted
state returns validated success without another domain marker; diagnostic accepted
outcome counters are not a substitute for observing the actual marker.

If Q is busy, exact reservation cleanup transfers once to fixed scope-owned
retirement slots without waiting or allocating. Off-fence Q acquisition drains
these slots; lifecycle and commit fences never drain them. The existing retained
reservation limit bounds pending cleanup. Defensive invariant overflow stops the
original domain and retains bounded cleanup state rather than claiming success.

Route references must not create State/VM/origin ownership cycles. Borrowed-resource
preparation, consumption and postcommit cache invalidation retain native manager
leases. Prebuilt values and retirement holders keep allocation, final destruction,
callbacks and waits outside the final publication fence. Logical registry commit
does not imply that live game resources were refreshed; that integration remains
a separate gate. A known postcommit cache failure reports Indeterminate for that
operation while preserving committed state. Durable pending diagnostics, repair
queues and retry policy remain m5 decisions; a later no-op success is not proof
that an earlier failed eviction was repaired. Existing textual-owner Lua logging
is not newly generation-tagged by this consumer. Stage evidence and verification
limits belong in ignored
`.analysis/u3fm4b-2026-10-17/` until exact independent audit and human acceptance.

The final Win32 Release build and all 30 host tests passed at ordinary process
priority in 35.48 seconds, including 1021 command checks and 2046 actual-manager
publication checks. The command fixture exercised all seven participating guard
contentions, selected/extracted same-ID handoffs, original lifetime closure,
revocation and context loss, postcommit cache failure, 127 simultaneous exact
reservations and 140 ticks with only one changed-command marker. Existing tests,
runtime budgets and security checks were not weakened. Ten production fixture
symbol groups remain zero; the integrated x86 DLL is 1,923,072 bytes. All 251
unowned baseline files and the original Base/SDK inputs remain unchanged.

## MCUT-13 — Scope retirement qualification boundary

M5b2a qualifies exact original command-ticket reservation acknowledgement and
separate try-only scope observation/off-lane disposal. It does not complete VM,
manager, Base, loader or borrowed-service retirement. Unknown resource dispositions
and non-command fresh reservations stay Pending; no stopped-domain publication or
authority renewal is introduced. See MPS-11 for the concrete ownership and limits.

Win32 Release build and all 32 host tests passed at ordinary priority in 44.89 seconds,
including 657 scope-retirement and 1037 corrected command checks. The command fixture
now handles explicit Busy within its bounded native contention budget while
preserving one captured input, all denial witnesses and grant issuance only after
candidate success. Actual real-M contention and true-stale failure were tested.
The earlier ambiguous preparation.stale log does not prove its exact failed branch.
Twelve production fixture-symbol groups remain zero; no new dependency, production
issuer, game installation or operating-system VM/crash operation was added.

These are isolated host results. Native allocation/free probes are not universal
allocator or Lua/file/wait instrumentation. Defensive impossible post-marker
publication failure and retirement-mailbox invariant overflow are source-reviewed,
not fault-injected. This does not establish live reads, native resource refresh,
gameplay or production authority. Exact independent audit and explicit human
acceptance remain distinct from successful tests.


V1 isolated execution on 2026-10-17: Win32 Release build PASS; full regression
33/33 PASS (46.27 s), including 200 VM enrollment checks (poll-count dependent). Production fixture
symbol segregation: all 13 groups absent. DLL remains x86, 1,930,240 bytes.
Audit V1-AUD-001 required the fixture to distinguish clean unbound, stale and
partial association. The corrected oracle and all regressions were re-executed;
production binding behavior is unchanged. Exact source/artifact pins and
independent audit are retained in `.analysis/u3fm5b2b1-2026-10-17/`. Enrollment
is inventory only; no VM retirement or manager finalization certificate follows.


Final V2 Win32 Release build PASS; full regression 34/34 PASS (50.65 s).
Actual retirement checks: 519 (poll-count dependent). Native retirement metadata
80 bytes and fixture action-holder aggregate 72 bytes, within separate
128/256-byte bounds. All fourteen fixture symbol groups are absent from production.
Integrated DLL remains x86, 1,930,240 bytes. Exact source/artifact/evidence and
independent audit reside in `.analysis/u3fm5b2b2-2026-10-17/`. This is isolated
current-record VM/slot qualification, not whole-manager Finalizable/Closed.

V3 implements the independently reviewed original-Plan retirement profile in
[MPS-14](manager-persistence-shutdown-contract.md#mps-14--original-plan-retirement-and-lifecycle-reservation-ownership).
Its persistent holder retains the complete genuine preparation/publication Plan
across contention, qualifies only original inactive/retired targets, and shares
the V2 native close action. Producer-coupled lifecycle leases use the existing
exact ticket/mailbox machinery and reservation limits. No active successor,
root grant or unknown raw reservation may be reclaimed by inference. This is
implementation in progress; executable qualification and exact final independent
audit are not yet claimed. No production cutover or whole-manager certificate
follows from this unit.


### U3-E3 command/cache integration contract

The original command path shares native settlement and lifecycle staging through
ManagerCache/ManagerCacheAction, with original pending lineage gates and atomic
pre-acquired marker acknowledgment. Automatic Lua output is captured before lane
acknowledgment and consumed off-lane on the existing single action worker.
[MPS-18](manager-persistence-shutdown-contract.md#mps-18--original-commandcache-settlement-and-off-lane-execution)
defines ownership, lock order, bounds and mandatory qualification. The earlier
per-unit execution records above remain historical evidence; this contract text
does not assert execution of the E3 candidate, production ingress or whole U3
acceptance. No live game refresh or universal close certificate follows from
native cache settlement.


### U3-E3 canonical reactivation qualification

[MPS-19](manager-persistence-shutdown-contract.md#mps-19--canonical-reactivation-and-original-preparation-snapshot)
binds fresh reactivation to the original canonical participant and requires the
original preparation stamp/domain pair throughout guarded preparation. The
source-audited repair remains subject to actual host regression and independent
result review. It neither enables production callers nor changes detached
record admission or the separate U4/U5/U6 boundaries.
