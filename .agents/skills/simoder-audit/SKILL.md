---
name: simoder-audit
description: Independently audit actual changes, contracts, security and evidence.
---

# simoder-audit

Read audit-scopes.md, contracts.md and policy.md. Do not audit your own work or edit implementation. Freeze baseline and candidate including new/deleted files. Examine actual artifacts/tests, select all affected scopes. Return finding IDs, severity/location/evidence/scenario/confidence/disposition and scope PASS/FAIL/BLOCKED. Human acceptance separate.

All referenced documents are in docs/ai at this package repository root. Locate that root from this skill. Read [entry point](../../../docs/ai/README.md) only for routing. This skill never grants new permissions.

Use one final stage audit/finding registry. Review substantive fix diffs, results and affected integration; reuse unchanged evidence. One competent nonauthor may cover all applicable scopes. Any required case failure keeps overall FAIL. Do not demand routine pre-audits merely to run the unchanged approved local procedure; pre-review new risk before effects.
