# Control and enforcement register

Version: 1.1. Snapshot for documentation package; no protected acceptance verifier/CI implementation supplied.

| ID | Control | Current mechanism | Enforcement/verification |
| --- | --- | --- | --- |
| CTL-001 | Scope and dirty-work preservation | Human instruction plus agent procedure | AGENT_ENFORCED; per-task inspection needed |
| CTL-002 | OS/files/network restrictions | Host sandbox and tool approvals | Provider-native; actual session boundary only; don't infer alternate tool coverage |
| CTL-003 | L2/L3 independent audit | Separate role and evidence | AGENT_ENFORCED; role alone not isolation |
| CTL-004 | Human acceptance | Explicit human message tied to reviewed content | Procedural currently; cryptographic local/CI verifier NOT_IMPLEMENTED |
| CTL-005 | Same-content rebase mapping | Compare task-owned final content to audited baseline | Procedure; automated protected verifier NOT_IMPLEMENTED |
| CTL-006 | API/runtime safety | Source/tests and approved sandbox tests | Scope-specific results only; not established by docs |
| CTL-007 | Instruction provenance | Approve exact external instruction content | AGENT_ENFORCED; no automatic trusted-document parser |
| CTL-008 | Knowledge links/skill syntax | Bundled skill validator and package inspection | Structural validation only; semantic audit separate |

Required unresolved deterministic controls must block claims of verified enforcement; do not disable the test or relabel missing control PASS. Reporting these gaps is compatible with using the procedural standard now.


Stage-scale records and runner reuse are procedures under [lifecycle](lifecycle.md), not new enforcement or authority. Exact executed content and actual failures/NOT_RUN remain visible. Protected instruction updates require independent security and knowledge validation before final human acceptance.
