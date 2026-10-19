# Manager ownership and immutable candidate integration

Status: **U3f-j architecture contract, refined by the U3f-m1
[canonical cutover plan](manager-cutover-plan.md). No production activation.**
U3f-i's isolated lifecycle model is accepted. This contract applies the
[permission/owner requirements](permission-owner-lifecycle-contract.md) to the
actual ModManager. DLL owns native identity, validation, publication and scope
state; Luau base presents APIs and settings; SDK describes public contracts;
DevTools supplies authenticated tooling ingress. None becomes a second grant
store, scheduler or patch registry. Names below describe proposed internal
concepts, not shipped ABI or manifest fields.

## MOWN-01 — Source-verified starting point

| Observed fact at U3f-j discovery (not current line offsets after later units) | Historical source |
| --- | --- |
| Canonical records contain definition/fingerprint, pending candidate, VM, service supervisor, scope and script patches; no sealed origin/record stamp | `src/mods/mod_manager.hpp:109` |
| Manager ScriptHost supplies legacy readFloat only; semantic loaded-definition capture is conditional on a semantic host | `src/mods/mod_manager.cpp:449`, `src/scripting/script_runtime.cpp:360` |
| Registry publication releases its writer/registry reservations before callers activate scope and update scheduler/canonical record | `src/mods/mod_manager.cpp:480`, `626`, `645`, `847`, `866` |
| VM initialization runs entry/onEnable before publication; Commit and scheduler Put acquire locks and Put can fail | `src/scripting/script_runtime.cpp:433`, `472`, `622` |
| Enable can start services before persistence/publication; reload rejects service-bearing mods | `src/mods/mod_manager.cpp:584`, `785` |
| Stop/onDisable can wait on the script lane; existing retirement releases manager lock first | `src/mods/mod_manager.cpp:90`, `src/scripting/script_runtime.cpp:501` |
| Enabled-order persistence stores IDs, not reviewed content or decision/revocation state | `src/mods/mod_state_store.cpp:87` |
| Source fingerprint/cache key is not an immutable consumed-output/install identity; local compiled descriptor and bytecode are mutable together | `src/scripting/source_project.cpp:318`, `427` |
| Raw payload validation retains path/digest, not its bytes; raw-resource execution is currently rejected | `src/mods/resource_project.cpp:123`, `147` |
| Base capture already reads bounded reviewed files and compiles captured bytes | `src/scripting/base_host.cpp:33` |

These source facts are not new findings of exploited runtime behavior. Existing
generic scopes deny authority by default. U3f-i fixtures do not prove actual
manager integration, canonical record uniqueness, immutable dependency closure,
authentic decisions or native/live postconditions.

## MOWN-02 — Separate identities and one immutable consumed candidate

Keep discovery correlation, installation identity, reviewed content identity,
VM/scope generation, lifecycle revision, grant revision and operation reservation
distinct. Existing textual ID, version, canonical path, source fingerprint,
cache directory and timestamp are locators or diagnostics, never approval.
Matching content after reinstall may retain content equality but must not silently
retain an installation incarnation. The Base identity covers its full captured
file inventory and interpretation, not only the current manifest revision string.
Code/publisher trust and capability consent are separate. Captured instructions
or documentation remain untrusted data; capturing or hashing them cannot activate
agent instructions, change configuration or delegate additional authority.
Source-to-output provenance does not approve the compiler, update source or native
publisher. External tool execution must use separately approved procedures and
verified tool identities. Public diagnostics expose bounded reason codes and
nonsecret identifiers, never raw tokens, private absolute paths or mod payloads.

The proposed immutable candidate retains the exact effective ModDefinition,
normalized declarative patches, script language/entry, bytecode and modules,
aliases, limits and every supported runtime input actually consumed. Capture
those inputs once into bounded owned immutable storage, then parse, validate,
compile and execute from that storage. Do not re-open an approved path later.
The VM loaded-definition pin must refer to the same captured script fields;
the manager record must retain the complete candidate, not only its script pin.
Keep the actual file-read/size/count/decode limits explicit, cumulative and tested
before allocation. Identity/capture failure is a precommit rejection.

A deterministic versioned content digest covers a canonical inventory and exact
consumed bytes plus their interpretation, including manifest semantics, script
outputs and compilation configuration where relevant. Ambiguous duplicates,
unknown security-relevant version/fields and path aliases fail closed. A digest
establishes content equality, not authenticity of the compiler, consent or owner.
For source-authored mods, retain the source-to-output preparation provenance;
the mutually mutable local cache descriptor and output hash are insufficient.
Use existing parsers/hashing/compilation paths with a captured-input adapter,
not a parallel build tool or a bundled TypeScript compiler.

Capture must prevent traversal/reparse-point escape and mixed snapshots: resolve
relative inventory entries under the intended root, retain appropriate source
file identities/leases while reading, and derive the candidate from the captured
bytes even if a path changes afterward. Before/after hashing alone misses ABA
and cannot establish that manifest/modules/output belong to one stable capture.
If a coherent capture cannot be obtained, reject or require a fresh preparation;
never silently mix old and new inputs or overwrite an already reviewed candidate.
Do not infer this protection already exists in all current loaders.

An installation binding is loader-owned and refers to the actual canonical
manager record and immutable candidate. Process-local binding is sufficient only
for closed isolated tests. Durable installation identity, reinstall/copy rules,
publisher trust and restart decision reuse require a separately qualified design.
Until then no disk identity or persisted enabled ID can rehydrate a grant.

There is no general gameplay mod dependency graph in the current ModDefinition.
Service process dependencies are not that graph. The first implementation unit
qualifies only dependency-free, supported declarative/Luau candidates and an
explicit captured Base pin. It does not invent a new dependency manifest schema.
An input requiring unqualified cross-mod/resource dependencies is rejected on
the new route; silently ignoring a declaration or claiming complete closure is
forbidden. A future resolver must retain exact resolved versions/content and
bind the complete dependency closure before consent can be reused.

Raw assets, native modules, services and external tools are excluded from initial
sealed-route eligibility. Exclusion is an explicit gate, not fallback to legacy
mutation for the same sealed owner. Existing raw-resource runtime rejection and
EA-asset reference-only restrictions remain. No game files are copied into a mod
or distribution by this identity work. Native DLL file leases/staging can inform
later qualification but do not sandbox native code or authorize loading it.

## MOWN-03 — Actual canonical record and preparation

The actual records_ entry becomes the sole lifecycle record. Its immutable stamp
retains candidate, native origin, scope/endpoint/VM generation and sealed registry
ownership, with a monotonic lifecycle revision. Do not keep a second independently
mutable U3f-i fixture Record alongside it. Refactor that closed model into a
shared transaction participant or equivalent typed private coordinator; preserve
its exact-key checks and single registry preparation algorithm.

The first manager integration unit covers script-bearing supported Luau owners.
Capturing declarative definitions in U3f-k does not qualify declarative-only
lifecycle cutover: U3f-i requires an actual VM/endpoint. Declarative-only ownership
must later receive its own native record/scope identity independent of a VM;
never fabricate a VM, agent principal or fake operation grant. Until that separate
gate, those owners remain on the existing unsealed route and cannot be adopted
as sealed owners. Eligibility is explicit and a sealed owner never falls back.

Preparation occurs outside writer/manager/context/scope fences and does not expose
the candidate to scheduler ticks, ordinary semantic tasks, native DLL loading,
service startup or I/O-capable mod callbacks. Existing entry/onEnable is not
automatically a pure initializer. Define its allowed staged output precisely:
bounded copied declarations from the retained candidate, no live effects or
legacy bypass. Unqualified initializer access fails closed. Required old and new
decision reservations, when eventually provided, remain in RuntimeScope alone;
preparation does not manufacture them or translate a fixture approval into trust.

Create the semantic endpoint from the actual manager candidate and scope, not a
caller-selected ID/VM. Bind the actual Base package and schema/descriptor catalog.
Ordinary execution remains NotReady before commitment. This stage does not enable
the production catalog, authentic decision issuer or native context observer.
Their absence keeps production operations closed even after plumbing exists.

Reserve all storage before effects: record/order stamps, patch snapshot, scheduler
slot, VM activation state, retirement holder and any bounded result metadata.
Make absent→candidate→active a separately tested initial-activation transaction;
U3f-i adopted an already active fixture and did not qualify this transition.

## MOWN-04 — Single publication fence and scheduler visibility

One writer coordinates the actual manager record, existing registry and existing
scheduler visibility. The scheduler unit now qualifies isolated
`D → S → canonical V → R → B → canonical Q → late T`; the actual manager adds
its existing manager-wide `mutex_` as M between D and S. There is no new per-record
mutex. See [SPUB-03](scheduler-publication-contract.md) and the
[canonical cutover plan](manager-cutover-plan.md) for acquisition, reverse release
and lifetime requirements. The isolated fixture has no M and does not prove that
integration. Do not acquire D while holding M or the lane lock. The accepted
VM-to-D semantic attempt is a narrow try-only exception; publication likewise
tries V rather than waiting. No new blocking reverse edge or hidden replay is allowed.

The existing ScriptRuntime::Commit, ScriptScheduler::Put/Remove and allocation
paths cannot simply be moved into this fence. Introduce a private preallocated
publication reservation on the existing scheduler, with exact slot/owner/generation
validation and typed no-fail activation/unscheduling. It must not create a second
lane, wait on a lane future, invoke scripts, release old ownership or allocate
under the fence. Scheduler consumers must observe the shared publication protocol
or an equivalent generation-stamped visibility gate qualified by tests; a raw
lock-free registry snapshot is not a coherent authority view.

At the last precommit fence validate canonical record/candidate pins, exact
seals, expected registry/domain/lifecycle revisions, scheduler reservation,
owner availability, current decisions and factual leases. Reuse the sole scope
evaluator and U3f-i candidate-only activation check. Success exchanges prebuilt
record/registry/scheduler state, irrevocably retires old admission and pending
operations, and exposes only the committed successor while reservations remain
held. Keep old resources retained beyond all fences. Publish handoff directly,
preserving load order and unrelated owners; never deactivate then activate.

Initial activation, exact removal and same-ID handoff each have a precise marker
and immutable registry-only result. Unknown/MP/lost native lifetime denies live
effects. Removal requires native release-only authority, not an operational grant;
it cannot activate a successor or disclose revoked payloads. Global domain stop
does not gain an AdvanceRollback exception. Retain sealed state for safe shutdown
until a separately qualified stopped-domain cleanup protocol exists.

## MOWN-05 — Deferred retirement, failure and persistence

After commit, run onDisable, scheduler lane flush/close, service teardown, storage
destruction, diagnostics, cache invalidation and native refresh outside publication
locks. Exact old ownership and callback-once state survive until completion. A
blocked/faulty onDisable leaves admission closed and resources retained; it cannot
block the writer or expose new work. Preserve the existing single scheduler,
reentry denial and bounded retired-generation accounting.

Before the marker, failed capture/initializer/decision/slot/context validation
preserves the still-approved old owner. Concurrent revocation/stop remains sticky.
After a marker, do not report rollback or Failed as if nothing committed. Expose
actual committed registry state, closed cleanup-pending or Indeterminate as
appropriate; never automatically replay or reactivate revoked predecessors.
Exact repeated removal is idempotent only for its retained tombstone. An old
prepared removal cannot affect the successor even when textual IDs match.

Current enabled-order persistence is not a crash-consistent authority journal.
Integration must explicitly qualify persist-denied, publish-denied after disk
write, compensation failure, crash/restart and stop races. Disk work is outside
the fence; no RAM+disk atomicity is presumed. Before production promotion choose
and audit a bounded reconciliation protocol. Until a durable revocation witness
and fresh identity recovery exist, require fresh decisions after restart and
never reuse persisted grants. Checkpoint/rebase evidence is not runtime consent.

Registry publication and live native application remain separate. Preserve the
truthful committed-but-refresh-failed diagnostic. Keep an original operational
task until its declared postcondition rather than upgrading a registry receipt
later. No isolated result proves game restoration, save compatibility or UI.

## MOWN-06 — Staged implementation and acceptance

Each row is a proposed separately bounded unit requiring human acceptance of its
plan, relevant regression tests and independent audit; this document implements
none of them.

| Unit | Deliverable and hard boundary | Required evidence |
| --- | --- | --- |
| U3f-k | Isolated immutable capture of dependency-free supported declarative/Luau candidates; captured script-output provenance; no manager/production cutover | Same bytes consumed after path mutation; manifest/module/output ABA and mixed snapshot denied; traversal/reparse/duplicate/limit failures; cache tamper cannot inherit reviewed source identity; unchanged valid behavior |
| U3f-l | Typed activation/retirement reservation in existing VM/scheduler, with explicit complete lock-order contract | Stop/slot loss/content generation races fail before effects; no lane wait/callback/allocation/destruction at fence; in-flight ownership and exactly-once deferred teardown; candidate NotReady |
| U3f-m | Actual canonical-record sealed lifecycle for supported script-bearing Luau owners in isolated manager host: initial activation, remove, direct handoff and original script patch producer reconciliation | One record/origin/registry/scope/scheduler view; old queue cancelled; full capacity reclaimed; same-ID stale cleanup/replay denied; order preserved; no sealed owner enters legacy writer; declarative-only cutover stays closed |
| U3f-n | Persistence/recovery and remaining production provenance decisions/qualification plan | Denied persistence, failed compensation, restart with stale allow, missing/corrupt identity and interrupted retirement remain closed; authentic decision/native context/dependency providers individually audited |

The immediate proposed next unit is U3f-k only. Its concrete capture format,
limits and tests must be documented before its implementation; acceptance of this
design cannot silently approve U3f-l/m/n or a production catalog cutover. External
compilers remain separately installed. Base/SDK/DevTools changes require their own
bounded repository scope; keep this native plumbing in DLL-owned code.

Preserve all current host regressions, especially script lifecycle transaction,
scheduler in-flight/shutdown/allocation, owner lifecycle, copied-value quota,
context/revocation/expiry and production/fixture segregation checks. Add tests at
the actual integration boundaries, not copies of implementation logic. Live
qualification follows only after providers and user-approved sandbox are ready.

## MOWN-07 — Current verification limits

U3f-j was source discovery and design. No runtime code, configuration, active
instructions, public SDK, dependency, build, game/VM operation, crash trial,
network operation, commit, push or PR change is included. U3f-i's 23/23 tests and
1178 checks remain historical evidence for its narrower candidate. They are not
executed proof of this proposed integration. Exact current source hashes,
preservation checks, discovery evidence and independent verdict are retained in
ignored `.analysis/u3fj-2026-10-16/`. VM, crash trials and PR #7 remain parked.

Later U3f-k/l1/l2 qualified immutable capture and isolated joint plumbing, not the
actual manager. Current source/lifetime gates, initializer restrictions and ordered
implementation approvals are in [U3f-m1](manager-cutover-plan.md). Its discovery
must not be read as enabling a production issuer or replacing current legacy routes.
