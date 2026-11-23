# Audit scope catalog

Version: 1.1. An audit identifies scope/version, exact baseline, candidate content identity, actual diff including relevant new/deleted files, auditor identity, test evidence and limitations. Audit only paths within authorization. Don't install dependencies, edit code or run game mutations merely to audit. Request scoped evidence if safe reproduction is unavailable.

| Scope ID | Trigger | Mandatory examination |
| --- | --- | --- |
| AUD-CORRECTNESS | L2/L3 | Requirement coverage, execution paths, error/cancellation handling, regressions, consumers and evidence honesty. |
| AUD-SECURITY | Permissions, IPC/MCP, bootstrap/update, protected policy | Authentication/admission, revocation/leases, grant scope, native non-isolation, secrets, traversal/symlink/TOCTOU, subprocess/network paths, artifact/publisher trust, downgrade, validator self-protection. |
| AUD-CONTRACT | API/schema/format/ABI or repo split | Version semantics, unknown fields, compatibility, ownership, rollback/migrations, exact consumers and generated sources. |
| AUD-RUNTIME | Game behavior, UI, resources, scripts | Exact game identity; actual runtime assertions; build versus behavior; UI action/state/render consistency; background/minimized differences; SP/MP context; reload limits and failures. |
| AUD-DATA | Save/profile/conversion/update | Vanilla baseline/provenance, no unchanged EA asset export, complete category inventory, atomicity, quiesced backups, content availability for restore, migration validation on copy and external effects outside rollback. |
| AUD-DISTRIBUTION | Packaging/CI/release | Single x86 runtime DLL, optional DevTools mods; OpenSCP/CLI exceptions, no bundled TS compiler, no LFS/game data, immutable inputs, signed Beta manifests, actual CI enforcement not workflow presence. |
| AUD-KNOWLEDGE | Instructions/skills/agents/docs | Semantic consistency with approved requirements, truthful capabilities, source links, ownership, targeted discovery, no new authority/dependency grants, usable delegation and acceptance boundaries. |
| AUD-RUST-HYGIENE | Any Rust crate creation or refactor | Compliance with [definition of done](definition-of-done.md): crate-first reuse (`Simoder-OpenSCP` + crate catalog), DRY, contract-respecting YAGNI, KISS & balanced SOLID (no over-modularization or monoliths), `#![forbid(unsafe_code)]`/no runtime panics, Rustdoc, and dual `x86_64` / `i686-pc-windows-msvc` checks. Blocking items (`MUST`) vs non-blocking (`SHOULD`) under Stop-Line. |

Select all affected scopes; correctness always applies to L2/L3. Policy/authorization/validator changes always need AUD-SECURITY and independent validation. Severity: CRITICAL unsafe/security/data loss; HIGH blocks requirement or major regression; MEDIUM meaningful bounded defect; LOW minor nonblocking issue. Critical/high findings block acceptance; other disposition requires rationale consistent with criteria, not silent deletion.

Each finding: stable ID, scope, severity, path/line or artifact identity, evidence, plausible failure scenario/impact, confidence, remediation or justified rejection, status and verification. Distinguish no findings from unavailable evidence. Re-audit material fixes. Human accepts verdict; auditor cannot grant exceptions for own work. Review records use [contracts](contracts.md).


Audit cadence follows [lifecycle](lifecycle.md): one independent final audit at the approved LARGE stage boundary; one competent nonauthor may cover every affected scope. Pre-review new risk before effects, then audit actual implementation and results. Re-audit substantive fixes and affected integration, reusing unchanged evidence; informational report/manifest edits do not automatically create new audit rounds. This does not waive protected security/knowledge validation, independence or final human acceptance.
