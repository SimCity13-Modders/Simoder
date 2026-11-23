# Release hygiene and the no-legacy gate

Accepted project requirement: 2026-10-17. Scope: Simoder ecosystem release
qualification. Apply each repository's ownership and upstream rules; this contract
does not authorize edits outside an approved task, deployment or publication.

## Continuous cleanup

The stage that replaces an implementation must remove the superseded code,
call sites, build/install entries, configuration, dependencies and documentation
within its owned scope. Do not postpone all cleanup until release stabilization.
If removal requires a later migration stage, record the exact retained paths,
reason, owning stage and removal criterion in `.analysis/`; keep the maintained
architecture explicit about the remaining migration boundary. Such retention is
temporary migration debt, not an accepted Beta compatibility requirement.

Reuse canonical algorithms and contracts rather than creating parallel owners,
schedulers, registries or validators. Assess DRY and SOLID by responsibilities,
dependency direction and behavior, not file length or abstraction count alone.
Preserve validation, security checks and required game-format support during cleanup.

## Beta Stabilization / Legacy Elimination

After the Beta scope is confirmed and its required features are implemented,
perform a dedicated stabilization stage before creating a Beta Release Candidate
(RC). Scope confirmation alone is not completion of implementation. U3 completion
does not certify ecosystem-wide removal of legacy; U4/U5 and the game-UI replacement
must retire their respective old paths before this gate can pass.

**RC entry requires Strictly NO Legacy:** no superseded alpha implementation,
retired API/format compatibility shim, old runtime route or hidden fallback remains
in the maintained first-party source or distribution. A disabled flag, excluded
build target, renamed class or unshipped obsolete source file is not removal.
Git history and ignored investigation evidence may retain historical material;
no destructive history rewrite is required. Historical notes must not instruct
users to deploy retired components.

An old format name belonging to the game's current external data contract is not
automatically obsolete Simoder code. Likewise a supported Windows compatibility
path or separately justified recovery mechanism is not automatically legacy.
Classify each case by required current behavior and evidence, not a keyword scan.
Do not remove third-party code or notices indiscriminately; review retained
dependencies and remove those no longer needed by the supported product.

## Independent full engineering audit

Bind the audit to exact cross-repository source revisions/content, dependency pins
and the actual distribution candidate. Independent reviewers must assess:

- DRY: duplicated behavior and divergent copies, including generated/vendored
  artifacts and their canonical source/provenance.
- SOLID: responsibilities, interfaces, dependency direction and substitutability
  where applicable; preserve DLL/base/SDK/DevTools/OpenSCP ownership boundaries.
- Dead and legacy code: call paths, unused APIs/branches, obsolete fixtures,
  configurations, build/install rules, dependencies and stale documentation.
- Security and lifecycle: authority boundaries, concurrency, resource ownership,
  shutdown, error handling and recovery without weakening safeguards.
- Distribution: inspect actual packaged files, binaries and dependency inventory;
  source cleanup or successful linking alone is insufficient evidence.

Use suitable static analysis, source/call-site inspection and executed tests.
A source file listed in CMake, a passing build or linker elimination does not
prove absence of dead code. Replace tests for retired contracts with regression
coverage of supported behavior; do not retain legacy implementations solely to
keep their tests passing or weaken tests to hide regressions.

Record findings, corrections, verification limits and exact candidate identities
in `.analysis/`. Unresolved legacy, security or required-behavior findings block RC
entry. An author cannot independently audit their own changes. Explicit human
acceptance remains required under the applicable project acceptance rules.

## Release Candidate qualification

After stabilization audit and acceptance, freeze features and qualify the exact RC:
full regressions, clean workspace/game installation, documentation, supported-game
operation and the autonomous mod-creation demonstrator. Material fixes require
affected tests and independent re-audit; update candidate identities and reassess
the no-legacy gate. RC qualification, final release acceptance and authorization
to publish are separate gates. These requirements do not themselves authorize
game/VM/crash tests or change currently parked work.

The public Beta establishes the compatibility baseline; pre-Beta prototypes do
not require retained compatibility layers. See [compatibility](compatibility.md)
and the [roadmap](../roadmap.md).
