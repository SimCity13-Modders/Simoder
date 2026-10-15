# ADR 0001: Native MCP DevTool with base-managed lifecycle

Date: 2026-10-15. Status: accepted architecture by explicit user decision. Implementation and runtime acceptance are pending. Scope: Simoder, public Luau base API and optional DevTools MCP mod; upstream OpenSCP PR #7 remains parked.

## Context

Legacy developer services use child executables and can expose offline bootstrap tools. The target distribution requires a single integrated x86 core `d3d9.dll`, no game-installed DevTools executable and no MCP when the game is absent. The user accepted a native MCP DLL mod and requested extension tools from other mods, public non-EA abstractions, configurable port/bind address and mandatory protection for network access.

## Decision

Use an optional, separately trusted native MCP DevTool DLL. Keep protocol, transport and tool registry in that mod; core exposes generic low-level primitives and mandatory ownership controls. Keep base entirely Luau with project-authored public facades, typed lifecycle state/events and managed service reconciliation. Cleanup must remain native-enforced when callbacks fail.

Provide one machine-wide managed runtime owner and one MCP host with multiple independently authorized clients. Serialize mutations with ModStudio and HotReload, rejecting stale epochs/revisions. Bind mod-provided tool registrations to namespace, owner, generation and required grants. Native code remains outside the script sandbox.

Default to authenticated loopback. Expose explicit port/address configuration; any non-loopback binding, including wildcard IPv4/IPv6, requires explicit opt-in, TLS with certificate validation and authentication. This extends the older local-only Beta profile without enabling access now. No automatic firewall/UAC changes. Exact OAuth/TLS/client interoperability remains a measured, audited implementation gate.

See the [complete contract and migration stages](../mcp-devtool.md) for lifecycle, trust, tests and limits. API names and schemas are design work, not claims of implemented functions.

## Alternatives and consequences

| Alternative | Assessment |
| --- | --- |
| Arbitrary method hooks alone | Useful later for verified method extensions; insufficient for crash cleanup, late enable and ownership |
| Typed events alone | Stable public abstraction, but needs a native-owned service scope and reconciliation |
| Luau MCP protocol with generic core transport | Avoids a native DevTool but grows core transport/parser requirements; not selected for this migration |
| Always-on core MCP or external bootstrap/broker | Conflicts with optional mod ownership, game-only availability or executable-free game payload |

The selected design keeps MCP footprint out of the core and supplies a representative native-mod Beta test. It increases x86 ABI, parser/network, DLL lifecycle and native publisher-trust testing. Native faults share the game process; no sandbox guarantee applies. Secure singleton implementation, exact lifecycle safe points, size/performance deltas, registry ABI, transport/auth compatibility and certificate provisioning still require evidence. Architecture acceptance permits scoped P0 realization, not speculative native operations, new privileges, deployment acceptance or publication.
