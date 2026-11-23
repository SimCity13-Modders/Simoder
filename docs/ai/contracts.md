# Stage, handoff, audit and report contracts

Version: 1.1. Procedural Markdown contracts, not machine-enforced schemas. Scale records to actual risk; populated private evidence belongs under .analysis/<stage>/. No secrets or full transcript dumps.

## Living STATUS and decisions

One STATUS/decisions record per approved LARGE stage (for example U4/U5): goal, scope/allowed paths, baseline and relevant dirty-work disposition, risk/criteria, authorization references, current phase, ownership, completed/remaining work, check results/evidence, blockers and next action. Include toolchain/profile/dependency identity and resource/budget limits where relevant. Use small labels as task/test IDs in this record; do not create mandatory microplans or acceptance dossiers per function/fix/commit.

Decisions retain stable IDs, statement/rationale, proposed/accepted/superseded/unresolved status, material consequences and actual approval reference. Requirements trace to implementation, executed evidence, findings and stage acceptance. Agent recommendations are not human decisions.

## Handoff

An ordinary delegate handoff is a message with goal, ownership/allowed paths, current evidence and next action. Add baseline, existing work, criteria, permitted effects, resources/budget, unavailable tools and stop conditions only as relevant to safe execution. No mandatory new file. Receiver verifies current state/permissions; a message transfers neither credentials nor authority. Durable resumption state stays in the stage STATUS.

## Final audit and finding registry

One independent final audit per LARGE stage records auditor identity/independence, applicable scopes, exact baseline/candidate content, actual source/diff/tests reviewed, findings, verdict PASS/FAIL/BLOCKED, limitations and unverified checks. One nonauthor may cover all competent scopes; additional specialists only when needed. Protected instruction changes require independent AUD-SECURITY and AUD-KNOWLEDGE validation. Re-audit substantive fix diffs, results and affected integration, retaining unchanged evidence; no numbered dossier per routine revision.

Keep one current finding registry with stable IDs, scope/severity/location, evidence, failure scenario/impact, confidence, remediation/disposition, status and verification. Preserve historical raw failures/logs and disposition history. A required case failure keeps the overall batch FAIL; unavailable/unexecuted checks remain NOT_RUN/BLOCKED. Compact machine evidence may store exact inputs/source/image identities, commands, outcomes and links to raw logs; no parallel canonical JSON final report.

## Human acceptance and exceptions

L2/L3 final human acceptance binds stage/criteria, independent audit, exact reviewed content, actual human consent reference, decision/conditions and date. Record in the stage STATUS or REPORT; no separate mandatory dossier. Transcript consent is procedural evidence until a protected verifier exists; do not fabricate hashes as human signatures or claim CI verification. A user action request authorizes its specific semantic change, not unrelated effects or self-issued final acceptance.

Exceptions record affected rule, rationale/evidence, exact scope, owner/authorized approver, expiry when relevant and approval reference. No exception bypasses higher restrictions or permits author self-approval. Relevant debt keeps identity, impact, owner, resolution conditions and status; do not expand work to unrelated debt.

## Final REPORT

One current final REPORT per LARGE stage: identity/outcome; approved scope/criteria; changes and baseline/final content; actual checks/coverage; findings/disposition; blockers/risks; assumptions/dependencies/environment; evidence links; remaining work and NOT_RUN. Keep implementation completeness, verification, human acceptance and release authorization distinct. Do not duplicate verbose histories per minor step; preserve original raw evidence and prior records rather than deleting them to fit this format. Routine smaller work uses proportionate existing records and normal documentation updates.
