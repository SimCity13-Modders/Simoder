# Repository rules

- Put audits, exploratory analyses, experiment reports, screenshots and raw investigation evidence in `.analysis/`. This directory is ignored and must not be committed.
- Keep maintained user guides, architecture contracts and API reference material in `docs/`.
- Document authored code with Doxygen-style comments, including `@summary` and relevant parameter/return annotations.
- Implement complete behavior; do not add TODO placeholders or retired runtime paths.
- The production distribution is one integrated x86 `d3d9.dll` placed beside `SimCity.exe`. Developer programs and example mods belong to Simoder-DevTools.
- Preserve local work. Do not push or publish a release unless the user explicitly requests it.
- Prefix shell commands with `rtk`; follow `C:/Users/dawid/.codex/RTK.md`.
