# U3 Base Luau host profile

Status on 2026-10-19: isolated host, serial DevTools13 and both whole developer
install closures are qualified. All eight clone controls and the final composition
evidence phase pass; integrated review and explicit human whole-U3 acceptance
remain pending.
This profile describes SDK **0.2.0**, **base-luau-alpha-v1**. It is an alpha
presentation contract; it does not establish a public Beta compatibility promise
or a complete game API.

## Canonical contracts and ownership

The canonical [SDK Base API](../../../Simoder-SDK/Luau/BASE_API.md) owns the public
call/return shapes and [declarations](../../../Simoder-SDK/Luau/base.d.luau).
The authored Base [service contract](../../../Simoder-Base/docs/SERVICES.md) and
[dependency graph contract](../../../Simoder-Base/docs/DAG.md) own their internal
Luau behavior. Core's [private source host](base-host.md) and
[private runtime](base-runtime.md) describe native capture, execution and resource
bounds. Earlier per-unit results in those documents retain their historical scope.
Sibling-source links assume `Simoder`, `Simoder-Base` and `Simoder-SDK` checkouts
under one workspace parent; the package/profile identifiers remain authoritative.

Core owns low-level identity, the single RuntimeScope evaluator/task record,
copied-value storage, scheduler and lifecycle safety. Base owns authored Luau
presentation, the dependency graph and private singleton services. SDK owns public
declarations and conformance. Optional authoring, MCP protocol/client tooling and
developer distribution belong to DevTools. The production loader remains one
integrated x86 `d3d9.dll` beside `SimCity.exe`; isolated fixtures do not change
[repository ownership](repository-ownership.md).

## Supported presentation

The verified Base factory creates one frozen mod-local facade with exactly six
keys. All public functions use dot calls; the SDK remains the detailed reference.

| Key | Supported behavior |
| --- | --- |
| `values` | Native strict copied-value validation and canonical wire encoding through `validate` and `wire`. Values are immutable original-VM userdata; serialization grants no authority. |
| `request` | Queue one native-owned task through the actual Base dispatcher; return a frozen task or `nil, code`. Admission is distinct from completion and commitment. |
| `run` | Admit a host-registered coroutine on a later script-lane turn; return `true` or `nil, code`. A raw coroutine does not acquire registered authority. |
| `snapshot` | Read a frozen lifecycle observation. Epochs, generations, revisions, sequence, time and context identities use exact decimal strings; observations are not permission leases. |
| `subscribe` | Atomically capture the current observation and ordered subsequent changes. Return a frozen unsubscribe handle or `nil, code`; overflow is explicit `MailboxOverflow`, and unsubscribe returns a native status. |
| `descriptors` | Read frozen reviewed metadata, possibly empty, or `nil, code`. Discovery does not authorize execution. |

A host may expose no `base`; preparation may expose the facade while operations
return `NotReady`. Invalid callbacks, ownership violations and invalid await
boundaries can throw; exceptions are distinct from `nil, code` returns.
Task outcomes and the committed flag are independent. Cancellation after
commitment returns `Busy` and cannot undo effects. Callback and registered await
delivery occur on the original lane, with access rechecked and stopped-generation
continuations suppressed. Mod-local presentation does not copy the authoritative
registry or create a second task engine.

Each native endpoint binds once to its real VM generation and original scope.
Caller-supplied mod IDs cannot change that identity. Runtime/Base epochs, factual
context leases and owner generations remain native; captured task contexts are
immutable. Separate VM globals do not share mutable Luau tables, closures or native
pointers. Native DLL mods remain a distinct trust category from script isolation.

The five internal services (`values`, `tasks`, `lifecycle`, `registry`,
`runtime-manager`) are a private singleton graph, not additional public game
managers. Startup validates and freezes explicit dependency metadata before
factories, orders eligible services deterministically, rejects missing/duplicate/
self/cyclic dependencies, preserves independent optional branches and reverses
successful initialization after critical failure. A successful Lua startup alone
cannot issue native readiness or permissions.

## Qualification and remaining verification

The current Core semantic source identity is
`5d41107ec86d3e4c1ac624786479e521e53e2773df5598848ba6a8789be05209`.
The pinned public profile is recorded by
[`tests/base_sdk_profile.lock.toml`](../../tests/base_sdk_profile.lock.toml).
This identifies a source/profile cohort, not an exact binary hash or crash-symbol
match. Detailed immutable raw results and independent audit records stay in
ignored analysis evidence rather than becoming dependencies of this guide.

| Verification area | Current result and scope |
| --- | --- |
| Current Core compiled graph and host regression | Independently audited compiled provenance and one cold, ordinary-priority, serial unfiltered **41/41 PASS** on the current graph. Full boundary preservation was verified. |
| Base API, isolation, tasks and dependency behavior | The actual pinned Base graph and canonical SDK/example files execute in isolated native Luau tests, including original-owner denial, copied values, callback/await cleanup, ordered subscription overflow and startup failure/unwind. |
| Reload and native ownership composition | Both canonical queued and committed-undelivered reload modes and all five same-owner composition groups pass in the current suite. Persistence, cache, original-Plan retention and generation retirement remain scoped host qualification. |
| Separate NativeSDK integrity cases | Independently reviewed **PASS**: valid configuration succeeds; tampered, missing, wrong-revision and dirty configurations reject with their expected diagnostics. Mutations are isolated and canonical inputs are preserved. |
| Final Core-linked DevTools consumers | Independently audited serial **13/13 PASS**, including DevBridge core **30/30 PASS**, on the corrected managed cohort with audited original native-proof reuse. Original failed cohorts remain preserved; their historical root causes are not newly diagnosed. |
| Whole developer install closures | Ordinary component and full default installs both have independently audited **PASS**, with their complete source-owned payloads, licenses, drivers and declarations retained. Neither whole tree contains external TypeScript/typescript-to-lua/Node compiler implementation; legitimate native Luau.Compiler and .NET runtime/JIT payloads remain required owners. |
| Disposable exact-clean/revision/dirt clone controls | Independently audited **8/8 PASS**: each Authoring/Examples positive clone completes full configuration; wrong local revision, tracked dirt and untracked dirt reject with the intended exact guard diagnostic. All cases use real local no-hardlink clones and preserve canonical inputs and both installed trees. |
| Final composition evidence phase | Actual **PASS**, preserving the finite receipt chain, exact source/native cohort and both whole installed closures. Final integrated engineering/security/knowledge review, owned checkpoints and explicit human whole-U3 acceptance remain separate. |
| Declaration analysis | Grammar parsing and actual runtime conformance are qualified. **Luau.Analysis/editor inference is NOT_RUN**. |
| Regular-file symbolic links | Win32 error 1314 leaves regular-file symlink execution **NOT_RUN / SOURCE_ONLY**. Actual mount-point rejection is a different reparse-tag test and does not close this slot. |
| Game, operating-system VM and intentional crash tests | No game qualification or executed VM/crash result is claimed. VM/crash authorization is separate from execution evidence. |
| Whole-U3 acceptance | Final integrated engineering/security/knowledge review and explicit human acceptance remain pending. |

The production descriptor catalog and privileged authorizers remain closed.
Fixture issuers/producers and the macro-only reservation-refresh adapter qualify
real isolated Luau behavior; they are absent from the production profile. A later
enabled production provider needs an original-owner reservation-refresh policy
after domain revision changes, preserving grant expiry/revocation, bounded
ownership and immutable task contexts. Fixture refresh does not supply that policy.

The existing TypeScriptToLua authoring regression is separate from this Luau alpha
host and from future JavaScript/TypeScript runtime selection. External Node and
compiler dependencies belong to authoring. The separately qualified installed closures do not
establish a JS engine or Base TS/JS runtime support; those results remain
separate from the Luau host tests.

## Lifecycle and migration limits

The [owner lifecycle](permission-owner-lifecycle-contract.md),
[scheduler publication](scheduler-publication-contract.md),
[manager cutover](manager-cutover-plan.md) and
[persistence/shutdown contract](manager-persistence-shutdown-contract.md) retain
their exact ownership and stage boundaries. Shared native commit and cooperative
file-writer paths qualify bounded logical publication, file intent/compensation
and cache settlement; they do not form a crash-proof disk-and-memory transaction.
Persistence intent cannot recreate permission grants, and file-only reconciliation
cannot replay a native memory marker.

The known-owner inventory remains partial: universal `finalizable=false` and
`remainingRetirement=true`. VM/slot acknowledgements and completed known resources
do not certify whole-manager or whole-loader finalization. **E5-OWNER-CAPTURE-001
HIGH remains open**, including original native query/removal lifetime and ignored
hook-removal outcomes. U4 owns reconciliation, U5 deployment remains blocked and
U6 requires actual ABI/process/game proof or an independently reviewed stronger
lifetime protocol. No safe global shutdown or dynamic DLL unload is certified.

Unsealed legacy ingress and the existing `SaveEnabledState` route (tracked as
`legacySaveEnabledState` debt) remain temporary stage-owned U5 migration debt
under the cutover contract. Sealed canonical owners cannot fall
back into that route. U3 host results do not certify production cutover, gameplay,
the separate diagnostic/crash work, or the **Strictly NO Legacy** Beta RC gate.
The [release hygiene contract](release-hygiene.md) and every Beta requirement in
the [roadmap](../roadmap.md) remain applicable.

Historical failed cohorts and their timing causes remain distinct: a later passing
cold run does not diagnose the earlier failures; those causes remain UNKNOWN.
After final whole-U3 human acceptance, U4 is the next proposed stage, subject to
its own approved scope.
