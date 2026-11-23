# Declarative patches in mod.toml

Current authoring projects use `schemaVersion = 2`, `format = "simoder.authoring"`
and a `[metadata]` table. A patch-only project needs no compiler executable or
script entry point. The same patch list can accompany a gameplay Luau script.

```toml
schemaVersion = 2
format = "simoder.authoring"

[metadata]
id = "example.scalar-patch"
name = "Scalar patch"
version = "1.0.0"

[[patches]]
target = { type = 0x00B1B104, group = 0x61EFC000, instance = 0x719436BD }

[[patches.properties]]
id = 0x09AE19D7
type = "float"
operation = "set"
value = 345
```

This illustrates the verified Sanitizer operating-cost property. It does not
claim changes to throughput, copied UI values, power consumption or other fields.

Readable aliases are declared with `[[symbols.resources]]` and
`[[symbols.properties]]`. Each alias includes its canonical identity and a
nonempty `source` documenting the mapping. Property symbols can also include
`type`, `description`, `unit` and `origin`. Use `name` in the patch target or
property table to resolve an alias; if numeric identity is also provided, it
must match. Names do not resolve game addresses or create unsupported runtime
capabilities.

Supported scalar float operations are `set`, `add`, `subtract`, `multiply` and
`divide`. Duplicate operations on the same TGI/property, unknown names,
incompatible symbol types, non-finite values and invalid divide operations reject
the candidate. A rejected candidate does not replace an active generation.

Native `resources` entries remain subject to the runtime's separate capability
checks. A readable authoring manifest alone does not imply support for registering
or replacing native resources.
