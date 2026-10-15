# MCP DevTool architecture

Status: architecture accepted by the user on 2026-10-15. Implementation and runtime acceptance remain separate. Decision record: [ADR 0001](decisions/0001-mcp-native-devtool.md).

## Ownership

The production Simoder core remains one integrated x86 `d3d9.dll`. MCP is an optional native DevTool mod DLL, installed and explicitly trusted separately; it is not a mandatory companion to the core. It owns the MCP protocol, transport and tool registry. No MCP/DevTools executable or always-running bootstrap server belongs in the game payload.

Simoder supplies generic low-level lifecycle, resource ownership, grants and validated game-operation primitives. `base` remains entirely Luau and supplies public, project-authored facades, lifecycle events and service management. Facades such as `GameManager` describe our abstraction, not published EA source or an assertion that such a Lua class exists in SimCity. No EA code or assets are required in this public API.

The accepted lifecycle design combines typed events with a managed service scope. Arbitrary `PreHook`/`PostHook` interception can later be a convenience API for verified methods; a quit callback alone cannot own cleanup. Event names, declarations, ABI and version numbers are still proposed, not callable APIs. Existing script `onEnable`/`onTick`/`onDisable` callbacks do not establish this future service contract.

## Lifetime and singleton

The host reconciles current readiness and delivers sequence-numbered state to late subscribers. Readiness includes safe menu operation; a loaded city is not required for serving permitted menu tools. Admission requires a running game, compatible and enabled base/DevTool, native execution trust, runtime readiness and explicit agent-access opt-in. Authentication and applicable grants are additionally required before catalog or tool access. The initial pairing surface must not provide anonymous tools.

One managed Simoder runtime per machine owns one MCP service generation across installations and Windows sessions. EA App's normal launch restriction is helpful but is not proof of this guard. A secured, version-independent OS owner guard and an in-process state guard must reject duplicate ownership without killing another process or escalating privileges. Exact Windows naming, DACL and owner verification need implementation/security tests. A mutex is not client authentication.

Multiple authorized clients share that one endpoint. Client identity, protocol session and user-selected agent-session lifetime are distinct. ModStudio, HotReload and MCP share authoritative mutation serialization, state revisions and explicit conflict resolution. Epoch, grants and SP/MP/sandbox context are revalidated immediately before commit.

Disable, revocation, reload and orderly quit revoke admission and stale handles, cancel uncommitted work, close the listener and drain safe work before releasing dependencies. Native ownership enforces cleanup even if Luau callbacks fail. No socket thread invokes the VM or game ABI directly. A drain timeout cannot free dependencies still in use or forcibly terminate committed game work: report an indeterminate/faulted outcome and require safe recovery. DllMain must not run service startup or blocking teardown. Process exit closes in-process handles; no external watchdog is required.

No running game means no callable MCP, including launch/diagnostic bootstrap tools. A stale client tool listing is not a server. During migration the old offline bootstrap is a known noncompliant path, not an exception. Reload stages a candidate, quiesces the old owner and replaces its generation without two active listeners. Failed restart must not resurrect old grants.

## Extensible tool registry and trust

Mods may register namespaced tools through a versioned API. Every registration binds host-verified mod identity, generation, bounded schema and required capabilities; duplicate names are rejected. Unload/reload removes owned entries and invalidates stale invocations. Tool registration grants no authority. Request and commit both check authenticated-client grants, owner generation, runtime epoch and operation-specific constraints. Synthetic tool calls cannot approve their own permissions or audit results.

Native mods execute outside the script sandbox. Explicit native trust and publisher/content policy apply; these API controls cannot isolate the process from a malicious native DLL. Tool descriptions and results remain untrusted data. Secrets must not be returned to scripts or ordinary tools; an absolute secrecy guarantee against arbitrary in-process native code is impossible. Full memory dumps retain separate explicit consent.

## Network profiles

The user can configure the port and exact bind address. Default is loopback; network binding is explicit opt-in. Accepting this architecture does not enable a listener, change client configuration or open a firewall now.

| Bind address | Required policy |
| --- | --- |
| `127.0.0.1`, `::1` | Authentication and grants mandatory; plaintext HTTP may be considered only under the validated local authentication profile |
| Specific LAN/VPN/non-loopback address | Explicit network opt-in, TLS and authentication mandatory |
| `0.0.0.0`, `::` | Same requirements, with a clear warning that all interfaces of that address family are included |

The non-loopback profile explicitly extends the earlier local-only Beta architecture. It remains disabled by default and needs separate security/runtime evidence. No automatic firewall rule, UAC, privileged bind, wildcard fallback or silent port change is permitted. Port collision fails with a useful diagnostic. IPv4/IPv6 and dual-stack behavior must be tested separately.

Loopback is not a trusted client identity. Validate Host and Origin as applicable, reject DNS rebinding, use exact allowlists and bounded request/connection/parser/response budgets. TLS requires a verified certificate/hostname trust path; do not disable certificate validation to make a client connect. Certificate provisioning and renewal are unresolved implementation gates, not an approved new dependency or permission expansion.

Direct Streamable HTTP is the chosen transport direction. MCP revision `2025-06-18` is the initial compatibility candidate, not a claim about the latest specification. Exact Codex compatibility, OAuth profile, secure pairing and certificate provisioning are **NOT_RUN**. OAuth HTTPS endpoint requirements cannot be waived merely because a listener is loopback; localhost redirect exceptions do not exempt an entire authorization server. Select and independently audit the exact interoperable profile before listener deployment. Credentials, audience, client identity, epochs and revocation must remain bound correctly; persisted login is not a current game grant.

## Migration and acceptance

1. Freeze lifecycle/native-mod ABI, service scopes, registry schemas and auth/network profile; establish real readiness/dispatch sites through source and REA evidence. Prototype direct-client interoperability in controlled fixtures and measure x86 payload size, dependencies, memory and latency. No speculative road/resource ABI calls.
2. Implement generic core ownership, singleton, epochs, grants and bounded dispatch. Audit failure/teardown before game deployment. Do not move MCP protocol/catalog into the permanent core.
3. Implement public Luau base service/event abstractions in an established task-owned repository/unit. Define late subscription, ordering, cancellation and compatibility without EA source.
4. Implement the optional native MCP DevTool and extensible owner-scoped registry. Inventory every legacy tool and schema, marking parity-tested, blocked or explicitly retired. New tools cannot imply unverified capability.
5. After audits and acceptance, migrate packaging/install/client registration; remove legacy MCP/broker executable and offline bootstrap routes from the shipped payload. Preserve separate external authoring tooling. Never run legacy and replacement hosts simultaneously.
6. Validate the exact installed build in the selected SinglePlayer sandbox, preserving backups. Verify menu/city transitions, background operation, shutdown/restart and tool parity. Region restoration remains unproven until separate catalog-closure, quiescence, transaction and durable reload tests pass.

Each L2/L3 unit requires independent audit and explicit human acceptance; architecture approval is not implementation/runtime acceptance. Publication, pushes, additional dependencies, privileges and unrelated PR work are not authorized by this document.

Required negative tests include: game absent; disabled mod or missing base; denied native trust/agent opt-in; no anonymous catalog; duplicate startup and second installation/session; port collision; invalid TLS certificate; non-loopback without TLS; wrong Origin/Host; malformed/oversized/slow requests; expired/wrong-audience/previous-epoch tokens; grant revocation while queued; stale tool owner; conflicting clients; VM failure; disable/reload; abrupt game crash; disconnect after commit without automatic replay. All tests of this proposed architecture are **NOT_RUN** until recorded evidence establishes otherwise.

## References and current baseline

Current legacy implementation: [service manifest parser](../../src/dev/service_manifest.cpp), [service supervisor](../../src/dev/service_supervisor.cpp), [script runtime](../../src/scripting/script_runtime.cpp). These establish executable service ownership and bounded script callbacks, not the new native-mod or HTTP contracts. Existing architecture descriptions of executable services describe the migration baseline.

Primary compatibility references: [MCP transport profile](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports), [MCP authorization profile](https://modelcontextprotocol.io/specification/2025-06-18/basic/authorization), [Codex MCP configuration](https://learn.chatgpt.com/docs/extend/mcp?surface=cli), [Windows kernel object namespaces](https://learn.microsoft.com/en-us/windows/win32/termserv/kernel-object-namespaces). These documents do not establish implementation or runtime compatibility.
