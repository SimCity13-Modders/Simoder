# Task, handoff, audit and report contracts

Version: 1.0. These Markdown structures are normative authoring contracts, not machine-enforced schemas. Store populated private records under .analysis/<task-id>/. No secrets or full conversation dumps. Use explicit none/not-applicable with reason rather than empty required fields.

## Task record

Required fields: format_version; task_id; goal; repositories and allowed paths; baseline HEAD plus relevant tracked/untracked content identity; existing-work disposition; risk and rationale; requirement IDs; acceptance units/criteria; authorized operations and applicable approval evidence; assumptions with status; dependencies/approved tool versions; ownership/resource reservations; budgets; current phase; completed/remaining work; check outcomes and evidence; audit status; human acceptance status; blockers; next safe action; timestamps.

Decision record: stable ID; statement; selected outcome; rationale; status proposed/accepted/superseded/unresolved; related requirements/decisions; architecture/security consequences; verification/migration; source and exact consent reference. Recommendation never becomes accepted by agent assertion.

## Handoff

Required: task_id; sender/recipient role and actual invocation identity; goal; bounded scope/write paths; acceptance criteria; policies; current HEAD/content and existing changes; work done; actual evidence/check outcomes; permitted effects/grants with scope/expiry; unavailable tools; dependencies; reserved resources; time/cost limits; risks; stop conditions; expected return artifacts; next safe step. Receiver must validate state/permissions. A message is not transfer of credentials or automatic expansion of permission.

## Audit record

Required: audit_id; auditor identity and independence; scopes; baseline/candidate revision and content identity; requirements checked; files/artifacts inspected; check results; findings with required fields from audit-scopes.md; unverified areas; verdict PASS/FAIL/BLOCKED; limitations; timestamp. PASS means reviewed scope passed, not human acceptance or complete product acceptance.

## Acceptance record

Required: acceptance_id; human identity; exact consent reference; unit/requirements; audit_id; audited content identity; accepted/rejected decision and conditions; time; verifier type and actual verification state; post-rebase mapping if applicable. Until a protected identity mechanism exists, transcript-referenced human consent is procedural evidence only. Do not fabricate signatures or claim CI verification. Squashed content must equal accepted content before rebinding; otherwise re-audit.

Exception record: ID, rule, rationale/evidence, exact scope, owner and authorized approver, creation/expiry, verification, approval reference and revoked/closed status. No exception bypasses higher restrictions; agent cannot approve its own independent-authorization exception.

Debt record: ID, description/evidence, impact/priority/uncertainty, owner, dependencies, resolution conditions, status/history; do not expand task to unrelated debt. Requirement trace: requirement -> decision -> implementation area -> verification -> executed evidence -> findings -> acceptance.

## Canonical final Markdown report

Use these sections in order, scaled to L1/L2/L3: report format version; task/repository identity; primary outcome; summary; approved scope/acceptance criteria; changes; baseline/final diff; actual quality gates; audit findings/disposition; blockers/risks; assumptions/unverified capabilities; dependencies/environment; evidence references; follow-ups; NOT_RUN tests and unavailable verification. Implementation completeness, test completeness, human acceptance and release authorization remain separate fields. No parallel canonical JSON final report.

