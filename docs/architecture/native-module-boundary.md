# Native module host boundary

The current Rust workspace includes an `ABI v2` native module host in
`simoder-mods`, integrated with the runtime and exposed to the Base mod's native
mod approval UI. It validates x86 PE32 candidates, records trust decisions,
and manages a bounded cooperative lifecycle for loaded images.

This does not make native DLLs a sandbox: approved modules execute machine
code in the game process. The host's ABI and validation constrain how modules
are loaded and how they interact with Simoder; they cannot contain arbitrary
native code or guarantee safe behavior.

## Lifecycle and limits

The host uses Query, Prepare, Admit, Quiesce and Drain lifecycle exports. It
checks the descriptor and ABI versions, validates the image and its imports,
and tracks module ownership and callbacks. Production semantic/dispatch facets
that do not have a provider remain unavailable. Loaded images are retained
until process exit; successful Drain is not permission to call `FreeLibrary`.

Resource and lifecycle limits are enforced by the implementation. These are
admission ceilings and safety controls, not measured performance guarantees or
evidence of a qualified third-party module ecosystem. Read the Rust source and
tests for exact current rules.

## Verification boundary

Synthetic fixtures exercise image validation, ABI negotiation, ownership,
lifecycle and failure cases. They do not establish that arbitrary native mods
are safe, that every optional ABI facet is available, or that the complete
workflow has been qualified in a live game. The [runtime README](../../README.md)
and current CI define the active implementation; dated qualification records
should be read as snapshots of the source revisions they identify.
