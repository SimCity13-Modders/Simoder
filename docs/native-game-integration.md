# Native game integration contract

> **Historical planning document.** This contract predates the Rust Alpha
> runtime. It records a target end-to-end workflow, not implemented or
> verified behavior. See the [current runtime overview](../README.md) for the
> active implementation and its verification limits.

MCP developer services remain optional, game-owned service mods. Production
installation remains one integrated x86 `d3d9.dll`. Game exit ends all service
descendants; an external stdio client must not own the runtime service lifetime.

## Required end-to-end workflow

The target is an autonomous runtime validation session: launch through EA App,
wait for user-approved UAC, enter the game, create a dedicated sandbox region,
claim and load a city, construct roads and buildings, demolish test entities,
query simulation data, validate a mod, and exit normally. Ordinary user saves
must not be selected implicitly as test fixtures.

Prefer semantic native commands over desktop input. A command carries its game
instance identity, typed parameters, request identity and deadline. Execute
mutations on the verified owning game thread, not the MCP, diagnostics or render
thread. A queued command is not a completed operation. Report its correlated
completion, resulting object identity and authoritative postcondition.

Native adapters require an exact supported executable fingerprint, verified
calling convention, object lifetime and thread affinity. A decompiled class name
or subsystem identifier alone is insufficient evidence to call a function.
Unsupported adapters must remain unavailable rather than invoking guessed RVAs.

Sandbox test ownership must include the region/save identity and entity handles.
Construction and demolition must reject stale entities and invalid coordinates,
and return engine validation failures. Queries must identify their observation
time and city generation. Historical PROP resource observations are not a live
simulation query or proof of gameplay effects.

## Current capability discovery

`simoder_get_capabilities` is available in both bootstrap and hosted MCP modes.
It reports supported transports, foreground requirements, unresolved native
operations and loader readiness from a fresh, consistent snapshot bound to the
owning game PID. Support describes implementation; callers must still observe
operation results and current readiness.

Implemented integration includes EA launch, owner-scoped mod enable/disable,
loader snapshots, normal window-close requests and verified window capture.
Developer memory analysis adds owning-game module discovery, complete bounded
reads and stable executable-section export for REA, without window focus or
memory writes. This enables ABI investigation; it is not a simulation adapter.
Native session entry, region/template observations, Sandbox creation, city claim
and loading are implemented through a fixed embedded router calling the game's
Client bindings. A private same-process window message wakes the DLL-owned queue;
it carries no input coordinates or caller pointers. Each execution checks the
owning window thread against the game's native duplicated thread handle and
admits only the measured executable fingerprint and entry signatures. No window
activation or click simulation is used.

The native adapter owns local JS function/value scopes, observes the actual view
inventory, copies UTF-16 results before releasing values, and exposes no arbitrary
script execution. Developer-service revocation, stale PID, expired requests and
unsupported ABI prevent execution. Menu/city tools distinguish accepted commands
from completion; city loading checks finalization and all expected save identities.
Region/city mutation is restricted to new or unclaimed Sandbox saves, with partial
identities retained on errors and no implicit mutation retry.

Installed-game acceptance passed on 2026-10-13 for the supported executable:
native updater-to-menu transition, creation of Sandbox region 6 using TwinCities
template 246, claim of city 34 (UID 1026), and finalized loading of that exact
city and region. The final menu/load tools returned `completed: true`; window
capture independently reported `isForeground: false`. Reclaiming the city was
rejected before mutation. Transient unavailable views during scene replacement
allow retries of read-only observations only; mutation commands are never replayed.
The embedded router uses ES5 function expressions because the game's older engine
rejects strict-mode function declarations inside statement blocks.

The frontend router distinguishes the intro movie from preload and menu readiness.
`simcity_skip_intro` calls the game's curtain handler, which stops the movie and
continues the configured preload callback. Entering the game skips a playing intro;
loading a city first waits for completed frontend preload. Stopping video playback
alone is insufficient because it does not advance the curtain lifecycle. These
transitions use neither window activation nor simulated input. The intro-specific
installed-game acceptance remains pending.

Read-only Sandbox world inspection exposes paged native unit references and exact
unit data. A reference contains both slot and generation stamp; stamp zero is valid.
Invalid sentinel references are excluded. Reads require the requested loaded city
and region, rather than implicitly selecting a user save.

Road construction, building
placement, demolition and general simulation queries remain unimplemented.
The separate desktop-click tool still requires a fresh capture and foreground
window and is not part of the native session workflow.

OpenSCP's subsystem descriptions are research inputs. They do not establish a
callable ABI in the installed game. Resolve the session/UI dispatch boundary and
game-thread command queue first, then region/city lifetime, entity construction
and simulation queries. Each adapter requires an actual game acceptance test
before capability discovery reports support.
