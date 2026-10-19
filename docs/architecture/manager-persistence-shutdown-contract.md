# Manager persistence and shutdown contract

Status: **Accepted isolated contract; m5b1/m5b2a/m5b2b1 independently qualified, m5b2b2 isolated current-record retirement tests passed; exact audit recorded separately. No production activation.**
This refines [MCUT-06/07](manager-cutover-plan.md),
[MOWN-05](manager-owner-integration-contract.md) and the
[scheduler publication contract](scheduler-publication-contract.md).
Private native concepts below are not public SDK signatures, manifest fields,
new permissions or end-user dependencies. Exact discovery and plan audit belong
in ignored `.analysis/u3fm5a-2026-10-17/`. Human plan acceptance and approval of
each implementation unit remain separate.

## MPS-01 — Verified baseline and distinct outcomes

At the accepted m4b baseline:

| Area | Observed implementation | Remaining gate |
| --- | --- | --- |
| Requested order | [state store](../../src/mods/mod_state_store.cpp) stores ordered enabled IDs; absent file means empty intent | IDs do not encode content, installation identity, consent or activation success |
| Disk replacement | Shared `.tmp`, stream flush, unconditional Win32 replacement, bounded retry policy | No conditional receipt, external-writer exclusion, disk/RAM transaction or guaranteed power-loss durability |
| Legacy lifecycle | [manager](../../src/mods/mod_manager.cpp) writes candidate order and can unconditionally restore previous order | Canonical coordination cannot inherit this rollback |
| Canonical lifecycle | [joint publication](../../src/mods/owned_publication.cpp) updates actual order/stamp/registry in memory | Canonical disk intent is not integrated |
| Commands | [typed consumer](../../src/mods/manager_commands.cpp) returns Indeterminate after committed cache failure | Result is volatile; no retained repair obligation, and later no-op success is not repair |
| Closure | Manager destructor waits for native users; scope drain observes only scoped running work | No complete bounded manager/lane/control/retirement completion certificate |

Report independent dimensions: requested disk intent, actual logical commit,
cache settlement, live game effects and closure. A diagnostic may be superseded,
but an unsettled obligation cannot silently disappear. Indeterminate does not
mean that no commit happened. Internal result storage must explicitly retain
whether the marker was crossed or its outcome is unknown; never infer it from
the latest textual diagnostic. Live refresh remains not requested/unqualified.

## MPS-02 — One cooperative state-path writer

The qualified first profile has one trusted in-process writer coordinator per
canonical native state path. Every cooperating writer sharing that path,
including legacy lifecycle while coexisting and future ModStudio/MCP/HotReload
ingress, must use it. Per-manager lifecycle exclusion alone is insufficient.
This is a common internal sequencer, not another owner map, permission evaluator,
service, background worker or script-visible filesystem capability.

Admission retains the original State/entry incarnation, expected canonical stamp,
order and content classification. Native write identities never reuse a process
generation/sequence; exhaustion rejects before effects. A receipt binds the exact
path owner, operation, previous state (including absence), proposed bytes and
write sequence. Textual IDs and file hashes alone cannot act as receipts.

The writer gate is acquired only off the script lane and outside publication
guards. Hold its exclusive ownership across the intent write, final memory
validation and any immediate compensation. While held, acquire D/M/S/V/R/B/Q/T
only in their established order and nonblocking; no path may wait for the writer
gate while holding any of those guards. File work, backoff, serialization,
diagnostics and final destruction stay outside those guards under a counted
native lifetime lease. Reentrant/lane requests enqueue or return Pending/Busy;
they do not perform synchronous I/O or start an ad hoc worker.
Gate identity must cover supported native path aliases, not merely matching
path strings; the first profile rejects unqualified alias/reparse cases.
Case/relative/alternate-name admission tests must either reach the same path
owner or reject before effects. No claim of alias safety follows from a textual
normalization alone.

Raw external editors/processes are outside this profile. Observe exact prior
bytes and native file/directory identity to detect changes, but do not call this
OS compare-and-swap: read/hash/check followed by replacement has a race and cannot
detect arbitrary A-to-B-to-A mutation. Detected external edits, disappearance or
path identity changes produce Conflict/Unknown and stop automatic overwrite or
compensation. Undetected hostile same-user writers are not isolated by this
contract. Multi-process writers require separately reviewed exclusion or fully
cooperative ingress before production use. File replacement is never described
as proof that all external writers are excluded.

## MPS-03 — Intent protocol and owned compensation

1. Outside all compound guards, prepare bounded previous/candidate bytes, exact
   previous absence, result/retirement storage and publication plan. Verify the
   trusted local state directory and reject unexpected reparse/identity changes;
   scripts cannot supply arbitrary destination paths. The first isolated profile
   caps each previous/candidate buffer at **1 MiB**. This is a proposed checked
   admission limit, not an existing parser guarantee. Oversize input stays intact
   and is diagnosed, never truncated or normalized to empty.
2. Under the cooperative writer gate and native lifetime lease, validate the
   expected write sequence and original canonical observation. Create only an
   operation-owned same-directory temporary file; do not truncate another
   operation's fixed `.tmp`. Reuse one shared file publication implementation
   with the existing retry policy; do not duplicate format serialization or
   claim a filesystem syscall has a hard deadline.
3. Publish candidate **requested intent** off-fence. A failed/ambiguous write
   reports its actual phase; reconcile observed destination before retrying.
   Clean up only the exact owned temporary file. Never delete an unknown file
   as cleanup or elevate/change permissions to make publication succeed.
4. Reacquire the original ordered reservations, verify exact current authority,
   content, owner/order/stamp and stop state, then use the existing single memory
   marker. Holding the writer gate does not validate permission or freeze native
   revocation. No disk I/O or arbitrary callback is added at the marker.
5. A definite pre-marker rejection may compensate only its still-owned receipt
   while the cooperative sequence and written bytes still match. Restore exact
   prior bytes or prior absence, not a reserialized guessed order. Compensation
   is one owned operation, not an unbounded retry. If its outcome is ambiguous,
   preserve mismatch evidence and reconcile; do not blindly repeat restoration.
   A deferred attempt must recheck the receipt after reacquiring the same gate.
   Superseded/missing ownership or detected external change forbids restoration.
6. After a marker, keep actual committed state. Persistence bookkeeping, cache or
   diagnostic failure is committed/pending or unknown; never re-enable the old
   VM/grant, republish the old registry or use an AdvanceRollback exception.

The file stays the existing enabled-ID format. No journal, persisted grant,
signature scheme or schema migration is introduced. A process interruption after
intent write but before the marker can leave a requested order that never ran.
Interruption after the marker can lose volatile settlement evidence. Stream
flush/Win32 write-through are not an asserted disk/RAM or power-loss transaction.
Fault-injected logical cut points in a synthetic harness qualify this protocol;
actual crash/power-loss experiments are not part of m5a or its first units.

## MPS-04 — Startup and authority boundary

Canonical startup treats the file as requested intent. Missing/rejected mods
remain requested with explicit diagnostics; do not silently save only successful
activations as if that preserved the request. Malformed/oversize intent admits
no activation from that input and remains intact for diagnosis. This proposed
canonical rule does not claim that existing legacy normalization already changed.

Recapture and validate current content, native installation/route classification,
Base pin, game context and independently valid consent before activation. Never
deserialize native pointers, generations, reservations, grants or acceptance from
IDs. A process-local sealed stamp does not classify the next process. Durable
route provenance or a separately approved trusted deployment policy must prevent
sealed-to-legacy downgrade across restart; until qualified, production startup
cutover stays closed. An untrusted manifest or missing approval cannot select a
legacy fallback. This unit neither implements a consent ledger nor weakens
existing revocation persistence requirements.

## MPS-05 — Bounded cache settlement

Known failed/partial cache invalidation creates a distinct volatile native
obligation reserved **before** the marker. It records exact committed revision,
nonreusable entry/incarnation identity and affected TGI payload. Do not retain a
full registry snapshot, entire publication plan, VM/Base/capture or another
mutable current-owner record merely to retry eviction. No ledger self-cycle or
long-lived ordinary manager Lease may prevent its own shutdown.

Proposed isolated bounds are **128 preallocated slots**, at most **one unsettled
obligation per owner**, **8192 unique TGIs per slot**, and **12 MiB aggregate TGI
payload** using checked flat native storage (12 bytes per TGI). Fixed metadata
also has 128 slots; any additional dynamic storage must be accounted separately
before admission. These are new qualification limits, not deductions that every
existing registry set or script baseline already satisfies them. Check actual
union size/bytes and arithmetic; reject before publication if unsupported/full.
No truncation, post-marker allocation surprise or unbounded coalescing. A new
write for that owner returns Pending/Busy until the obligation is settled;
neither a no-op nor a newer success clears it accidentally.

Retry only exact idempotent native cache invalidation under a safe borrowed-
resource lease/owner certificate, outside compound guards. It is not command
replay, grant renewal, task fabrication or live game refresh. After partial
failure, retain all target obligations until exact repair completes. Cache epoch
exhaustion/uncertain partial progress must fail closed and stay explicit; wrap
cannot validate an old plan. Any atomicity change to the cache itself needs its
own source-reviewed tests, not an assumption that false means no mutation.

After owner handoff, old settlement cannot change successor diagnostics or state
through its textual ID. Original TGI-only conservative cache invalidation may
be used only with validated current cache lifetime; supersession is not proof of
repair. A bounded off-lane native polling entry point reuses existing control
ownership. No reaper thread/service is proposed. Pending results survive later
diagnostic changes within the running process, but do not survive restart.
Shutdown may cancel an obligation for teardown only with quiescent consumers and
explicit AbandonedOnClose disposition; this is not successful repair or live
restoration. Until that certificate exists, retain the obligation/dependencies.

## MPS-06 — Request, poll and completed closure

The single retained close coordinator has these conceptual states:

| State | Permitted behavior | Required transition proof |
| --- | --- | --- |
| Open | Existing admitted operations | None |
| ClosingRequested | Close ordinary admission; idempotent request from lane/callback/current Lease | No join, self-wait, Lua, destructor or reverse-order guard acquisition |
| Draining | Off-lane native owner polls exact outstanding obligations | Busy/Pending with explicit reason; stopped latch alone is not completion |
| Finalizable | No future borrower can enter; mandatory work/retirement settled | Complete resource certificate below |
| Closed | Final owner released dependencies in order; repeat callers observe completion | One completion acknowledgement, no reopening |

Close ordinary admission before waiting. Existing admitted users retain valid
borrowed storage until their Lease exits; closing does not null resources under
them. Stop/cancel operational authority without introducing a replacement grant
or fake semantic request. A request callable from an owned callback cannot acquire
its own lifetime/lane barrier and synchronously wait for itself.

The off-lane close owner uses one exclusive native close token bound to the
original retained State and close generation, distinct from an ordinary borrower
Lease. It certifies release-only ownership, not permission or operational cache
access. Poll must not hold an ordinary counted user while waiting for users==0,
or subtract an arbitrary caller from that count. Any ordinary temporary Lease
must end before the zero-user observation. Other callers observe the same close
generation/completion and cannot create a competing finalizer.

The first close unit adds one fixed close state/token/completion record per
manager, **no deferred-request queue**, and at most **32 existing-holder checks
per poll** with an owned resumable cursor after native user quiescence. These
are proposed qualification limits. A sticky close request is out of band and
cannot be dropped because a VM/control slot is full. Use the existing 128 VM/
control slots and distinct 128 retired-scope holders without pooling their
budgets; deduplicate references by native generation, never create unbounded
new retirement records on repeated polls. Pending writer admission returns Busy
without accepting a queued job. Cache obligations use MPS-05's separate bounds;
scope release mailboxes keep their own existing bound. Any later accepted queue
needs a separate count/byte budget and pre-admission reservation. A polling work
budget does not guarantee the duration of arbitrary callbacks or OS operations.

The complete certificate checks separate counters/identities: manager native
users; action-worker request admission and completion; scheduler selected ticks;
queued/executing lane controls and VM callbacks; stopped/drained RuntimeScopes;
command reservation release mailboxes; original candidate/retired VM Stop/Close
and slot release; pending intent/compensation/cache work; and loader service/hook
borrowers. RuntimeScope running==0 does not prove the others. Existing scope,
scheduler VM/control and joint-plan retirement capacities are distinct budgets,
not interchangeable free slots. Capacity exhaustion retains ownership and rejects
growth before publication; no unsafe holder may be evicted to make room.

Poll uses nonblocking participant acquisition and no wait under D/M/S/V/R/B/Q/T
or on the script lane. Bounded polling is not a bounded destructor, callback,
filesystem syscall or thread join. Once readiness is observed, the final
off-lane owner must still own exclusive close/completion sequencing so a new
control or concurrent finalizer cannot enter between observation and release.
Queued stack-owned control requests need completed/cancelled acknowledgement;
a stop bit is insufficient. At-most-once onDisable means the obligation is
consumed once even when it throws or reenters; it does not imply callback success.

On timeout, contention, stuck native callback or unknown borrower, return Pending/
Busy and keep the entire needed manager/State/VM/code/cache/registry ownership
bundle alive. Never timeout into a destructor, force-kill a thread, detach an
unsafe worker or substitute no-op shared ownership for borrowed storage.
Destruction is allowed only after Finalizable off the owning lane/current Lease;
do not advertise the existing blocking destructor as a reentrant close API.
If orderly close never completes, explicitly report restart/process termination
as a user decision rather than pretending storage was safely freed.

## MPS-07 — Stopped domain and loader boundary

Global D stop remains sticky: no new publication, successor, cleanup marker or
rollback advance. Release-only original-owner cleanup can stop/close and return
slots or exact reservation bookkeeping without D advance. Retained plans/batches
after manager close inspect closed native anchors and deny borrowed access;
self-owned safe observations may outlive the manager. A cleanup certificate is
not renewed operational authority and cannot follow stale game pointers.

Only a later approved loader integration may replace its present scope-only
readiness check. It must retain hooks/services/manager before cache and registry
until their own real quiescence acknowledgements complete. Disable/detach of a
hook is not proof that its trampoline/code is reclaimable; existing process-
lifetime retention remains. No general stopped-domain registry cleanup bypass
is introduced. Retain sealed registry/dependencies when its stronger cleanup
proof is unavailable.

DllMain/process detach must not run this waiting protocol, Lua, disk settlement
or synthetic callback replay under the loader lock. Process termination/abnormal
exit is distinct from orderly close; no promise of callbacks, repaired cache,
persisted intent or crash report follows. VM and crash tests remain parked.

## MPS-08 — Required isolated failure matrix

These are planned tests, not executed m5 results. Each uses original native
identity, coherent registry/stamp/order observations and explicit ownership counts.

| ID | Cut or interleaving | Required oracle |
| --- | --- | --- |
| P01 | Serialization, 1 MiB boundary, temporary creation or replacement failure | No marker; exact destination/absence and owned-temp cleanup or explicit unknown; no security fallback |
| P02 | Intent published, then stale owner/revoke/stop before marker | No memory effect; exact receipt compensation or explicit mismatch |
| P03 | Cooperative newer receipt and delayed compensation; sequence exhaustion | Preserve newer bytes, deny stale receipt/ABA; never wrap identity |
| P04 | Detected external edit, removal, reparse/path change, ambiguous write | Conflict/Unknown and no automatic overwrite; no arbitrary external-CAS claim |
| P05 | Synthetic interruption before write/between write and marker/after marker | Truthful requested-vs-committed state; fresh authority on simulated restart, never journal/durability claim |
| P06 | Absent/corrupt/missing mod or changed content at canonical startup | Preserve intent and deny unsupported activation; no ID grant or legacy downgrade |
| C01 | Post-marker partial eviction fails; later no-op arrives | Commit retained, exact pending remains; new owner write denied until settlement |
| C02 | 128/129 slots, 8192/8193 targets, aggregate/overflow and epoch exhaustion | Checked pre-marker admission; no allocation/free surprise, lost obligation or wrapped witness |
| C03 | Repair under same-ID handoff/revoke/stopped D; teardown cancellation | No replay or successor status mutation; exact lifetime, no D advance, explicit settled/abandoned distinction |
| S01 | Close from lane/onDisable/current Lease and reentrant close | Request-only returns; external owner completes without self-deadlock |
| S02 | Reader/postcommit/native borrower, selected tick or queued control paused | Closed admission, no early free; complete separate counts after release |
| S03 | Q busy/deferred releases, VM/control/retirement capacity full | Nonblocking Pending with retained exact holders, eventual acknowledged cleanup |
| S04 | onDisable throws, repeat/concurrent close, old plans survive | Once-only obligations, single completed-close certificate, no successor retarget |
| S05 | Domain already stopped or borrower never returns | Release-only cleanup or retained Pending, no rollback marker/forced destruction |
| S06 | Loader teardown ordering, detached hooks, process-detach source route | Dependencies/code retained until their certificates; no waiting close from DllMain |
| S07 | Close token, 32/33+ holders, repeated polls, competing finalizer and stale cursor generation | No ordinary self-user; resumable bounded progress without duplicate retention; one finalizer; changed/reused identity denies stale completion |

## MPS-09 — Implementation sequence and acceptance

| Unit | Bounded proposed scope | Gate |
| --- | --- | --- |
| **m5b1** close foundation | Private request/poll/completion coordinator using actual manager admission/native close token and scheduler/lane/control witnesses; isolated harness, no loader routing | S01/S02/S04/S07, no ownership cycles/self-wait; full relevant regressions and independent audit |
| **m5b2** exact retirement completion | Original VM/slot/scope/deferred-reservation/cache-holder accounting under stopped D; no production cleanup bypass | S03/S05, capacity/timeout/once-only tests and independent audit |
| **m5c1** writer receipts | Shared bounded state serializer/publisher and cooperative path sequencer/receipt, synthetic local paths only | P01/P03/P04, preserved legacy default behavior and no OS-CAS claim |
| **m5c2** canonical disk coordination | Off-fence intent plus conditional compensation around unchanged canonical transaction | P02/P05/P06, no file work at marker, no production startup |
| **m5d** cache settlement | Bounded volatile obligation/result storage and exact idempotent repair | C01-C03; explicitly tested bytes/counts/partial failure and original lifetime |
| **m5e** combined eligibility review | Cross-unit regression and loader/startup production checklist | S06 plus all unmet ingress/provenance/live-read/refresh/route gates; integration requires separate approval |

M5a discovery/contract and its independent audit were accepted, followed by the
qualified m5b1 foundation. On 2026-10-17 the user authorized completion of full U3
with intermediate continuation conditional on independent exact-candidate PASS.
This temporary task-specific delegation expires at full U3 completion or revocation;
final U3 human acceptance remains separate. It changes no production permissions,
active instruction policy, deployment or publication scope. Each unit retains
bounded source ownership, a fault/test plan, exact evidence and independent audit.
M5b2a scope retirement has passed its exact independent audit. The current unit
is **isolated m5b2b2 explicit current-record retirement**, following independent plan PASS;
asynchronous VM retirement and inventory completion remain later reviewed units.
No new end-user program/compiler/service, package format, backward-compatibility
migration, signed consent journal, network feature or runtime instrumentation
dependency is introduced. DLL owns low-level lifetime/publication; Luau base
owns presentation/high-level task APIs; SDK specifies supported boundaries;
DevTools remains a consumer, never an alternative permission or current-owner store.

## MPS-10 — Isolated close foundation boundary

M5b1 implements a private fixture-qualified request/token/poll foundation on the
actual manager State, action worker and original scheduler/lane/control records.
Production entry issuance and loader routing remain closed. Request reaches only
State-owned retained admission latches, never a borrowed manager/worker pointer.
The original native users and real queued/executing/selected witnesses determine
progress; an ordinary borrower Lease cannot be subtracted to let its own poll pass.

The result **FoundationReady** covers this subset and always retains an explicit
remaining-retirement condition. It is not Finalizable, Closed or permission to
destroy manager/cache/registry/code. Unbound preparation/Base/external controls,
scope and deferred-reservation settlement, VM slot release, services and hooks
remain m5b2/later obligations. Existing destruction behavior is not relabeled a
bounded or reentrant close API.

Request closes new ordinary controls before both inline lane execution and actual
control enqueue; previously admitted stack-owned controls retain their completion
acknowledgement. Release-only Stop/Close/Flush remain available to existing
retirement. A once-only foundation acknowledgement does not skip later current
witness checks: newly admitted release controls can make a later observation
Pending. The exact target host's generation/activity invalidates a resumable scan;
unrelated Base/other-manager activity cannot substitute its witness or prevent
target progress merely by changing a global counter. Scan visits at most32 actual
existing lane slots per poll; no synthetic fixture counter is its sole proof.

The close token is exclusive, release-only and not an ordinary user. Dropping it
releases the exact owner nonce without waiting or reopening admission; a fresh
token restarts scanning. Poll retains safe native lifetime while borrowing actual
objects, uses only try acquisitions and no callbacks, and never frees resources.
Exact tests/artifacts and independently reviewed limits belong in ignored
`.analysis/u3fm5b1-2026-10-17/`.

Win32 Release build and the complete 31-test host suite passed at ordinary
priority in 36.10 seconds. The new target passed 565 checks in that run; a focused
run passed 589 checks. Counts vary with bounded polling, not with removed
oracles. Tests exercised real native borrowers, selected/queued/executing work,
accepted actions, request from lane/current Lease/throwing onDisable, historical
acknowledgement with new release work, 32/33 holders, token/cursor reset, actual
slot release and unrelated-host activity, plus retained handles under stopped D.
The slot-release identity test does not claim a full same-slot reuse/ABA cycle.

All eleven production fixture-symbol groups were zero; the integrated x86 DLL
is 1,929,216 bytes. Existing tests, security checks and runtime budgets remained
intact. All 267 unowned baseline files and original Base/SDK inputs were preserved.
Close-sequence/activity exhaustion guards are source-reviewed, not deliberately
exhausted. This is isolated foundation evidence; full retirement, loader routing,
gameplay and a bounded destructor remain unqualified. Independent exact audit
and explicit human acceptance are separate from successful host tests.

## MPS-11 — Exact scope retirement qualification profile

M5b2a separates native **Observe** from **DrainOne**. Observe uses try-only Q and
scalar observations; it never runs callbacks, reduces release mailboxes, extracts
operation nodes or destroys retained resources. DrainOne is an explicit off-lane
native action, outside publication guards and borrowed-resource callbacks. It may
run an unbounded native destructor outside every guard; this is not a bounded
destructor or callback deadline. No production issuer is introduced.

Each fresh command reservation receives its exact native incarnation/phase under
the same Q acquisition that creates it. Root grant reservations are not command
tickets. Immediate release and mailbox acknowledgement share exact once-only
retirement; an empty mailbox cannot hide live delayed envelopes or Writing
publishers. Old/reused identities cannot retire a successor. Non-command fresh
reservations are reported separately and remain unqualified until their original
owners release them. Stopped D never advances or grants replacement authority.

The proposed qualification uses 128 fixed ticket records, checked to at most
32 KiB, alongside the existing separate mailbox and retained-reservation budgets.
One extracted operation node and one explicitly qualified native resource holder
plus disposal metadata are checked to at most 1024 bytes of additional storage.
Existing payloads transfer without copying; opaque retained resource bytes have
no inferred total-size guarantee. Unknown resource disposition stays Pending.

An exclusive original-State disposal turn retains lifetime across off-guard
destruction and its atomic acknowledgement. Reentrant observation returns Busy;
only a later try-Q join can confirm completed disposal. No manager Lease is
invented, no caller is subtracted from native users, and a partial ScopeReady
observation is never ManagerClosed or permission to free VM/cache/registry/Base.
Exact implementation results and independent candidate audit are recorded under
ignored `.analysis/u3fm5b2a-2026-10-17/`; successful tests do not replace that audit.

The existing fixture preparation boundary distinguishes actual final-witness
Busy from stale rejection. Busy is still rejection, never successful preparation.
The isolated command caller may retry only explicit Busy within its finite native
contention budget, preserving one captured input and issuing no fixture authority
until an unexposed candidate succeeds. Stale, closed, Base and initialization
failures are not retried. This is not command replay, permission renewal or a
production retry policy; Lua and admission budgets remain unchanged.

The final Win32 Release build and all **32 host tests passed in 44.89 seconds**
at ordinary process priority. The new target passed **657 checks in 0.25 seconds**;
actual original tickets, Writing/Ready mailboxes, 127 fresh reservations plus root,
running callback retention, off-Q reentrant disposal, real copied-value pool
charges and an extracted manager-command envelope were exercised. The actual
script-lane disposal attempt was denied. Corrected command tests passed 1037
checks, including deterministic real-M Busy and true-stale rejection without
authority issuance or replay. Earlier failed attempts remain preserved; the exact
branch of the earlier ambiguous Handoff failure was not proved by its old log.

Measured fixed storage is 20,480 bytes of ticket records, 96 bytes of disposal
holders/turn observation and 2,168 bytes of separately reported added metadata.
Turn metadata overlaps those categories; they are not disjoint total-process or
opaque-payload byte bounds. Twelve fixture-symbol groups are absent from production;
the integrated x86 DLL is 1,930,240 bytes. Original Base/SDK inputs are unchanged.

Non-command fresh reservations and unknown generic resources remain explicitly
Pending/unqualified; this unit adds no owner-release API for those reservations.
Native sequence exhaustion and the defensive mailbox-overflow invariant are
source-reviewed rather than deliberately exhausted. This qualification does not
establish VM slot release, manager destruction, loader/service/hook quiescence,
production authority, gameplay or a bounded native destructor.

## MPS-12 — Early original-host VM enrollment

M5b2b1/V1 reuses the existing VM scheduler host and native slot host/weak-VM fields.
It adds no owner map, generation counter, holder array, accepted queue, thread or
strong asynchronous dependency bundle. An association is inventory provenance,
not committed/sealed/scheduled state, a grant or operational scheduler admission.

The same binding primitive verifies original VM pointer, nonreused generation and
null-or-exact host under actual V/L ownership. Foreign hosts are never overwritten.
Busy control positions, executing work, scheduled VMs and the existing host's
selected-holder witness prevent association changes. The latter is conservative;
an unbound VM cannot legitimately be selected because valid scheduling first binds
its host. Busy leaves all association fields unchanged. Same-host idempotence is
available only after these safety checks; public setup role restrictions remain.

Actual constructor control completion clears the slot's executing pointer, sets
Work.done and acknowledges the captured activity host under the same L acquisition.
The waiter observes completion under L before returning. No later slot mutation
can decrement a newly attached host for previously unbound initialization work.

Preparation performs Current, private BindPreparation, then Current again against
the original snapshots. Each observation is evaluated once; there is no hidden
retry, new authority or atomic-facts-freeze promise. Later publication still
revalidates all original participants. A first-current failure leaves the VM
unassociated; a second-current failure cleans the original already-bound candidate.

When close wins before binding, attach the original closed host before returning
Cancelled and rejecting the unexposed Plan. Existing native users retain constructor
ownership through original VM cleanup and exact slot release. A close after the
binding check still leaves a correctly associated, closed candidate, never a
permission to activate it. Existing Plan ownership closes VM before Base/capture;
this unit introduces no longer-lived VM owner requiring a new dependency bundle.

A fixture-only hook at the real seam after the first Current supplies a bounded
borrow of the actual VM. Native control tests finish/join/reset every asynchronous
borrow before the hook returns, while Plan/Base/capture/native Lease remain alive.
The hook is absent from production and creates no public lifecycle phase or issuer.

This is an implementation qualification profile; executed results and exact audit
are recorded separately under ignored `.analysis/u3fm5b2b1-2026-10-17/`. It cannot
certify VM Stop/Close completion, scope/mailbox settlement, cache/writer work,
manager destruction, production loader routing or final full-U3 acceptance.


V1 isolated execution on 2026-10-17: Win32 Release build PASS; full regression
33/33 PASS (46.27 s), including 200 VM enrollment checks (poll-count dependent). Production fixture
symbol segregation: all 13 groups absent. DLL remains x86, 1,930,240 bytes.
Audit V1-AUD-001 required the fixture to distinguish clean unbound, stale and
partial association. The corrected oracle and all regressions were re-executed;
production binding behavior is unchanged. Exact source/artifact pins and
independent audit are retained in `.analysis/u3fm5b2b1-2026-10-17/`. Enrollment
is inventory only; no VM retirement or manager finalization certificate follows.

## MPS-13 — Explicit current-record VM retirement profile

The independently reviewed m5b2b2 profile separates a pure try-only observation
from an explicit off-lane `DrainOne` action. The latter may wait for the existing
interpreter lane and arbitrary callback/destructor completion; it has no bounded
duration promise. It introduces no work queue, reaper, thread or production issuer.
`Request` remains request-only and reentrant; deleting the manager from its own
callback/action remains unsupported.

Selection traverses the original closed-admission record map, at most 32 record
inspections per call and one target VM. An exclusive native turn, original token
nonce, target generation and monotonic completion sequence protect resumable
progress. A distinct atomic native close pin protects borrowed manager storage
through the entire action and local holder disposal. The destructor closes
ordinary admission, drains users, then waits for that pin outside the State gate
and every publication guard. Completion needs no post-callback State gate.

The target must be the VM owned by the actual current record. Canonical targets
also require the exact stamp/current origin/runtime/endpoint/scope relation and
the genuine preparation proof retaining Base/captured inputs. A weak lane slot
alone cannot prolong external Plan-owned candidate or retired VM lifetimes.
Those roles remain pending a separately qualified ownership path.

Scope retirement uses the existing explicit native drain and a separate coherent
observation. Stopping with zero running operations permits this VM phase even
while its registrations still need Lua cleanup. A subsequent control Busy may
leave scope stop already applied; there is no rollback/regrant. No scope resource
destructor runs under the VM control mutex. Existing Stop/Close and once-only
disable consumption remain shared; exact slot release follows actual Work.done
acknowledgement and original pointer/generation validation, never a stop flag.

This profile permits existing native Q/pool waits inside the explicit action;
passive observations remain responsive Busy/Pending. Native Q holders must not
synchronously wait on that same lane. No no-wait-lane guarantee, forced release
or hidden timeout completion is claimed. Scope tickets/noncommand reservations,
resources, Base, writer/cache, services/hooks/loader and external-plan retirement
remain separate certificate components. A completed current-record scan or VM
slot return is never whole-manager Finalizable/Closed.

The proposed added native metadata and action-holder aggregates are separately
bounded to 128/256 bytes; actual compiled sizes and execution belong in ignored
`.analysis/u3fm5b2b2-2026-10-17/`. Actual execution is recorded below; the exact independent candidate audit
is a separate gate. No game installation or production activation follows.


Final V2 Win32 Release build PASS; full regression 34/34 PASS (50.65 s).
Actual retirement checks: 519 (poll-count dependent). Native retirement metadata
80 bytes and fixture action-holder aggregate 72 bytes, within separate
128/256-byte bounds. All fourteen fixture symbol groups are absent from production.
Integrated DLL remains x86, 1,930,240 bytes. Exact source/artifact/evidence and
independent audit reside in `.analysis/u3fm5b2b2-2026-10-17/`. This is isolated
current-record VM/slot qualification, not whole-manager Finalizable/Closed.

## MPS-14 — Original Plan retirement and lifecycle reservation ownership

The isolated V3 profile extends explicit off-lane retirement to a caller-owned,
move-only holder of one genuine preparation or publication Plan. It transfers
the complete original Plan, retaining its Base and captured inputs across
`Busy`/`NotReady`; a temporary bare VM or scheduler Plan is insufficient.
The holder has at most two exact native VM targets. A staged candidate or retired
predecessor may qualify; a still-current successor cannot. Each action rechecks
original ownership and generation. Passive observation performs no Lua work or
destruction and never certifies complete manager closure.

Record and Plan retirement share the original native action turn, counted close
pin, scope-stop and VM-close algorithm. Explicit actions can wait indefinitely;
only passive observation is nonblocking. Whole-Plan destruction and replacement
occur outside publication guards, with VM references released before the original
Base/capture and decision owners. Moving or destroying a holder during its active
borrow is unsupported native lifetime misuse. An early ordinary destructor is
not an orderly-retirement certificate. Holder generations use the existing
original State monotonic sequence, survive idle moves, and never reuse a reset
holder identity; exhaustion rejects before transfer or effects.

Typed lifecycle decisions retain an immutable original scope/context binding.
A fresh lease is issued together with its exact reservation under the same Q,
using the existing tracked-ticket records and release mailbox. Command and
lifecycle owners share the existing 128-slot budget; with one retained root,
at most 127 fresh reservations fit. Root views own no release ticket.

Private lifecycle bookkeeping distinguishes an original uncommitted candidate
from an active scope under that same Q. It reuses the existing grant evaluator
with the actual scope phase; it does not activate the candidate or establish
genuine preparation provenance. Both the initial check and fresh-reservation
insertion use this distinction. Operational command admission and raw fixture
reservations remain committed-only. Final publication independently validates
the genuine preparation and the required candidate/current roles. Stopped,
closed or revoked scopes cannot become candidates through this bookkeeping.

For compatibility with earlier native observations, the historical
`commandTickets` field now counts the combined tracked command and lifecycle
tickets. It is not a command-only count and does not include untracked raw
reservations. The last native owner releases exactly its original reservation, including deferred
mailbox acknowledgement under contention or after revocation. No raw copied
context is adopted after the fact, and no root or arbitrary grant is swept away.
Producer/Plan disposal stays outside guards because releasing the last scope
owner can itself invoke blocking native cleanup.

This remains an isolated host profile without a production issuer, new thread,
queue, grant, dependency or installation step. Unknown external Plans, untracked
raw reservations, scope resources and writer/cache/service/hook/loader completion
remain independent gates. The exact implementation plan has independent PASS;
implementation, executable regression and final candidate audit are pending.

## MPS-15 — Isolated bounded native state writer

StatePathWriter keeps the original qualified local NTFS parent handle, file
identity, exact default-stream bytes and opaque operation identity. It is a
cooperative native protocol, not a same-user hostile filesystem compare-and-swap
or a durable transaction. Production exposes no Owner issuer. Original path
qualification rejects unsupported/reparse/ambiguous profiles before effects.

The fixed process registry has 32 original owners, eight retained operations,
two operations per owner including externally retained terminal views, and at
most two MiB raw payload per operation (16 MiB aggregate). Closed/Unknown ownership
is not evicted or reset to manufacture capacity. History is distinct from fresh
observation: a successful syscall alone does not prove publication. An ambiguous
effect quarantines original ownership and is reconciled without replay. Exact
identity/bytes/stream/location proof governs publication, cleanup and conditional
compensation; changed or inaccessible evidence cannot be treated as absence.

The existing serializer and bounded eleven-attempt/500-ms sharing/access retry
policy are shared. Retry requires a definite failed eligible native call and
fresh positive safety proof; successful, pending or ambiguous effects are never repeated.
Explicit filesystem actions may block; HistoryView is try-only and performs no
I/O. Last native disposal is outside compound publication guards. No manager,
VM, Base, authorization context or production grant is retained by this writer.

Qualified rename uses one explicit x86 system `ntdll!NtSetInformationFile`
parent-relative backend with the original held parent and simple leaf. It has
no DOS-path conversion, absolute-path fallback, alternate rename attempt,
additional dependency or privilege/share expansion. Loaded-system image/export
provenance and the actual host file identity/hash/version are qualification
evidence, rather than a universal operating-system binary hash requirement.

The already-loaded module is matched to the OS-derived physical system file by
its native mapped-device name and a read-only handle's normalized final NT path.
The loader's logical WOW64 `System32` spelling is not physical-file evidence.
Unavailable, truncated, non-disk/reparse or mismatched metadata rejects. The
original valid system-file identity is retained as scalars and must still match
on revalidation. This opens only existing metadata handles, adds no component,
and retains no system-file handle or large path buffer in the native owner.
The fixture reports checked physical/device paths for independent inspection.
This is not loaded-instruction hashing or an OS-update transaction: first
qualification across replacement cannot certify loaded-byte/disk-byte equality.


Native rename call/final NTSTATUS and explicitly valid translated DOS errors
remain distinct from the last actual Win32 diagnostic. Each original record
retains its fixed native input and IOSB, initialized to Pending before submission.
An outstanding request retains Unknown ownership, handles, buffers and capacity;
observation is scalar-only and no cleanup, new I/O or context reset is allowed.
Only explicit original Reconcile may zero-wait and acknowledge qualified final
completion once. A stale signal with nonfinal status cannot become success.
No operation is replayed to resolve pending or ambiguous evidence. The eight
fixed native contexts fit within eight times 1152 bytes of existing record
metadata; actual compiled sizes and execution remain separate qualification.

This unit does not integrate manager publication or enable legacy/production
writer cutover. Actual native profile, regression, size and symbol qualification
and independent candidate audit are recorded separately under
`.analysis/u3fm5c1-2026-10-17/`; source authorship alone is not runtime acceptance.

## MPS-16 — Original cache witnesses and bounded eviction obligations

The isolated cache profile binds every successful native build plan, including
reuse, to the original weak cache control, a nonreused process nonce, the reset
generation and target invalidation epoch. Commit validates these under the same
actual cache mutex. Clear advances reset before releasing snapshots; exhaustion
permanently closes mutation rather than wrapping or reviving a stale plan.

One shared eviction algorithm serves the existing native facade and retained
original batches. It precreates every missing epoch node before semantic changes,
prechecks all epochs for exhaustion, then increments epochs and invalidates every
affected entry without allocation. Partial metadata allocation may leave zero
epoch nodes, but cannot report complete eviction. Diagnostics follow completion:
`Evicted` and `EvictedDiagnosticPending` both acknowledge actual complete eviction,
with its authoritative count. Other results remain pending with count zero.
Repeated settled eviction performs no additional epoch changes or game work.

Private native batches retain exact original owner/cache/reset identity, immutable
first committed revision and full target payload. The bounded volatile process
store has 128 metadata slots and 8192 unique TGIs per slot, with a fixed 12 MiB
target region; metadata is separate and checked. It has no thread, service,
reset API or strong cache/manager/VM/Base/permission ownership. Pending storage
survives dropped views and remains rediscoverable only through its original
native token. Terminal externally retained views stay charged. Checked atomic
view release cannot be lost to contention; Reserve reclaims eligible terminal
zero-reference records, disposing detached weak metadata outside its guards.
Native issuers, incarnation and reset counters never wrap.

Observation is try-only scalar copying under the original record turn, without
cache locks, I/O, allocation or eviction. Explicit synchronous eviction requires
the caller to retain the actual live original cache for the entire action. The
record action turn and internal borrow span cache validation/mutation and direct
result acknowledgement; no later store-gate reacquisition can lose completion.
Weak identity is not a lifetime pin. Reset/lifetime mismatch remains unqualified
pending; it does not certify restoration or native game-consumer quiescence.

Owner issuance is fixture-only and absent from production. An isolated native
revision acknowledgement does not prove a real manager D marker. Actual command
premarker reservation/postmarker binding, same-owner production write exclusion,
manager close lifetime, qualified abandonment, live game consumers and DLL unload
remain separate integration/eligibility gates. Process exit loses volatile
evidence; this is neither crash durability nor a full manager-close certificate.
The MPS-15 writer scope is independently owned and not replaced by this section.

## MPS-18 — Original command/cache settlement and off-lane execution

Each original manager participant has one stable native cache owner identity.
Its once-bound original State/cache lineage survives active stamp replacement,
handoff and removal; resetting cache contents cannot reissue that identity.
Pending or unqualified original eviction blocks later commands, including
SamePatches, and lifecycle publication before their own marker. A replacement
route, equal text identifier or newer successful diagnostic cannot settle it.

The complete immutable affected union is reserved before the final publication
fence. Atomic ReserveMarker publishes the batch only with its original record
turn and action borrow already retained. The actual nonzero Advance result is
acknowledged directly through that turn before any memory publication/swap;
this step neither allocates nor reacquires store admission. Premarker rejection
cancels only its own preparation. Once acknowledged, errors retain the original
bounded obligation; no rollback, hidden script replay or successor adoption is
permitted. Original release-only eviction runs off compound guards under an
action-local manager Lease. The persistent record owns neither manager, State,
VM, Base nor that operational Lease. Original repair changes no grant or D
revision and remains possible after revocation or stopped D when cache lineage
is still qualified. Reset/unavailable remains explicitly unsettled.

Automatic command consumption uses the manager's existing single worker/thread
and one tagged UI-or-native occupancy. Admission borrows the once-bound original
Control under try-V, releases V, then uses W -> try-V to revalidate the exact
route/registration/origin before any successful ownership promotion. Occupancy
lasts through off-guard disposal; rejection executes no Lua or command ticket.
Queued native work retains original provenance, never an ordinary manager Lease.
Worker execution obtains an action-local original Lease before manager access.

Semantic Pump, onTick and extraction of that exact automatic output complete
under one V region before Work.done. Earlier manual output is restored before
acknowledgment, later manual Tick/Take cannot become part of the captured batch,
and the combined command limit remains 256. Only after acknowledgment does the
worker attach/consume the captured batch and perform native cache work. No
second worker, VM mailbox, post-ack Take, textual sink fallback, permission
issuer, external runtime dependency or replay is introduced. Base/routeless
cold execution and the existing 50ms automatic due rule remain unchanged.

Fixed cache bounds from MPS-16 remain: 128 records, 8192 targets per record and
12MiB target payload. Native scheduled job storage is at most128 bytes and the
single Control increase at most256 bytes; actual x86 sizeof evidence is required
separately. Closed retained Control storage is inert after joined detachment,
with no queued cohort, callable payload or live manager dependency.

Qualification requires genuine unintercepted automatic Set/Clear, exact manual
output attribution/quota, initial-V/W/final-V contention, real postmarker
allocation failure, original pending payload and lifecycle gates, original-only
repair, capacity, reset, revocation, close/cancel and disposal evidence. Fixture
observations are absent from production. CacheSettled is never a certificate of
whole-manager closure or loader/native-consumer quiescence. Source review and
isolated host qualification do not establish game refresh, production authority
or full U3 acceptance; those outcomes remain separately evidenced.


## MPS-19 — Canonical reactivation and original preparation snapshot

A sealed inactive canonical participant may receive a genuinely new candidate
after its preceding removal. Its original manager State, installed record,
participant, joint binding and scheduler node remain the same; its new runtime,
scope, incarnation and active generation must be genuine new identities. A
predecessor tombstone is not authority to reuse the retired incarnation. The
private empty-stamp classifier only describes shape; installed membership,
qualified provenance, inactive status, absent runtime/service and no pending
cleanup remain independently required under their original guards. Detached
records retain their candidate-specific tombstone rules.

Canonical successor preparation carries its exact original immutable stamp and
domain revision into shared lifecycle preparation. Both must still match under
the final guarded snapshot capture before a committable output is exposed. An
absent snapshot cannot select a permissive canonical path. An intervening
activation/removal, including a different participant changing the same domain,
requires a fresh genuine preparation; it cannot refresh authority silently.
The original stamp is retained locally through preparation without restoring
the active-origin ownership cycle. Failed partial work keeps whole-Plan cleanup
ownership and is reclaimed off compound guards.

These rules preserve original commit, cache, scheduler, context, registry and
status fences, including a single domain advance. They introduce no cache reset,
permission issuer or production ingress. Required host qualification includes
fresh same-participant lineage, competing candidates, foreign/detached/replayed
negatives and actual intervening snapshot/domain changes. The passive fixture
cut and weak lineage observations are test-only; source audit alone does not
establish runtime qualification or final U3 acceptance.

## MPS-17 — Explicit known-owner retirement qualification

The isolated known-owner profile constructs one original native manager cohort:
the actual current successor, a predecessor retained in its genuine whole
publication Plan, an unused genuine preparation Plan, their three distinct
original scopes, a real extracted command envelope and an original typed fresh
lifecycle lease. It qualifies only these explicitly retained owners. It adds no
Core API, global owner registry, production issuer or cleanup permission.

The test-only `listedOwnersSettled` conjunction samples separate existing native
observations after admission closes and every listed producer/action joins. It
is not a simultaneous cross-component snapshot or a manager-wide certificate.
Actual slot/whole-Plan acknowledgements and exact original-scope readiness remain
separate facts. Component manager scans keep their existing32-check bound; this
small cohort establishes no128-record/scopes limit. Explicit VM/scope actions may
block; passive observations retain their existing try-only behavior.

Required native interleavings include a preaccepted executing control, preserved
whole Plans across contention, precise current/predecessor/candidate targeting,
held future release with an empty mailbox, actual Q contention, original release
Writing/Ready and reducer acknowledgements, and a trusted resource's actual off-Q
destructor. Unknown opaque resources and untracked raw fresh reservations remain
unqualified in a separate negative cohort. No scalar input can substitute for
these actual ownership paths. Original Base/capture dependencies remain alive
through VM Close; all fixture Plan/stamp/observer owners must drop before weak
expiry is interpreted as lifetime evidence.

Status: isolated Windows x86 native host qualification passed independent
result review as part of the full regression suite. This covers only the
explicitly enrolled known-owner cohort; combined E4 ownership remains separate.
If a required barrier becomes unreachable, qualification remains blocked; an
omitted branch cannot silently pass through another dedicated test.

The broader MPS-09 m5b2 retirement census remains PARTIAL. Arbitrary external
Plans, retained cache/registry ownership, writer intents/receipts, services/hooks,
consumer quiescence and loader/unload eligibility still require their original
m5c/m5d/m5e and U5/U6 witnesses. Even a passing listed cohort keeps
`remainingRetirement=true` and `finalizable=false`; it does not close a manager,
certify safe DLL unloading or authorize production cutover.

## MPS-20 — Private synchronous file/memory coordination

Qualification status (2026-10-18): the isolated Windows x86 host build and all 40 regression tests passed independent result review, including all 13 persistence groups and the final fence checks. This closed native host adapter still has no production issuer, startup route or new grant. The result does not qualify later changed inputs, gameplay or final U3 acceptance.

`ManagerPublication::AttemptPersistence` admits only an original complete canonical Plan bound to the supplied manager's exact State. An ordinary counted Lease encloses path capture, one original cache reservation, the native file turn, one possible memory commit and destruction of the entire moved Plan. Rejected preadmission leaves caller input intact. Entry is off the actual script lane, outside component guards, under caller-exclusive input/output ownership; same-thread recursion is denied through final teardown.

Cache admission precedes native file intent. Only positively confirmed original candidate bytes, file identity and default-stream evidence admit the single shared memory algorithm. That algorithm emits explicit scalar evidence: premarker rejection, consumed no-change, actual nonzero Advance and marker-indeterminate are distinct. Cache acknowledgement follows recording the actual marker. A definite premarker memory rejection permits at most one immediate conditional compensation of the original receipt, off guards. Crossed or indeterminate markers never permit compensation or revival of predecessor memory. A returned cache-pending result does not erase its original obligations.

The original file Turn and ordinary Lease survive destruction of all members of the whole Plan, including arbitrary final VM callbacks. Synchronous teardown cannot be disguised as an early Pending return. The returned file outcome is at most512 bytes in the compiled target and contains only native writer ownership/intent/receipt, original operation identity and fixed scalar observations. It retains no manager, State, Lease, Plan, VM, Base, scheduler, scope, context owner or callback. `ReconcileFile` services only that original native operation; it cannot recapture authority, retry memory or start delayed compensation.

The separate private qualification target preserves existing tests and budgets, adding explicit Base and test-owned analysis-root arguments. Its required cases cover actual activation/handoff/removal/no-change, exact-State and early admission negatives, real final-guard contention and revoked facts, native interruption/foreign edits, genuine file Unknown, distinct postmarker cache pending, retention limits, and whole-Plan teardown under the original Turn/Lease. File Unknown and postmarker cache pending are not claimed to describe the same attempt. Existing native C1 bounds, E3 original-snapshot/reactivation tests and known-owner retirement remain required regressions.

Production `SaveEnabledState` coexistence and its replacement remain explicit U5 cutover gates; this private host unit does not certify startup recovery, whole-manager finalizability, hook/service quiescence, DLL unloading or gameplay. E4 combined-owner qualification and final cross-repository U3 acceptance remain separate.
