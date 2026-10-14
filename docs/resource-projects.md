# TOML raw-resource projects

Editable sources in the OpenSCP workspace now use `mod.toml` (schema 2,
`simoder.authoring`), named PROP files under `objects/`, localization TOML and
symbolic asset references. Their binary metadata is in `.simoder/resources.lock.toml`.
Mod Studio compiles these sources to the separate runtime contract below.
See `C:\Projects\Simoder-OpenSCP\AUTHORING.md`. The loader consumes compiled
native payloads; readable authoring TOML does not imply additional runtime support.

Installed schema 3 packages use `mod.toml`, `contract = "simoder.raw-resources"`, metadata,
declared capabilities and a `[[resources]]` array. Each resource stores its full
`TYPE:GROUP:64-bit-INSTANCE`, `operation` (`add` or `replace`), byte size,
lowercase SHA-256 and a relative payload path under `assets/`. Typed subdirectories
and extensions are supported; legacy `assets/<sha256>.bin` paths remain valid.
Traversal, alternate data streams, links and junctions are rejected. Named files
still require matching manifest sizes and hashes; this does not enable runtime
registration or import of FBX/GLB into the game's proprietary formats.
`changes.toml` and JSON reports explain provenance and differences; the original
binary resource remains authoritative for this contract.

The current OpenSCP raw export and Mod Studio build write `mod.toml`. Older
exporter output using `project.toml` must be migrated offline with the
DevTools resource migration command before installation. Runtime discovery does
not load that retired entry point or TOON property manifests. A current manifest
cannot coexist with retired manifests in the same mod directory. The compiler's
private immutable generation descriptor remains separate from installed discovery;
compiled resource payloads inside that generation also use `mod.toml`.

The loader validates schema, identities, capabilities, paths, sizes and payload
hashes, rejects junctions and links, and displays imported metadata. Imported raw
resources cannot be activated yet: the existing runtime only edits scalar float
values in resources already deserialized by the game. Discovery reports the
missing `resource.register/resource.replace` capabilities and applies no part of
the mod. Validation tools likewise report runtime incompatibility.

The next runtime work requires a verified resource-manager lookup/registration
boundary, ownership and lifetime of binary streams and decoded assets, dependency
resolution, load timing, and rollback/unload behavior. The current PROP deserializer
hook does not establish any of those contracts. No unverified game addresses are
introduced by the TOML importer.

Extracted assets and private local acceptance fixtures remain ignored by Git and
excluded from public CI. Public tests use synthetic redistributable resources.
