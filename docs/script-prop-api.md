# PROP scripting API

The [proposed production adapter contract](architecture/prop-adapter-contract.md)
describes the U3f migration gates. It does not change these current bindings or
enable a new permission provider or SDK facet.

`simoder.props` supports the existing verified scalar float32 PROP path. It does
not register new resources or replace resource streams. Scripts can add absolute
overrides for existing fields, clear their own overrides and read copied historical
evidence. `simoder.hasCapability("game.props.float")` reports whether the host is
bound to the loader's patch registry/cache. The standalone compiler check has no
game host and reports this capability as false.

```luau
-- @summary Apply one mod-owned override after successful initialization.
return {
    onEnable = function()
        simoder.props.setFloat("sanitizer", "operatingCost", 345)
    end,
}
```

Names resolve exclusively from the project's `mod.toml` symbol catalog, using the
existing provenance-checked resolver. Catalog changes participate in source
fingerprints and reload validation. Duplicate/missing-source catalog entries fail;
non-float declarations are not available to this API. No name is guessed or hashed.
See `examples/PropApiExample` in Simoder-DevTools for a complete project. `operatingCost` is a Simoder
alias backed by the verified cost effect, not a claim about EA's original field name.

Canonical identifiers also work: `"00B1B104:61EFC000:719436BD"` and `"0x09AE19D7"`
or numeric `0x09AE19D7`. The 16-digit instance spelling used by authoring projects
is accepted when its high 32 bits are zero. Nonzero high bits fail without truncation;
the verified game PROP prefix currently supports 32-bit instances.

## Observations

`getFloat(resource, property)` returns nil before the loader has observed that
target. It returns a read-only table with `value`, `vanilla`, `generation`,
`observedAtUnixMs`, `instances`, `status` and `historical = true` afterward.
This reads retained snapshots, not current game memory. Missing/non-float fields,
non-finite data and disagreement among observed instances raise an error. There
are no raw pointers, native instance handles or implicit resource loading.

The production hook currently retains resources targeted by patches, and later
observations of those retained instances. An override for a target not yet retained
applies on its next successful game deserialization. API calls do not force the
game to reload a resource. Already copied simulation/UI values can remain stale.

## Transactions, ownership and timing

`setFloat` and `clearFloat` enqueue requests; they do not synchronously change game
memory. Successful initialization publishes its requests with the mod's activation
or reload. Successful ticks submit one batch outside the VM lock. A callback error,
quota violation, stale VM or failed preflight discards the entire new batch.
Reading immediately after `setFloat` therefore returns the earlier observation.

Each script owns at most 256 overridden fields and can queue at most 256 commands
per callback. Values must fit finite float32. Repeated writes of the same absolute
value do not generate registry updates. Commands preserve activation order and
participate in the existing conflict report; later owners take precedence.

Clearing removes only that mod's scripted field override, retaining its declarative
operations and other mods' contributions. Disabling/removing a mod removes all its
scripted overrides. Successful reload replaces them with the new VM's initialization
requests; failed initialization leaves the old generation and overrides intact.
Changes are ephemeral and are not written into source files or persistent state.
Initialization recreates them after restart. Writes in `onDisable` are rejected:
the loader restores resources by removing the owner's complete patch set.

Known retained tables are preflighted against vanilla and the complete ordered
patch set. Native writes reuse the existing validated compare/write/rollback path.
A committed registry update can still fail to refresh a changed native instance;
the existing UI/runtime diagnostics report that separately. A rejected tick batch
stops that script with a diagnostic and retains its previous committed overrides
until disable or a successful reload.

## TypeScript

The shipped declarations expose `SimoderProps` and `SimoderFloatObservation`,
including receiver-free calls required by TypeScriptToLua:

```typescript
/** @summary Stage a verified named override using the same host contract as Luau. */
export function onEnable(): void {
    if (simoder.hasCapability("game.props.float")) {
        simoder.props.setFloat("sanitizer", "operatingCost", 345);
    }
}
```

Tests execute the actual x86 VM/compiler, scheduler, registry and cache with
synthetic owned tables. They cover named bindings, TypeScript receiver semantics,
failed callbacks/reloads, restoration, repeated writes, conflicts, 64-bit rejection
and ambiguous observations. An injected-game test remains separate from this
synthetic acceptance. Native models, textures, JavaScript and structural PROP edits
still need verified resource-manager integration.
