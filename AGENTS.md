# Repository rules

- Put audits, exploratory analyses, experiment reports, screenshots and raw investigation evidence in `.analysis/`. This directory is ignored and must not be committed.
- Keep maintained user guides, architecture contracts and API reference material in `docs/`.
- Document authored code with Doxygen-style comments, including `@summary` and relevant parameter/return annotations.
- Implement complete behavior; do not add TODO placeholders or retired runtime paths.
- Follow [release hygiene](docs/architecture/release-hygiene.md): remove replaced code within its owning migration stage; Beta Release Candidate entry requires Strictly NO Legacy and an independent full engineering audit. Disabled legacy paths do not satisfy this gate.
- The production distribution is one integrated x86 `d3d9.dll` placed beside `SimCity.exe`. Developer programs and example mods belong to Simoder-DevTools.
- **Git & Gitea Workflow**: Preserve local work during active development. Closing a task in Gitea strictly requires explicit user approval. Upon approval: commit referencing the task (e.g. `Refs #<id>` / `Closes #<id>`), push to remote, and close the Gitea issue with a comment referencing the commit (bidirectional traceability).
- Prefix shell commands with `rtk`; follow `C:/Users/dawid/.codex/RTK.md`.

## Delivery workflow

- Work in large approved stages (for example U4/U5). Keep one stage STATUS/decisions record, one final REPORT and independent final audit; internal fixes do not need separate dossiers or acceptance gates.
- Compile and run approved local regressions early, before implementation audit. Batch independent cases; any required failure still makes the overall result FAIL. Preserve raw failures and NOT_RUN evidence.
- Reuse qualified runners, toolchains and security profiles. Review changed effects or risks, not an unchanged procedure for each source fix or new run ID.
- Preserve pre-review and applicable approval for architecture/material contracts, security policy, privilege expansion and destructive/external actions. L2/L3 final stage acceptance remains human; intermediate autonomy follows the explicit user scope.
- Maintain API/architecture/user documentation with behavior at every size. Use the [shared lifecycle](docs/ai/lifecycle.md) when available; it does not grant extra permissions.
