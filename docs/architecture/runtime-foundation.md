# Internal runtime foundation (U1)

U1 implements internal ownership and dispatch primitives. It does not deploy the
native-mod SDK, a production authentication provider, Luau base, or a new MCP
transport. Existing executable services and their authorization remain the migration
baseline. No new game operation is admitted through these primitives.

The [script lane and lifecycle](script-scheduler.md) guide describes the U3c
interpreter ownership and staged ModManager migration layered on this foundation.
The [private copied-value codec](copied-values.md) remains a separate Core primitive.
The [verified Base host](base-host.md) adds U3d source integrity and isolated
bootstrap execution without publishing production service readiness.

## Boundaries

`RuntimeOwner` acquires a fresh, secured `Global\Simoder.Runtime.Owner` object after
the exact game-build gate and before preparing mods. An existing object, access
denial, wrong object type, or creation failure denies ownership. The DACL permits
the current user and SYSTEM. The host retains the handle until process exit; there
is no takeover, waiting, or privilege escalation. Unique TEST objects exercise the
Win32 implementation without acquiring the production name. Cross-user/session
deployment qualification is still required.

`RuntimeScope` retains one generation's resources, epochs, operations, and results.
Preparation grants no operation admission. ModManager commits a scope only after
its registry/persistence transaction; reload prepares a replacement generation.
An absent production authority provider always denies generic operation requests.
Synthetic authority construction is compiled into an independent test library,
never `sc13_core` or `d3d9.dll`, including builds with tests enabled.

The core and all mod scopes share one `RuntimeCommitDomain`: one serial commit
lane, monotonically increasing revision, and at most eight queued/running generic
operations. Scope bounds include 128 retained operation handles, 64 retained
resources, and 1 MiB input/result ceilings. Identity, epoch, issued context,
grant, readiness, deadline, and revision are checked at commit. A stale revision
conflicts; it is never silently retried. Polling preserves the original operation's
outcome across subsequent revisions. Legacy registry writers retain their existing
manager/registry serialization; they have not been ported into an authenticated
native operation bridge.

Activation-state publication reuses the existing compiled-generation backoff
policy: only Win32 access/sharing errors (5/32), at most 11 attempts within a
500 ms backoff budget. Every attempt uses the same atomic replacement, path and
permissions. Persistent denial aborts the transition and preserves the previous
state and active patches. Compiled-generation identity/reparse checks still run
on every attempt; there is no alternate write or permission fallback.

`RequestStop` closes admission and cancels queued work without waiting for a
callback. A committed callback retains its resources, reports its real completion,
and cannot be replayed. Drain timeout preserves ownership. ModManager stops
accepting mutations before collecting generations to drain outside its lock.
The shared domain's window-stop gate immediately denies child admission; it does
not advance each child's local epoch or clear its buffers. Explicit manager
shutdown subsequently stops and drains those scopes. `ScopeSnapshot.stopping`
describes local scope stop, not the shared domain's admission status.

`RuntimeLifecycle` orders explicit state snapshots and atomic subscribe-current
delivery. Callbacks run outside the state lock. Runtime preparation, menu, and city
are separate facts: D3D/window observation does not establish menu/city readiness.
U1 publishes runtime preparation only; it invents no EA lifecycle hook.
Delivery is serialized on the publisher/subscriber thread that wins dispatch,
not on a dedicated game or VM thread. Later base callbacks must be marshaled to
their verified execution thread before using this generic notification primitive.

## Window dispatch and shutdown

The core `game_window_dispatch` owns verified current-process/window-thread HWND
identity, generations, and the original WNDPROC chain. Existing session transport
uses this router independently of ImGui initialization. Graphics observation binds
the existing window even when UI initialization fails. Dispatch rejects stale
window generations and preserves existing thread, permission, and queue checks.
It introduces no focus or input mechanism.

Owned close/destroy messages conservatively revoke admission before forwarding to
the original procedure, even when that procedure vetoes closing. Bootstrap
initialization/shutdown ownership mutations use a nonblocking serialization gate.
Once stopped, this runtime requires process restart; old grants are never reused.

Shutdown closes session, router, overlay, and generic scope admission, checks their
drains, then disables hooks. It retains dependencies and returns `ERROR_BUSY` when
safe teardown cannot complete. Hook code and original trampolines remain retained
for the process lifetime: a zero active count cannot prove that a thread paused
before detour entry has disappeared. Late retired entries only forward through
retained originals. This is not native DLL unloading. No blocking service work
belongs in DllMain.

## Qualification limits

Synthetic x86 ownership, concurrency, window, and regression tests qualify their
specific fixture paths. They do not establish actual SimCity shutdown, multiplayer
restoration, game-result parity, client authentication, or cross-session ownership.
Native module loading/search/ABI enforcement is the next U2 unit. Base, transport,
credential provisioning, packaging migration, and installed sandbox qualification
remain later gated units. See [MCP migration](mcp-devtool.md).
