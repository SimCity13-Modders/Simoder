# Provider and specialist qualification

Canonical role distribution files live in [codex-agents](codex-agents/). Identical local copies are installed in .codex/agents for Codex discovery. Existing Git ignore rules exclude .codex, so only the canonical docs copies are candidates for future version control. No ignore rules were changed. After an approved role update, refresh its local copy and verify byte equality; do not edit both independently.

Version: 1.0. Config authored is not behavior verified. Current-session skill catalog may require refresh before new skills appear. Explicit file invocation is usable immediately if file reading is permitted.

Codex project skills live at .agents/skills/<name>/SKILL.md; standalone custom agent TOML at .codex/agents/<name>.toml contains name, description, developer_instructions. These agents inherit parent permissions/provider/MCP; don't place sandbox/provider/server overrides in role files. Keep model/reasoning inherited. These shapes were checked against official docs on 2026-10-14, but installed host dispatch must still be tested. https://learn.chatgpt.com/docs/build-skills ; https://learn.chatgpt.com/docs/agent-configuration/subagents

When role selection unavailable, coordinator can use generic spawn with task instructing role-file read. This fallback is not proof of custom-agent auto-discovery. No modification of existing .codex/config.toml or AGENTS.md is required for this package. Root instructions integration remains separately authorized.

Claude Code installation alone does not qualify adapter; native Windows sandbox is not supported per inspected official setup documentation. No Claude native definitions are installed by this package. https://code.claude.com/docs/en/setup

REA availability is UNVERIFIED locally. See delegation.md. Third-party roles/tools are discovered and vetted per session. Preserve their required policies if compatible; escalate conflict without granting new privilege.

For every capability record declared support NATIVE/ADAPTER_BASED/SCRIPT_BASED/INSTRUCTION_ONLY/UNSUPPORTED independently of evidence state VERIFIED/UNVERIFIED/FAILED and provider/version/OS/date/source. Instruction-only protection cannot be described as deterministic enforcement.

