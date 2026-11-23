# Permission provenance and owner lifecycle

Status: **U3f-g contract and U3f-h isolated implementation accepted; U3f-i closed
lifecycle model awaits independent audit and human acceptance. Production
permission activation remains closed.**
This extends the [PROP adapter](prop-adapter-contract.md), [Base design](base.md)
and [runtime foundation](runtime-foundation.md). Names below are conceptual types
and transition requirements, not shipped SDK symbols, wire schemas or new grants.
Core, Base, SDK and DevTools retain their existing repository responsibilities.

### U3f-h isolated implementation status

RuntimeScope now evaluates a closed ordinary-script/agent origin variant in its
existing sole effective-grant store. A decision reference, grant identity/revision
and immutable operation reservation are separate. Fixture-only one-shot prompts
bind owner, opaque loaded-content pin, capabilities, action, extent, revision and
expiry; opening a changed review invalidates older pending prompts. This opaque
fixture pin is not a qualified production installation/dependency/content proof.

Ordinary execution requires no agent login. Agent reservations retain actor,
channel, consent session, acting owner and extent; session consent or exact entity
consent remains additional to mandatory gates. Session revocation advances the
effective grant revision: old reservations fail, while independently valid entity
consent may obtain a fresh reservation without renewing the decision expiry.
All-participants fixtures model authenticated membership from trusted fixture
ingress for later members of the reviewed session;
they do not lend the original actor's entity consent. Channel loss denies its
reservations and inspection. Restricted-mode inspection can truthfully report no
effective capabilities without authorizing an operation. Required denial closes
owner admission before deferred retirement; no patch cleanup/transfer is added.

Only separately compiled RuntimeScopeTestAccess can open/consume prompts, issue
grants or refresh reservations. Production authority remains unavailable. Fixtures
are bounded to 128 pending prompts, 128 effective grants, 128 total retained
reservations and 128 closed channels per scope; grant/reservation metadata retires with the scope, so
capacity exhaustion fails closed rather than recycling identities. Prompts expire
with the reviewed consent, at most 24 hours after creation. No durable ledger,
authentic human UI producer, real agent transport adapter, native factual observer
or production permission activation is qualified by this implementation. Existing
legacy PROP remains outside this semantic enforcement until the joint cutover.
Isolated verification and independent audit evidence belong in
`.analysis/u3fh-2026-10-16/`; executed results and acceptance are recorded there,
not inferred from this implementation description.

### U3f-i isolated lifecycle model status

Fixture-only native record issuance now retains the exact active VM, loaded
definition, registry, commit domain and nonreusable owner seal. Purpose-bound
one-shot plans prepare either release-only removal or a direct sealed successor
replacement through the existing registry snapshot algorithm. Handoff preserves
activation sequence and unrelated owners; exact tombstone removal is idempotent
without another registry/domain revision. A stale predecessor cannot select a
successor by a shared textual mod ID. Candidate preparation does not activate its
scope or issue permissions. Copied successor sets are limited to 4096 resource
definitions and 4096 total property operations before registry preparation.

The model serializes domain → registry → actual bridge context → canonically
ordered exact scope guards. Handoff revalidates both original reservations in the
sole RuntimeScope grant store, actual loaded VM pins, record stamp, registry and
domain generations and retained factual leases. Uncommitted candidate evaluation
is private and cannot admit ordinary tasks. Publication swaps prebuilt registry
state and record stamp, retires old admission/epochs/grants/tasks and exposes the
successor's committed scope while the same guards remain held. Coherence is
promised only to native record readers reserving the original domain; a lock-free
registry snapshot alone is not a cross-record/grant observation protocol.

Retirement cancels queued/preparing tasks with exact pending accounting while
retaining their buffers and dependencies for later explicit owner cleanup. No
allocation, caller callback, Lua, persistence or retired-state destruction belongs
after the marker. Revoked old operational grants do not authorize arbitrary
writes but do not prevent exact release-only removal while the domain is live.
Global domain stop denies new publication, including cleanup; no rollback advance
exception is introduced. Precommit failure leaves the registry/model record and
old admission unchanged except for independently sticky revocation or stop.

This is a closed isolated model, not the actual ModManager record coordinator,
production factual observer, authentic human-decision producer or live game
restoration/application. Production issuance remains unavailable and legacy PROP
remains outside the semantic enforcement cutover. Test and independent audit
evidence belongs in `.analysis/u3fi-2026-10-16/` and does not imply gameplay
acceptance. Existing conceptual production gates below remain unchanged.

## POWN-01 — Observed baseline and activation gates

U3f-f qualifies isolated process-local ownership: actual VM and loaded script pin,
original request/task/context, native registry incarnation seal and owner-local
property clear. Only separately compiled fixtures issue origins. Production has
no qualified permission issuer, ordinary-script principal adapter, native factual
observer, per-mod Base bootstrap, sealed-owner removal or successor transfer.

At the U3f-g baseline, RuntimeScope required fixture session/entity fields for every
call; U3f-h replaces that requirement with the typed model described above. Current ModManager
publishes its legacy registry change before scope activation and record assignment;
it does not implement the atomic sealed handoff below. Existing legacy PROP writes
remain outside semantic permission enforcement until the accepted joint cutover.

An immutable loaded ScriptDefinition pin covers its bytecode, modules, aliases and
limits. It is not an immutable raw-resource/dependency snapshot or durable install
identity. Metadata IDs, versions, cache keys and source correlation fingerprints
must not become approvals. These missing production proofs remain hard gates.

## POWN-02 — Typed origin and decision identities

| Conceptual identity | Core-owned meaning | Cannot substitute |
| --- | --- | --- |
| Owner incarnation | Actual manager record, installation binding, loaded content pin, VM generation, scope and registry seal | Text mod ID, version, path or an unrelated VM |
| OrdinaryScript origin | Native origin for that active mod's own execution | Invented agent principal/session/entity |
| Agent origin | Authenticated actor/channel, real agent consent-session lifetime and acting owner, retained through delegated work | Transport connection, enabled DevTools mod or claimed caller ID alone |
| Decision reference | Reviewed human action, subject/content/capability/extent, decision revision and provenance | Manifest request, UI boolean or task handle |
| Effective grant revision | Current RuntimeScope permission generation for a precise subject and extent | Commit-domain revision or registry generation |
| Operation reservation | Descriptor, immutable original input, captured grant/domain revision and factual leases | A new grant or implicit retry permission |
| Lifecycle reservation | Native candidate/old/new ownership transaction and exact expected registry generation | An operational script permission |

OrdinaryScript and Agent are a closed discriminated union, not a permissive bag
of optional numeric fields. Reject an unknown kind or inconsistent combination.
Core captures the discriminator and identities from trusted ingress. No mod,
manifest, Base method or MCP payload chooses its own authoritative role or owner.

Agent-origin work retains that provenance through Base, cooperation boundaries,
scheduled continuations and completion. A broker cannot relabel it OrdinaryScript
to bypass consent. Independent native events do not become agent-origin forever;
Core must distinguish them from the retained causal chain of an agent request.
An unqualified route cannot downgrade to ordinary execution. Cross-mod cooperation
uses an explicit narrowed contract; lending a facade cannot transfer identity or
widen privileges. Exact cooperation declarations remain a separate public gate.

## POWN-03 — Effective authority and denied permissions

Core is the sole evaluator. RuntimeScope is the sole effective operation-grant
store. Base provides presentation and requests; ModManager supplies actual owner
lifecycle; DevTools supplies authenticated agent ingress. No second live grant
map belongs in Base, ModManager, a ledger or MCP.

For an ordinary script, admission intersects active owner/content approval,
descriptor allowlist, requested-and-approved capability, readiness, factual
context/lifetime, quota and deadline. It requires no agent login. Agent calls
additionally intersect authenticated routing role and **session consent OR
entity consent**, scoped to that exact operation and subject. The OR never
bypasses any mandatory predicate. A valid session for another actor, channel,
extent or lifetime cannot satisfy this check.

Session-wide consent includes later participants only within the explicitly
disclosed scope. Its participant selector may be all present and future members
of the reviewed consent session; it is not narrowed to the first approving
connection's actor. Core must authenticate each joining participant and its actual
membership in that scope. Consent-session identity is distinct from transport
channel identity; no caller can assert membership or invent a new consent session.
User chooses expiry at game close or agent-session end; explicit
revocation can end it sooner. Session consent never survives its chosen lifetime.
Entity consent remains bound to its reviewed subject and extent. Neither consent
automatically approves new script capabilities, changed mod content or native DLL
execution. Loss of an authenticated channel closes that channel's admission; it
cannot silently create or inherit a replacement session.

Required and optional capability declarations must be distinguished by the future
canonical manifest contract. They are requests. Missing required approval prevents
candidate activation; withdrawing a required capability closes the active owner's
operational admission and initiates safe native retirement. It must not continue
privileged execution because physical patch cleanup is pending. Optional denial
permits restricted mode only when the mod supports it; expose its current effective
capabilities without granting missing ones. Availability probes never reserve access.

Validate at admission, immediately before irreversible publication and before
delivery. After domain revision changes, a fresh operation may capture a fresh
reservation from the same still-valid decision; this does not renew consent.
Never rewrite an already queued task's captured identity/revision to make it pass.
The U3f-f fixture context refresh is not a production authority mechanism.

## POWN-04 — Trusted human decision ingress and persistence

An approval ingestion adapter stays closed until its actual trust boundary is
qualified. Bind one host-owned, expiring, one-shot challenge to the displayed
subject, immutable reviewed content, capability set, extent, action and revision.
Any change while a prompt is open invalidates that challenge. Core verifies the
human-action evidence; Base rendering or calling a public confirm function alone
cannot establish it. MCP/agent/mod input cannot mint that evidence. Prompt replay,
an old challenge and a decision for another action must fail closed.

This is a threat boundary against script/MCP impersonation, not proof against
same-user native injection, compromised OS input or an already trusted native DLL.
No existing UI, transcript reference, Windows input API or helper is declared a
qualified runtime producer here. Qualify the actual native ingestion mechanism
separately before enabling it; this contract adds no executable or focus requirement.

The durable ledger records decisions/provenance, not live grants or runtime
incarnation handles. Use a loader-owned namespace outside mod payloads, bounded
versioned records, unknown-field/version rejection where security meaning is
ambiguous, atomic persistence and explicit denial on corrupt/missing evidence.
Bound each durable decision to a separately qualified installation/content and
requested-capability identity before reuse. A cached version/path hash is insufficient.
Session-only decisions are not durably restored. Native execution trust, signed
publisher trust and GitHub source trust are separate from capability decisions.

Runtime revocation takes effect before asynchronous persistence. An old ledger
copy or file watcher must not undo it in the current runtime. Local closure alone
cannot protect the next startup if a failed write leaves an old allow record.
Durable grant rehydration therefore remains closed until a qualified crash/restart
revocation witness and recovery protocol exists. Without that proof, require fresh
human review at every restart; a previously valid allow record alone cannot issue
a grant. Persistence failure closes affected issuance and reports its actual state.
A local ledger is not tamper-proof against same-user native/admin access; no
cryptographic or cross-restart anti-rollback claim follows from local file hashes.
No raw credentials, tokens, paths containing private data or full payloads belong
in public permission diagnostics.

## POWN-05 — Revocation, expiry and delivery

Revocation and lifecycle publication share a qualified serialization protocol.
The revocation linearization point closes applicable admission and advances its
grant revision before deferred UI, disk work or cleanup. Scope cancellation/state
must remain coherent with that point; no inversion of the existing writer order.
Game/render callbacks cannot call a blocking fixture mutex and claim nonblocking
retirement. A native callback producer requires the separately qualified atomic
admission-closure protocol; failed enqueue/try-lock cannot retain stale authority.

Before commit, revocation/expiry denies effects. After commit, it can deny delivery
but cannot rewrite the stored result as undone. Owner cleanup may release retained
tasks, buffers and dependencies without disclosing revoked payloads. A cancelled
or disconnected caller must not cause automatic replay. Failure after an
irreversible marker is Indeterminate where the postcondition cannot be established.

Operation expiry is bounded by its governing consent lifetime. If it wins at the
last precommit check, deny; time advancing after the irreversible marker cannot
retroactively undo a publication. Separately qualified game execution must retain
the original task through its actual postcondition rather than completing a
registry-only receipt early and later upgrading it to live success.

## POWN-06 — Candidate, activation and failed replacement

| Transition | Required invariant |
| --- | --- |
| Absent → Candidate | Native manager captures exact inputs, pending owner and bounded resources; no operational grants |
| Candidate preparation | Pure initializer/reducer work only; ordinary operational calls return NotReady |
| Candidate → Active | Native record, registry seal and permission activation are coherently committed, then exposed |
| Candidate → Retired | Discard only candidate resources; it cannot stop or clear an existing active owner |
| Active → AdmissionClosed | Required denial, explicit revoke/disable or context retirement closes affected work; no new ordinary mutations |
| AdmissionClosed → Retired | Exact-owner native cleanup/drain completes or remains explicitly pending without restoring admission |
| Active old + Candidate new → Active new + Retired old | Exact-key atomic handoff with current decision checks and preserved order |

These are conceptual states, not new public enum/API promises. U3f-f's three-phase
fixture is not a manager postcommit producer. In particular, existing
RuntimeScope::CommitActivation alone precedes record assignment and cannot expose
the new owner. Preallocate everything needed for publication, owner-record stamp,
scope activation and the terminal receipt before effects. After the last fence,
couple the authoritative record stamp and registry change under the same native
transaction; do not run Lua, persistence, logging, allocation or fallible callbacks.
Readers must not observe a new registry owner with old operational authority.

Normal replacement preparation leaves the old runtime and grants valid. Any
temporary admission reservation is distinct from irreversible revocation. If the
candidate fails before commit, keep the old owner only while its own decisions,
content and context are still valid. Concurrent/earlier revocation is sticky: a
failed reload cannot resurrect it. Failed candidate cleanup must not erase old
patches or consume the old owner's key. Do not call existing RequestStop on old
scope merely to prepare a replacement and later pretend to roll it back.

## POWN-07 — Exact-owner removal and atomic handoff

Native maintenance reservations are private, one-shot and narrower than ordinary
operations. They are issued only from the actual manager record and bind exact
registry/domain, old incarnation key, expected registry generation, lifecycle
revision, affected targets and purpose. A key alone is not general write authority.
Retain the owning objects, not a reusable address. No script/MCP call accepts an
arbitrary owner/key, invokes a maintenance bypass or selects another mod's state.

**Removal:** close the actual owner's operational admission, prepare removal of
only its exact sealed set, revalidate key/generation/transaction under the shared
writer, and publish at most once. Revoked script permissions do not block this
native release-only maintenance. It cannot add patches, change other owners,
activate a replacement, reveal revoked data or imply live-game restoration.
A stale key/task from a retired owner must not remove a successor with the same ID.
Prepare for any failure before closing admission where practical; once closure is
irreversible, failure retains explicit closed/cleanup-pending state, never Active.

**Handoff:** prepare a direct old-key → new-key replacement of that one owner's
set through the existing registry algorithm. Preserve activation sequence and
other owners; do not publish Deactivate then Activate or momentarily expose vanilla.
Revalidate old/current and new/candidate record identities, both content pins,
expected generations, required decision revisions and current factual gates at
the last fence. Publish the new sealed set and manager ownership stamp coherently,
make old admission irrevocably retired, then expose only the committed successor's
operational grants. No transient double owner is admissible. An independent
revocation affecting a required handoff decision that wins before the fence
invalidates the handoff. After commit apply revocation to its exact reviewed scope:
an old-incarnation-only decision cannot revoke an unrelated successor, while an
installation/content/session-wide decision covering the successor closes it.
Delayed old cleanup or revocation cannot acquire successor authority or resurrect
the predecessor.

Startup lifecycle publication is not a script operational task and does not
manufacture permission to queue one before activation. When an already admitted
operational task participates in a qualified mutation, retain its original task
and outcome rather than creating a parallel lifecycle task. Maintenance identity
is not another live grant store or scheduler.

Removal/handoff preparation may allocate; publication may only exchange prebuilt
state. Retired keys/stores/resources remain retained until all reservations release,
then drain outside manager/registry/context/scope locks and outside the script lane.
Use domain → manager/owner-record → registry → applicable context → scope ordering;
multiple scope reservations use a canonical native ordering, never mutable mod IDs.
Do not wait for a domain while holding manager/VM locks. A preflight failure has no
registry effect. Unexpected partial post-marker failure closes affected admission
and reports the actual committed/indeterminate state; no automatic replay or claim
that persistence rollback restored authority.

The sticky global stop permits release-only owner resource retirement. It does not
authorize a new successor or a general AdvanceRollback bypass. A stopped-domain
registry cleanup needs a separately qualified narrow maintenance reservation
proving that no future observer can treat removed state as an active game write;
until that exists, retain sealed state and dependencies for safe process shutdown.
Do not reuse the existing two legacy activation-recovery exceptions for new work.

## POWN-08 — Registry, persistence and game effects

Registry removal/handoff does not prove disk persistence, native object restoration
or actual gameplay. Context-appropriate native refresh may require a region reload
or restart. Unknown/MP context or lost lifetime denies new live mutation; do not
follow an old pointer because native cleanup was authorized. Safe copied-registry
release remains distinct from game writes. Keep one original task until its declared
postcondition; a registry-only maintenance record explicitly says live not requested
or pending and is not subsequently rewritten as gameplay success.

Durable intent/order updates must be planned outside commit fences. Specify their
failure and recovery protocol before production integration; no atomic disk+RAM
transaction is presumed. On restart reconcile only qualified durable state with
fresh native identities and approvals, never restore pointer/key/epoch handles.
No rollback re-enables separately revoked grants or reverses external side effects.

## POWN-09 — Implementation and qualification sequence

1. **Proposed U3f-h:** isolated typed ordinary-script/agent provenance and closed
   decision-adapter fixtures. Map to the sole RuntimeScope evaluator, test mandatory
   intersections, nonce/revision/expiry rules, required/optional behavior, causal
   routing and immutable task delivery. No real UI/ledger/producer activation.
2. Isolated exact-key maintenance/removal and atomic successor handoff using the
   shared registry algorithm. Cover coherent owner-record/grant publication,
   failed candidate preservation, revocation races and bounded retained resources.
   No public mutation or legacy bypass; audit before any manager wiring.
3. Qualify actual manager lifecycle, durable identity/decision storage, trusted
   input and native context producers, each against its own source/runtime evidence.
   Nonblocking game observation remains a hard separate REA/context gate.
4. Joint Base/SDK/public cutover only after all applicable gates; remove superseded
   legacy mutation bindings in that accepted unit. No pre-Beta compatibility shim
   or production fallback to fixture authority.

This sequence is a proposal for later acceptance, not implementation authorization.
No new dependency, helper executable, network access, instruction/config activation,
game/VM operation or crash test is granted by this document.

## POWN-10 — Required future negative oracles

| Family | Required failure/success evidence before promotion |
| --- | --- |
| Role/provenance | OrdinaryScript works without agent login; Agent cannot shed its route; forged kind/actor/channel/owner and lent facade denied |
| Consent/capabilities | Session OR entity never skips mandatory gates; wrong extent denied; later participants limited by explicit session; required denial blocks activation; supported optional denial gives restricted mode |
| Human ingress/ledger | Forged Base confirm, prompt mutation/replay, absent/corrupt records and old revocation ledger denied; persistence failure cannot resurrect issuance |
| Time/revisions | Expiry/revoke before fence prevents effects; postcommit denial hides data without undo; fresh reservation never mutates an old task or refreshes consent |
| Lifecycle | Initialization cannot operate; failed candidate leaves still-approved old owner; required revocation plus failed reload never revives old authority |
| Sealed registry | Two valid same-ID VMs cannot overwrite/remove each other; stale key cannot clear successor; atomic transfer preserves order/other owners and exposes no gap/double owner |
| Fences/recovery | Allocation/persistence/receipt failures before marker preserve state as specified; cleanup after irreversible closure stays closed; contention fails boundedly; no destructor/Lua/I/O under fences |
| Context/live | Unknown/MP/lost lease blocks live effects; registry-only release never claims restoration; original task completes once after its actual postcondition |

All new producer, maintenance, durable recovery and gameplay tests above are
**NOT RUN** in U3f-g. U3f-f's accepted 23/23 host tests and 558 semantic assertions
are historical evidence for its narrower closed contract, not these future claims.
