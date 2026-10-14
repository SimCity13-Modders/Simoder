# Game-build compatibility and reverse engineering

Runtime addresses and signatures are valid only for an explicitly verified game
build. The installed DLL validates executable/loaded-code identity before hook
installation. An unknown build must leave graphics forwarding available and
decline unsupported game-memory operations. Never infer compatibility from a
matching filename, package-editor support or a historical gameplay observation.

## Maintained boundaries

`src/reverse/game_build` and `build_compatibility` establish build identity;
`resource_symbols` resolves the supported exact-build resource boundary.
`hooks/resource_trace` validates resource table shape and property identity before
applying owned scalar changes. Declarative mod TGI/property IDs identify data;
they are not process addresses and must remain separate from symbol resolution.

Package extraction, dictionary research and offline binary comparisons belong
to Simoder-DevTools. A valid exported resource or package does not establish
runtime registration, decoded-object ownership or safe unload behavior.
Raw-resource activation remains gated by implemented capabilities; see
[resource contracts](resource-projects.md).

## Investigation records

Put audits, experiments, screenshots, logs and per-build investigation narratives
under ignored `.analysis/`. Preserve exact executable hashes, observation times
and confirmed versus inferred conclusions there. Historical attach/detach and
injection results do not prove the current integrated DLL installation.

Update maintained build profiles only with reproducible identity, signature and
runtime evidence. Run synthetic checks and separately perform
[live-game acceptance](first-test.md) for each changed supported boundary.
Do not ship proprietary executable or package payloads as public fixtures.
