# Immutable selected-input candidate

Status: **U3f-k concrete isolated implementation contract; no production cutover.**
This implements the capture unit in
[MOWN-02/03/06](manager-owner-integration-contract.md). It neither changes
ModManager nor grants script, agent or native execution permission.

## CAP-01 — Exact meaning and API

A native capture selects a bounded canonical input inventory, retains all selected
file handles simultaneously before reading any file bytes, then parses and
compiles only owned bytes. Those selected bytes coexist when the last file lease
is acquired. Input identity is checked against enumeration and final rescan;
observed inventory drift rejects. Directory/ancestor handles prevent root/path
replacement while retained. This is **not an atomic exhaustive directory tree
snapshot**. Transient additions/removals of unselected files are not claimed
detected. They cannot affect the result: its compiler, module resolver and future
VM use exactly the retained inventory, never reopen original paths or mutable cache.

The new native-only immutable CapturedCandidate returns const definition, exact
source inventory, copied Base pin and deterministic content digest. A separately
created opaque native InstallationBinding may be reused across captures of the
same modeled installation. Equal content can have different bindings; the same
binding can have changed content. A fresh capture instance is not itself proof of
installation identity. Bindings are process-local test/plumbing identities, not
authenticated persistent install records, user decisions or public script handles.
The actual manager binding/reinstall protocol remains a later gate.

Capture is disconnected from production consumers. Construction is complete or
fails without partial candidate output. No caller-supplied mod ID, digest or cache
locator can replace the actual captured definition or mint approval. Return bounded
reason codes; do not expose parser source excerpts, private absolute paths or tokens.

## CAP-02 — Supported input and bounds

Select mod.toml plus local scripts/**/*.luau. Non-input authoring material such as
README and .simoder caches is ignored and cannot contribute runtime data or trust.
Ambiguous retired runtime manifests are rejected. Unsupported files in the scripts
inventory are rejected. No cache descriptor or compiled cache output is consumed.

Only current schemaVersion2/format simoder.authoring gameplay declarative patches
and optional Luau scripts are eligible. Strict recognized root and nested field
sets include metadata, scripts, patches and symbols. Reject unknown security-bearing
fields and unsupported dependencies, native/modules-as-DLL, services, raw resources,
TypeScript or other contract/version/kind. Preserve existing validated alias/patch
semantics through shared pure byte-input parser helpers; legacy filesystem wrappers
delegate to those helpers and retain their behavior. Do not add another parser or
compiler implementation. No new manifest dependency format is introduced.

Limits are initial closed-route limits, not new limits on existing legacy loaders:

| Input | Maximum |
| --- | --- |
| Selected files including manifest | 257 |
| Selected directories | 64 |
| Portable relative name/depth | 240 ASCII lowercase characters / 16 components |
| Manifest | 1 MiB |
| Each Luau source | 1 MiB |
| All Luau sources | 8 MiB |
| All selected source bytes including manifest | 9 MiB |
| Compiled bytecode total including entry and modules | 16 MiB |
| Symbol catalog | Existing 4096 entries per category, with cumulative bounded input |
| Declarative resource/property operations | 4096 resource definitions and 4096 total property operations |
| Base pin | Nonempty version up to128bytes;2–64 canonical files, including mod.toml and scripts/init.luau; each uppercase64hexSHA256 |

Reject traversal, streams, device/nonportable aliases, reparse points, unsupported
file types and multiply linked input files. Bound allocation and inventory traversal
before accumulating bytes/entries. Capture retains root/ancestor and selected
directory identities plus every selected plain file lease denying write/delete
sharing. Selected manifest/module write, rename, replacement and ABA cannot succeed
while leases remain held. No watcher, oplock, USN, VSS, administrator privilege or
new end-user filesystem service is used as a snapshot authority.

## CAP-03 — Content, Base and compilation provenance

Versioned domain-separated length-framed deterministic SHA256 covers canonical
sorted names and exact captured bytes, compiler interpretation/profile, compiled
entry/module output and complete copied BasePackagePin version/file inventory.
Never concatenate ambiguous strings or use an installation token/path in the
content digest. Derive the VM ScriptDefinition from captured source and shared
CompileLuauSource output; retain modules, aliases and execution limits. Once
constructed, caller mutations and later disk changes cannot alter the candidate.

The Base pin is copied and structurally validated, not authenticated by this API.
Its full inventory participates in identity; a manifest-only revision is insufficient.
This unit does not alter or qualify actual Base bootstrap filesystem capture or
bind the candidate to an actual BaseHost. That integration remains later work.
Compiler profile denotes the current integrated implementation/options and bytecode
interpretation, not trust in an external compiler. No external tool is invoked and
no TypeScript compiler, executable, service or other dependency is added.

No digest authenticates publisher, source server, human consent or instructions.
Captured documentation remains data. No authority, persistence ledger, native DLL
trust, dependency closure or restart approval is inferred from content equality.

## CAP-04 — Required isolated qualification

Tests exercise exact captured manifest/modules/aliases/limits and declarative
behavior through existing parsers/compiler and an isolated VM, with no game process.
Require deterministic identity independent of enumeration/installation path; equal
content with distinct install binding and changed content with same binding; full
Base pin and compiler/output interpretation in digest; no mutable-cache inheritance.

Typed macro-only fixture phases may inspect AfterInventory, AfterLeases and
AfterRead. No production callback hook is exposed. Test selected replacement before
lease acquisition, selected manifest/module write/rename/ABA attempts while held,
post-capture source mutation/deletion, inventory drift, file/directory/link/path
limits, unknown fields/contracts, unqualified input rejection and output bounds.
Retained bytes/output must remain unchanged or capture must reject; no silent
mixed-input parse or original-path reread. Transient unselected tree ABA is NOT
qualified and is not substituted for selected-input coherence.

Full existing host regressions and production/fixture symbol segregation remain
mandatory. Build/test success is not actual manager, native context, permission or
gameplay acceptance. Runtime code/configuration/active instructions outside capture,
shared byte-parser seams, build membership and isolated tests remain unchanged.
Evidence and independent exact-candidate audit belong in `.analysis/u3fk-2026-10-16/`.
VM, crash trials and PR #7 remain parked.

Filesystem qualification in this isolated unit is local NTFS on the tested Windows
host. The legacy 64-bit file-information identity used here does not establish
equivalent guarantees on ReFS, FAT or network filesystems. Other filesystems and
strict production rejection of unavailable/unreliable IDs are a later admission
gate; this API has no production consumer. No general Windows-filesystem or
cross-machine identity claim follows from these host tests.
