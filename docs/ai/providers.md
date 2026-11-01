# Provider and specialist qualification

Role source ownership belongs to Simoder-Adapter-Codex. The prepared [role payload](codex-agents/) and identical local .codex/agents copies preserve project discovery; provenance binds the canonical package revision and bytes. These consumer copies are not independently editable canonical sources. Existing Git ignore rules exclude .codex, so only the canonical docs copies are candidates for future version control. No ignore rules were changed. After an approved role update, refresh its local copy and verify byte equality; do not edit both independently.

Version: 1.1. Config authored is not behavior verified. Current-session skill catalog may require refresh before new skills appear. Explicit file invocation is usable immediately if file reading is permitted.

Codex project skills live at .agents/skills/<name>/SKILL.md; standalone custom agent TOML at .codex/agents/<name>.toml contains name, description, developer_instructions. These agents inherit parent permissions/provider/MCP; don't place sandbox/provider/server overrides in role files. Keep model/reasoning inherited. These shapes were checked against official documentation during the 2026-10-15 discovery. Existing session exposure and one role dispatch do not verify a new package cutover; fresh-host loading and delegation must still be tested after separately approved activation. https://learn.chatgpt.com/docs/build-skills ; https://learn.chatgpt.com/docs/agent-configuration/subagents

When role selection unavailable, coordinator can use generic spawn with task instructing role-file read. This fallback is not proof of custom-agent auto-discovery. No modification of existing .codex/config.toml or AGENTS.md is required for this package. Root instructions integration remains separately authorized.

Claude Code installation alone does not qualify an adapter. Discovery on 2026-10-15 observed an installed binary with Windows file metadata version 2.1.292.0 without executing it or inspecting credentials. No Claude native definitions are installed by this package. The documented Bash sandbox is unavailable on native Windows; file tools, hooks and MCP are outside that Bash sandbox. Do not treat permission prompts or a WSL2 assumption as equivalent isolation or Windows IPC compatibility. https://code.claude.com/docs/en/sandboxing ; https://code.claude.com/docs/en/sub-agents ; https://code.claude.com/docs/en/skills

REA availability is qualified per session; see delegation.md. The 2026-10-15 discovery observed 170 advertised REA tools and performed no live health probe. This dated observation does not guarantee future server readiness or authorize tool operations. Third-party roles/tools are discovered and vetted per session. Preserve their required policies if compatible; escalate conflict without granting new privilege.

For every capability record declared support NATIVE/ADAPTER_BASED/SCRIPT_BASED/INSTRUCTION_ONLY/UNSUPPORTED independently of evidence state VERIFIED/UNVERIFIED/FAILED and provider/version/OS/date/source. Instruction-only protection cannot be described as deterministic enforcement.

