# Script lane and lifecycle

U3c gives the integrated DLL one private interpreter lane. All Lua allocation,
library setup, module loading, entry code, lifecycle calls and VM destruction run
there. Standalone native fixtures use the same lane service as `ModManager`.
Compiling immutable source generations remains outside the interpreter lane.

The lane is shared by its native hosts and VM owners. Its private registry has
128 fixed identity/control slots, reserved before VM allocation. Each VM has one
serialized caller-owned control record, including close; teardown does not need
a free queue entry or a new heap-allocated control record. Generations are
host-assigned, process-monotonic and reject exhaustion. They are diagnostic
identities, not capabilities or SDK handles. The existing 128 MiB aggregate and
2–64 MiB per-VM limits remain: at the minimum budget the memory admission bound
is reached at 64 simultaneous VMs, before the structural 128-slot bound.

Control visits rotate across slots, alternating with eligible script visits.
Each VM's automatic tick has a nominal 50 ms minimum interval; callback deadlines
and round-robin visits isolate a runaway script. Neither a hard 5 ms scheduler
cycle nor a hard frame-time guarantee is claimed. Native observation callbacks
must remain bounded; the Lua interrupt cannot preempt a blocking native function.

## Publication and retirement

`ModManager` reserves a nonblocking lifecycle transaction. Overlapping lifecycle
writers and synchronous mutations originating from the script lane fail with a
diagnostic. This reservation is not a held mutex: snapshots and existing active
script command transactions continue during preparation.

Enable and reload copy their candidate definition and registry revision under
the manager mutex, release that mutex, initialize the staged VM on the lane, bind
its copied host sinks, then reacquire the mutex. Stop or a changed registry
revision rejects the candidate before durable publication. The lifecycle writer
reservation prevents another refresh/enable/reload from replacing its record or
candidate while the mutex is released. Existing persistence, patch-registry and
runtime-scope publication checks remain authoritative.

Scheduler owner-map nodes are reserved before publication. Replacement retains
that node; publication swaps existing shared owners without allocating a new
node. Failed preparation releases only empty reservations. Declarative-only mods
do not consume interpreter owner slots.

Disable, reload replacement and missing-mod removal unschedule the old identity
and update native state before releasing the manager mutex. Only then do they
wait for `onDisable`, flush copied messages and destroy old VM owners. A staged
candidate discarded before commit receives no `onDisable`. A committed generation
receives at most one bounded `onDisable`; writes remain forbidden during it.
An old generation's commands are still checked against the current active VM
identity, so an in-flight retired callback cannot publish into its successor.

The lane takes a genuine shared VM owner for each dispatch. Scheduler shutdown
unschedules its owners, fences active dispatch destruction without holding
manager/scheduler locks, then stops and closes its retained VMs on the live lane.
Only the final off-lane owner release joins the lane. No process-lifetime worker,
static-destructor join or auxiliary reaper is introduced. Native scheduler
mutators reject calls from a lane sink before acquiring locks; the native host
must also destroy the scheduler off-lane. Core owns that lifetime and destroys
it after its action worker. These private native controls are not script APIs.

## Diagnostics and boundaries

Status readers copy a separately published native snapshot; they never wait for
the interpreter lane or a VM callback mutex. It carries host generation and a
saturating dropped-log count. The existing buffer keeps at most 128 messages of
at most 2048 bytes each. Native lifecycle observers receive only an authored phase
and host generation, and must not reenter runtime controls. Observer failures do
not become script privileges or alter Lua outcomes.

Live and published diagnostics retain at most the first 2048 bytes in fixed
storage. Capture and publication do not allocate; only an ordinary status reader
materializes a string copy. Retirement catches callback/reporting failures and
still closes the VM and returns its aggregate memory reservation. Truncated local
diagnostics are not complete crash reports or symbolized native stacks.

These observations support later Beta diagnostic correlation. They do not claim
that freeform script diagnostics are anonymous, capture a native crash stack,
intercept stderr or the frontend bridge, or select a dump/helper-process format.
Those remain separate diagnostic discovery and implementation units.

The lane and VM registry belong to DLL Core. High-level services, task semantics
and facades remain the responsibility of the future Luau `base`; canonical SDK
declarations and DevTools adapters are unchanged. U3c does not activate a native
provider, production task authority, `base` readiness, MCP or new game operations.

## Regression evidence

`simoder-script-scheduler-tests` instruments real create/require/entry/enable/
tick/disable/close execution, including failed initialization; exercises aggregate
admission/recovery and log overflow; checks fair visits and runaway isolation;
and races last-owner removal with shutdown and sink reentry.
`simoder-script-lifecycle-transaction-tests` checks snapshot reentry from sinks,
persistence-denied disable and exactly-once retirement. Existing script, PROP,
resource and authoring tests remain required regression oracles. Passing these
synthetic tests does not establish installed-game acceptance or suspended-task
resume behavior, which is not implemented in U3c.
