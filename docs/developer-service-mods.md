# Developer service mods

## Validation and ownership boundaries

`dev/authoring_tool_manifest` validates the optional authoring-tool descriptors
without activating them. The shared capability predicate in `dev/authoring_tools`
keeps manifest validation and the lease registry aligned. Manifest validation
checks contained files and toolchain layout; registration separately checks its
absolute-path contract and owner/capability conflicts. A validated descriptor
does not grant a capability: the existing service supervisor owns the registration
lease and revokes it when its service fails or stops.

This separates existing validation from executable-service ownership. It does
not implement the accepted [native MCP DevTool architecture](architecture/mcp-devtool.md)
or add a new manifest kind. Existing service behavior remains the migration baseline.

Optional `[tools]` entries declare `compiler`, `resource-converter` or
`typescript-toolchain` as paths inside the owning package. Executable capabilities
must resolve to plain local EXE files; a TypeScript directory must contain
`build.mjs`. Registration occurs only after all declared services start successfully.
Disable and observed service exit revoke the owner's capabilities. Conflicting
owners are rejected transactionally. Compilation path resolution is a separate
integration step; declaration alone does not replace the current ToolPath caller.

A developer service mod is installed under the game's `mods/` directory and is
inactive until explicitly enabled. ModLoader owns its processes through a
kill-on-close Windows Job Object. Disabling the mod stops its process tree;
closing the game also ends its children. Gameplay Luau scripts cannot launch
processes.

The manifest uses the existing authoring envelope:

```toml
schemaVersion = 2
format = "simoder.authoring"
kind = "developer-services"
serviceApiVersion = 1

[metadata]
id = "simoder.devtools"
name = "Simoder Developer Tools"
version = "1.0.0"

[[services]]
id = "broker"
executable = "Broker/Simoder.DevBridge.exe"

[[services]]
id = "mcp"
executable = "MCP/Simoder.McpServer.exe"
arguments = ["--game-host"]
dependencies = ["broker"]
```

Executable paths must refer to plain local `.exe` files within the mod directory.
Dependencies refer to service IDs in the same manifest. Missing dependencies,
cycles, duplicate IDs and unsupported API versions reject the whole definition.
At most 16 services and 64 arguments or dependencies per service are accepted.
The loader supplies `--parent-pid` and `--game-root`; manifests cannot override
these arguments.

Dependency order controls process launch order, not application readiness.
Services must handle delayed IPC availability. Startup failure rolls back all
children already created. Services are never automatically restarted.
After editing a service mod, disable and enable it to apply the new definition;
gameplay hot reload does not replace running executable services.

A developer-service mod cannot also declare gameplay scripts or resources.
The service API runs outside the gameplay sandbox: enable only developer packages
whose executable code you intend to run.
