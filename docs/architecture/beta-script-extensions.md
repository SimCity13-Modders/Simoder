# Beta script extensions: hooks, timers, utilities and localization

> **Design proposal.** The document describes a target Beta API and acceptance
> plan; it does not assert that every listed extension is implemented. Current
> scripting behavior is documented in [scripting](../scripting.md).

Status: product requirements accepted in the 2026-10-25 Grill-Me conversation,
including controlled mutable references across isolated mod VMs. Discovery is
source inspection, not an implemented or runtime-qualified SDK. Exact signatures,
native ABI, quotas and the synchronous cross-VM mechanism need a concrete design,
isolated proof and independent architecture/security audit before implementation.

This is a required Beta feature stream alongside the existing U1-U6 migration.
It does not silently enlarge U4/U5 or reopen accepted U3 implementation scope.
See [Base architecture](base.md) and [script scheduler](script-scheduler.md).
Method names below specify requested functionality, not shipped declarations.

## Accepted decision register

| ID | Accepted requirement |
| --- | --- |
| H-001 | Hook other mods' script functions without requiring their authors to declare cooperation. Arbitrary native function interception is excluded. |
| H-002 | User approval identifies the target mod and scope of interception. Callback execution retains the registering mod's identity and permissions. |
| H-003 | Execution order is PreHook, active override or original body, then PostHook. Override does not automatically execute the original or a next override. |
| H-004 | Last override wins according to deterministic load configuration. Show conflicts and the active winner. Documentation recommends OverrideFunction only when pre/post hooks are insufficient. |
| H-005 | GetReturn reads the current invocation result. A PostHook can replace it; later posthooks see that replacement. Document multiple results and nil explicitly. |
| H-006 | A failed override fails the invocation; never automatically execute the original as fallback. Existing DEV/REGULAR fault policy remains. |
| H-007 | Ordinary PostHook runs only after success. Error/finally handling has a separate contract and cannot silently hide failure through a normal PostHook. |
| H-008 | DelayedCalls supports real time by default and simulation time stopped by simulation pause. Deliver through the scheduler, allow cancellation and retire pending calls with their mod generation. |
| H-009 | Localization fallback is exact game locale, author-declared fallback, then mod default. Resolve missing keys the same way; finally display the key and emit diagnostics. No automatic variant guessing. |
| H-010 | Guaranteed Beta targets are functions reachable through named objects/modules. Private local functions hidden inside closures are excluded. |
| H-011 | RegisterHook creates a public namespaced event with an argument/result contract. Its owner emits by default; other mods may subscribe under their own permissions. Retire generation-owned registrations/subscriptions. |
| H-012 | Function hooks are synchronous, bounded and cannot await. They may schedule later work, which cannot change an already completed invocation. Asynchronous notifications have a separate contract. |
| H-013 | GetFunction returns the current body without pre/post hooks, including the active override. It preserves permission and generation checks. An expired handle fails instead of rebinding. |
| H-014 | Foreign mutable arguments use controlled references/proxies, not detached copies: an approved prehook can modify the real settings object before the target body runs. Preserve VM isolation and native authorization. |

Beta also requires a documented Utils surface including IsCityLoaded, multiple
translation files such as pl-PL and en-GB, and a configurable default locale.
The scope targets no additional external End User runtime, service or executable.
Size, latency, memory and dependency compliance require measurements, not assertion.

## Ownership and current implementation gap

| Owner | Responsibility |
| --- | --- |
| Simoder DLL | Verified VM/owner/target identities, scoped invocation and object handles, permission enforcement, bounded scheduling, lifetime, accounting and safe retirement. |
| Simoder-Base | Public project-authored Luau APIs; hook ordering/body selection, events, timer/localization/utility semantics using narrow native primitives. No second scheduler or authority store. |
| Simoder-SDK | Canonical public declarations, errors, examples, compatibility rules and conformance tests against actual behavior. |
| DevTools/MCP | Consume these APIs and expose applicable diagnostics/tests; no ownership of the public Hooks implementation. |

Current Base requests queue immutable copied inputs for a later Base turn. Its
contract provides no synchronous cross-VM entry from a mod callback. The copied
codec contains no closure or general mutable foreign-table representation.
Existing same-lane native controls are not proof that nested mod VM entry is safe.
Therefore H-012/H-014 need a reviewed extension of the invocation model; do not
pretend that an asynchronous task facade implements synchronous function hooks.

Keep independent heaps and the one existing script lane. Do not transplant a
callback into the target VM to borrow its capabilities, expose raw Lua/native
pointers, copy arbitrary executable closures, enable unrestricted debug APIs or
merge mods into a common mutable VM. Target discovery needs a host-mediated view
of named mod objects/modules, not unrestricted foreign require or globals access.

## Function interception contract

The required success path is ordered prehooks, one selected body, then ordered
posthooks. The selected body is the last applicable override or the original when
there is no override. Replacing a body preserves surrounding hooks. Override is a
compatibility-sensitive last resort: documentation must explain why adding a
pre/post hook is preferred when it can express the change.

Last-wins selection must follow configured mod order, not thread timing or arbitrary
late registration. Registration/replacement rules within one owner, ties, reload
publication and removal of the winner need an exact deterministic contract and
tests before SDK publication. Expose owner identities and inactive contenders so
users can inspect conflicts; never claim that multiple replacements were merged.

GetFunction bypasses only the pre/post chain and exposes the current selected body
through a checked handle. The retrieval/invocation linearization point and retained
body lifetime need specification. Old handles cannot silently acquire a new owner's
generation or bypass native grants. No API here promises an always-original handle.

GetReturn must belong to a private invocation frame, not one global current-hook
variable. Preserve result count, explicit nil, interior/trailing nil and multiple
values. Specify the distinction between no replacement and an explicit nil result;
the candidate rule is zero returned values means no replacement, whereas a nonzero
return count replaces the result tuple, including nil. This engineering detail
requires SDK conformance qualification; it is not inferred from Lua array length.

Ordinary posthooks execute only after successful preceding execution. Failure
propagates without automatic replay, original-body fallback or success-shaped
result. Writes/effects already performed are not rolled back by catching an error.
Separate error/finally facilities must document delivery and failure limits;
do not promise script cleanup after a native crash, exhausted memory or VM teardown.

H-010 is a target-discovery scope, not evidence that replacing a table field catches
previously cached aliases, recursive local calls or compiler-inlined calls. Qualify
these paths and frozen-table methods explicitly. An implementation that only hooks
future calls through a named slot must describe that boundary and must not silently
downgrade the accepted scope. If qualifying broader coverage requires material VM
instrumentation, bring that design change to review before proceeding.

## Cross-VM reference and execution safeguards

H-014 preserves authorized object identity: if two arguments name the same object,
their proxies must not invent two unrelated copies. Writes must reach the owning
VM's actual object. Define self, nested objects, cycles, comparison, iteration,
metatables, function-valued fields, callbacks, userdata and read-only/frozen values.
Do not claim transparent support for any of these until tested. Unsupported shapes
fail clearly; never silently copy a mutable reference and report a successful edit.

Proxies bind the actual owner/generation and permitted access. Recheck live grants
and target validity on use; release handles safely on the lane. No old proxy may
bind to an equivalent-looking object after reload. In-flight references retain
their actual owners until safe release; revoke admission before cleanup. Retaining
an object does not retain permission to access it.

User approval of broad hook/data access can expose ordinary target-mod state. Make
that consequence clear; proxying is not automatic redaction. Native credentials,
authority objects, private Base capabilities and protected host enforcement paths
are not ordinary hook targets. Do not thaw privileged facades or permit hooks to
bypass permission, commit, epoch or grant enforcement.

Track both the original initiating provenance and the currently executing hook
owner. A target call must not launder an agent action into an unrelated native
origin or let a callback impersonate another mod. Exact delegation/taint rules,
including writes that influence a later target operation, require security review.

Use scoped stack restoration, retained VM owners, bounded recursion/fan-out and
a chain-wide deadline that nested hooks cannot reset. Preserve memory charges for
handles, copied data and proxy metadata. Reject await/yield in synchronous hooks.
No VM entry while manager/publication/authority locks are held, no wait for a turn
on the occupied lane, and no direct socket-thread Lua execution. A Lua execution
budget cannot preempt an arbitrary blocking native call; exposed primitives remain
bounded and reviewed. A-to-B-to-A reentry is an explicit proof obligation.

## Custom events, DelayedCalls and Utils

Custom event names belong to their verified owner's namespace. RegisterHook,
subscription, unsubscription, emission and return-valued events need versioned
contracts, bounded schemas and deterministic ordering. Public subscription does
not authorize impersonating the emitter or accessing its private state. Scope
callback failures and generation retirement consistently with function hooks.

DelayedCalls reuses the existing scheduler. Real time uses monotonic deadlines;
simulation time uses verified simulation progress and pause state, not wall time
or guessed onTick intervals. Define cancel/replace races, same-owner ID reuse,
invalid/nonfinite delays, menu/no-city transitions and per-turn fairness/bounds.
Passing a deadline makes work eligible, not guaranteed to run at that instant.
No unbounded catch-up burst or background timer thread is implied.

Utils is a convenience facade over authoritative domain/context queries, not a
second game-state store. IsCityLoaded must distinguish a qualified false from
unknown/not-ready/stale context; exact return/error shape belongs in the SDK.
Base readiness or a graphics/window observation is not proof of a loaded city.
Availability checks never reserve authority for a later mutation.

## Localization

Load several bounded mod-owned translation files, select the exact game locale and
apply explicit author-declared fallbacks followed by the mod default. Resolve each
missing key through that chain; on final failure expose the key plus diagnostics.
Validate locale identifiers, fallback cycles, duplicate/overlapping files and keys,
encoding, path containment, file size and dictionary budgets. Namespace mod keys.

Freeze an inert dictionary format and interpolation/escaping rules before SDK
publication; JSON data is a discovery candidate, not an approved new parser.
Translation loading cannot execute arbitrary scripts or export EA dictionaries.
Stage reload and publish a whole validated dictionary generation atomically.
Language changes, text lookup, game UI/font support and live visual refresh have
separate conformance evidence; successful dictionary loading proves none of the
other paths. Do not automatically guess en-GB when en-US is unavailable unless
the author configured that relationship.

## Implementation plan and acceptance

Use one large Beta scripting-extension stage with one status/decisions record,
final report and independent final audit. The following are work packages within
that stage, not new per-fix acceptance dossiers.

1. **Invocation and proxy feasibility.** Use real isolated Luau VMs to prove
   target discovery, controlled mutable identity, supported value shapes, nested
   call frames, revocation and a bounded synchronous lane path. Specify gaps in
   cached aliases/frozen methods and JS/TS representation. Independently audit the
   proposed extension to current asynchronous-only cross-VM semantics before
   production implementation. Unresolved mandatory coverage blocks completion.
2. **DLL primitives and Base Hooks.** Implement narrow native handles/lifetimes/
   checks, then Luau orchestration, conflicts, result frames and custom events.
   Keep public policy/orchestration out of permanent native Core except required
   enforcement. Add conformance and existing regression tests alongside behavior.
3. **Timers, utilities and localization.** Reuse that ownership infrastructure and
   scheduler. Qualify factual city/pause/clock sources and dictionaries, generation
   cleanup and HotReload. Do not substitute guessed state for missing observations.
4. **SDK, examples and diagnostics.** Update owning repositories' declarations,
   API guides and examples together. Document override warnings, all ordering,
   result/error, permission, cancellation, fallback and stale-handle semantics.
   Provide conflict inspection, bounded errors and correlation without exposing
   protected objects. JS/TS parity needs the selected JS engine and its own tests;
   Luau tests do not certify cross-language interception.
5. **Regression and release qualification.** Run isolated positive/negative and
   concurrency tests, final independent engineering/security audit, then separately
   approved installed-game validation. Measure x86 size, memory and latency and
   inspect dependencies. Include this stream in Beta/SDK/NO Legacy acceptance;
   do not report completion based only on declarations or synthetic tests.

Required coverage includes two independent real VMs; approved/denied/revoked
targets; pre/body/post order; last-wins conflict visibility; no implicit original
execution; no posthook after error; nested GetReturn; zero/multiple/nil returns;
mutable identity/alias/cycle and unsupported types; A-to-B-to-A; chain budget
exhaustion; stale GetFunction/proxy rejection; reload/disable during dispatch;
no permission laundering; retained owner cleanup and no old-generation delivery.
Also cover timer pause/cancel/replace/fairness, unknown city state, partial/missing
translations, invalid fallback graphs, malformed/oversized files, atomic reload
and real UI refresh wherever promised. All these new runtime tests are NOT_RUN.

## References

- [SuperBLT Hooks](https://superblt.znix.xyz/doc/common_functions/hooks/) and
  [implementation](https://github.com/diesel-modding/PAYDAY2-SuperBLT-Lua/blob/master/req/core/Hooks.lua)
  inform selected ordering and override semantics, not cross-VM compatibility.
- [SuperBLT delayed calls](https://superblt.znix.xyz/doc/common_functions/delayed_calls/)
  and [localization](https://superblt.znix.xyz/doc/common_functions/localization/)
  are reference APIs; Simoder's ownership/clock/fallback contracts are explicit here.
- [Luau sandboxing](https://luau.org/sandbox/) and
  [performance](https://luau.org/performance/) inform isolation and call-coverage
  proof obligations. Target Simoder behavior still needs local qualification.
