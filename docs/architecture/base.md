# Base manager and mod API architecture

> **Design reference.** Active Luau Base source is in the
> [Simoder-Base repository](https://github.com/SimCity13-Modders/Simoder-Base).

Status: design decisions accepted by the user in the 2026-10-15 base Grill-Me
conversation. This document records the agreed scope; it does not claim that base,
its managers, task API or cross-VM transport are implemented or runtime-qualified.
Method and manager names in examples are illustrative, not shipped SDK symbols.

See [repository ownership](repository-ownership.md). An internal primitive is
not automatically a public script API.

## Layer ownership

| Layer | Responsibility |
| --- | --- |
| Simoder DLL | Low-level operations and verified entry points, native identity, execution/thread boundaries, grants, bounded dispatch and resource lifetime enforcement. |
| Simoder-Base | Entirely project-authored Luau abstraction layer translating those primitives into high-level semantic APIs, lifecycle, UI and cooperation interfaces. |
| DevTools | Optional developer-facing functionality. MCP owns its protocol, transport and tool catalog; it consumes semantic base APIs and appropriate bounded native primitives. |
| SDK | Canonical declarations and documentation for accepted public contracts, with conformance checks against the actual implementation. |

Public abstractions are authored by this project, not EA source classes. The
original manager design does not require game source redistribution or a general
native function-address interface. The accepted 2026-10-25
[Beta script extensions](beta-script-extensions.md) add named script-function
hooks, controlled cross-VM object proxies, timers, utility queries and localization.
These are required Beta scope, not implemented capabilities of the current host.
No protocol parser or MCP catalog moves into permanent core or base merely to
expose a manager method.

MCP remains an optional DevTools mod for informed developers, including AI-assisted
development. Test regions and copied saves limit save-related consequences; they
do not isolate native code from the game process or undo external effects. Native
mod trust and agent operation consent remain separate decisions.

## Accepted decision register

| ID | Accepted choice |
| --- | --- |
| BASE-001 | Hybrid organization: one public manager registry, singleton managers, explicit internal dependencies. |
| BASE-002 | Public manager views automatically bound to each mod's verified identity; explicit context internally. |
| BASE-003 | Mixed execution: synchronous snapshot reads and asynchronous game work. |
| BASE-004 | Task objects with callbacks and optional coroutine awaiting. |
| BASE-005 | Automatic generation-owned task, callback and subscription cleanup. |
| BASE-006 | Explicit invalidation of handles when their context expires; no automatic rebinding. |
| BASE-007 | Atomic subscribe-with-current-state followed by ordered changes. |
| BASE-008 | Explicit initialization dependency graph, missing-dependency and cycle detection. |
| BASE-009 | Startup failure propagation according to explicit module criticality and dependency relationships. |

The user selected each choice during the side conversation and then requested
this documentation update. These are accepted design requirements, not fabricated
implementation tests, independent audit verdicts or deployment approvals.

## Modular managers and internal services

Each public manager has one authoritative instance per runtime. A registry offers
convenient discovery; it is not permission to reach into arbitrary implementation
services. Managers expose cohesive domain interfaces, delegating work to small
services with explicit constructor/factory dependencies. Avoid global service
lookup throughout internal implementation and avoid an all-purpose manager.

Use encapsulation, narrow interfaces and composition by default. Classes and
inheritance are appropriate where they express real substitutability; consumers
must not need to know a concrete subclass. Apply SOLID and DRY to behavior and
ownership, not merely to file names or the number of classes. Share a canonical
implementation rather than independently evolving copies of manager logic.

Runtime-singleton lifetime is distinct from region/city state. Managers hold or
resolve explicit context objects; switching region/city replaces the relevant
context and invalidates its handles, rather than silently reusing old state.
Independent domains do not all need a loaded city to be available.

The single authoritative manager instance is a semantic requirement, not a shared
mutable Luau global injected into every mod VM. Per-mod facades may be local
proxies; they must not duplicate authoritative registries or expose another VM's
pointers. Exact base VM placement and cross-VM scheduling/transport require their
own implementation contract and evidence. This document does not remove existing
script isolation to imitate global objects in another game's mod loader.

## Mod-bound public views

Authors access an identity-bound view without supplying an authorization context
to every method:

```lua
-- @summary Illustrative API, not an implemented declaration.
local navigation = base.managers.navigation
navigation:someOperation(...)
```

The host binds the facade to verified mod identity and generation. The facade
forwards an explicit host-validated context to internal services and the shared
manager. It is not another manager instance, a mutable caller-ID argument or a
permission grant. Retaining a facade cannot preserve authority after disable,
reload, revocation or context expiry.

Capture and restore the correct originating mod context across asynchronous
tasks and callbacks. Cross-mod cooperation must use its explicit API rather than
passing an authoritative facade to impersonate its owner. Check applicable grants
at execution/commit, not only when a view was created or a method was discovered.
An availability check is helpful guidance, never a reservation of permission.

Report useful errors distinguishing unavailable capability, denied permission,
invalid/expired context, readiness failure and operation failure. Do not expose
credentials or privileged internal state through diagnostics. Native authority
remains enforced by core; a Luau wrapper is not a replacement security boundary.

## Snapshots, tasks and callback experience

Synchronous reads return a snapshot whose context and freshness are explicit.
They must not perform hidden blocking game-thread work. Operations requiring game
execution return a task immediately. Acceptance into a queue is not successful
completion; success means the operation's required outcome has been confirmed.

Task objects provide status, successful completion, failure and cancellation
requests. Authors may subscribe to completion/failure callbacks or await a task
inside a permitted coroutine. Both styles observe the same result and error
semantics; awaiting suspends that coroutine, never the game thread.

```lua
-- @summary Illustrative API; this does not add an executable operation.
local task = base.managers.regions:loadCity(cityId)

task:onCompleted(function(city)
    print(city.name)
end)

task:onFailed(function(error)
    print(error.message)
end)

-- Optional alternative inside a permitted scheduled coroutine:
-- local city, error = task:await()
```

Cancellation is a request, not a rollback promise. Uncommitted work may be
cancelled; committed work reports its actual completion/failure or an explicit
indeterminate outcome. A timeout or disconnected caller does not authorize
automatic replay. Do not retain a suspended coroutine indefinitely after its
owning mod generation stops. Exact task declarations, callback scheduling and
terminal-state details must be qualified before SDK publication.

## Resource ownership and context invalidation

Tasks, subscriptions and callbacks belong automatically to the originating mod
generation. Disable/reload closes that generation's admission, detaches callbacks
and subscriptions and cancels uncommitted work. Already committed operations may
finish with native resources retained safely, but must never invoke code in the
old/unloaded mod generation. Manual author cleanup is not the sole safety mechanism.

Host resource retention/drain remains authoritative even when a script callback
fails. No executing native code or dependency may be unloaded to satisfy a timeout.
The task abstraction does not introduce a new rollback, region restore or native
fault-isolation guarantee.

Entity/region/city handles bind their identity to the relevant context and
generation. When that context expires, access produces an explicit stale-context
error; an old handle never starts pointing at an equivalent-looking object in
another city or region. Authors obtain a new handle through the appropriate
manager. Runtime-wide views need not expire on every city change, but cannot
silently turn stale context-specific handles into valid ones.

## State subscriptions

Subscribe-with-current-state is one atomic contract: deliver the initial snapshot,
then subsequent ordered changes, without a read/subscribe race. Identify context,
generation and ordering so consumers can reject stale delivery. Late-loaded mods
receive current state without requiring replay of the entire game's history.

Callbacks run on the qualified script execution scheduler and outside manager
locks. Internal U1 lifecycle notifications do not establish that scheduler: their
delivery thread may differ, so the base adapter must marshal appropriately. Stop
or unsubscribe prevents new deliveries, with in-flight resources retained until
safe completion. Do not call a VM directly from an arbitrary publisher/socket thread.

## Initialization and partial availability

Modules explicitly declare dependencies and criticality. Validate the graph for
missing dependencies and cycles before admitting dependent functionality; initialize
in dependency order and unwind successfully initialized resources on failed startup.
Dependency injection and deterministic construction make tests independent of
accidental first-use/global initialization order.

A failed critical module prevents base readiness. A failed optional module makes
its functionality and dependent consumers unavailable, with a clear diagnostic;
independent modules may continue. A missing required dependency cannot be ignored
because the failed module was labelled optional. Criticality is an authored and
reviewed base contract, not an untrusted mod's mechanism to declare itself critical.

This decision addresses module startup/availability. Existing DEV/REGULAR runtime
fault policies, native trust and operation permission rules remain separate and
must not be weakened by optional-module degradation. Exact critical module lists
are not invented here and must accompany the implementation's dependency graph.

## SDK conformance and JavaScript discovery

Add conformance coverage tying public Luau/native and applicable JS/TS declarations,
documented behavior and error contracts to the actual implementation. Declaration
generation or matching names alone is insufficient. Document supported versions and
update relevant declarations/examples when behavior changes. Public Beta establishes
the backwards-compatibility baseline under the existing
[compatibility policy](compatibility.md); this does not introduce alpha migrations.

The game already has a JavaScript execution environment used by the current
[native script bridge](../../src/dev/session/native_script_bridge.cpp) and
[session router](../../src/dev/session/session_router.js). Reusing that engine for
mod JS/compiled TS is a discovery candidate, not proof of suitable sandboxing.
Verify language support, isolated contexts, memory/execution limits, API exposure,
lifetime, reload and cross-mod behavior before choosing a runtime. Do not assume
Node.js APIs or an independent safe mod VM from the existence of UI JavaScript.
An additional engine needs evidence and the existing dependency/size approval.

## Required implementation evidence

Validate mod-bound facade identity across callbacks/await, forged or stale context
rejection, commit-time revocation, snapshot freshness, task completion versus queue
acceptance, cancellation before/after commit, generation cleanup and no old callback
after reload. Verify handle invalidation on context changes, atomic initial state
delivery, ordering and safe callback scheduling.

Test singleton authority without shared mutable mod globals, explicit dependencies,
cycles/missing dependencies, startup rollback and critical/optional failure
propagation. Add actual SDK conformance checks and targeted documentation/examples.
Synthetic tests, source inspection and real-game validation remain distinct; these
requirements are not PASS merely because this document records them. Independent
audit and human acceptance of implementation remain required under project rules.
