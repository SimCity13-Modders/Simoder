# TOML raw-resource projects

Schema 3 uses `project.toml`, `contract = "simoder.raw-resources"`, metadata,
declared capabilities and a `[[resources]]` array. Each resource stores its full
`TYPE:GROUP:64-bit-INSTANCE`, `operation` (`add` or `replace`), byte size,
lowercase SHA-256 and content-addressed `assets/<sha256>.bin` payload.
`changes.toml` and JSON reports explain provenance and differences; the original
binary resource remains authoritative for this contract.

The converter's CLI `export-toml` and desktop TOML export produce this contract.
Existing TOON property mods retain their current contract. A directory cannot
contain both `project.toml` and `mod.toon`.

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

Project Agua is a private local acceptance fixture under `mods/ProjectAgua`,
containing six losslessly preserved resources. It and extracted assets remain
ignored by Git and excluded from public CI.
