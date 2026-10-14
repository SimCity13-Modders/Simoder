# Ecosystem boundaries and acceptance targets

Version: 1.0. Existing policies and explicitly approved scope govern present changes. The product architecture proposal is not yet implementation authorization.

Runtime: one integrated x86 d3d9.dll beside SimCity.exe. Target Beta: high-level base in Luau; JS/TS language support; low-level DLL primitives; optional DevTools as mods, no game DevTools executable. OpenSCP and standalone workspace CLI may be executable programs. TS compilers are external separately installed tools; OpenSCP/MCP/runtime detect and use them only with applicable permission, never bundle/download them.

Fork OpenSCP: inspect upstream/local CONTRIBUTING, SECURITY, notices and coding/UI conventions; preserve author notices and upstream update path. MIT for new authored work does not relicense third-party code. Never export unchanged EA assets; references/deltas resolve locally. Legal-only exclusions must not excuse missing reference architecture.

Beta runtime acceptance requires separately tested Windows 10/11 and one verified game build/DLC set; Linux experimental, Linux/macOS tool verification cannot be inferred. RoadBuilder Luau plus BusLine TS, arbitrary valid roads, simulation, UI presets, cross-mod integration and save/reload are required and unproven until executed.

MCP local/authenticated/explicitly enabled; designated sandbox, ask for existing region or create after preparation approval. Unknown SP/MP context blocks mutation; MP needs separate scope. Preserve results by default; deletion explicit. Background semantic interaction must not steal focus/input. Computer Use only independent verification in autonomy run, never primary automation. Region-copy tests don't protect against already-authorized external effects.

Current private Gitea precedes GitHub migration; no push/publication from inferred approval. No LFS. No change to generated artifacts without authoritative source. Docs/index update with behavior and contracts.

