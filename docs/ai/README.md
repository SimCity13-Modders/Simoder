# AI engineering operating standard

Version: 1.0. Scope: Simoder ecosystem. This documentation package is authorized independently of the proposed product architecture. ARCH-001 and its implementation phases remain unapproved. English is canonical; communicate with this user in Polish.

Start with [policy](policy.md), then load only the relevant skill. Skills are in [the project skill directory](../../.agents/skills/). Custom Codex roles are in [the project agent directory](../../.codex/agents/). These files do not grant OS permissions, register external services or implement the future CI acceptance verifier.

| Task | Skill | Role |
| --- | --- | --- |
| Scope/risk/acceptance and orchestration | simoder-engineering | simoder-coordinator |
| Evidence-based discovery | simoder-discovery | simoder-explorer |
| Native ABI/hook uncertainty | simoder-reverse-engineering | Existing REA when verified, otherwise simoder-researcher |
| Authorized bounded implementation | simoder-implementation | simoder-worker |
| Independent review | simoder-audit | simoder-auditor and simoder-security-auditor |
| Game acceptance through MCP | simoder-runtime-validation | simoder-runtime-tester |
| Maintain targeted documentation | simoder-knowledge | simoder-documentation |
| Exact state transfer | simoder-handoff | coordinator/recipient |

Read [delegation](delegation.md) for existing specialists such as REA, [audit scopes](audit-scopes.md) for evidence obligations, [lifecycle](lifecycle.md) for gates and [contracts](contracts.md) for records. [Repository boundaries](repository-boundaries.md) prevent changing runtime/tool ownership accidentally. [Provider qualification](providers.md) distinguishes discoverable configuration from tested behavior. [Controls](controls.md) records what is currently enforceable.

Typical invocation: `Use $simoder-engineering for this task; delegate native ABI discovery to an available REA specialist and request independent audit before human acceptance.` No model override is required.

Project-scoped discovery does not automatically install these files into sibling repositories. From a sibling, explicitly supply this directory and its own applicable policies. Future workspace composition can install compatible copies through SAFE_ADD after approval. Do not overwrite sibling instructions.

