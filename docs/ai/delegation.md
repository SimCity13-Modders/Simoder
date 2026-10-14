# Delegation and specialist routing

Version: 1.0. Roles are instruction profiles, not independent permission boundaries.

## Discovery before routing

Enumerate available subagent tools and project/user agent definitions permitted by current scope. Inspect matching description and relevant instructions; don't read unrelated histories or credentials. Build an ephemeral registry with identity, invocation method, supported tasks/tools, write scope, availability, verification and evidence. Matching names alone do not prove capability. Never overwrite or rename an existing REA profile.

REA means Reverse Engineer Anything as named by this user. No local REA definition was found in the inspected Simoder .codex or personal agents directory during package creation. Availability is therefore UNVERIFIED, not installed or forbidden. If exposed later as a named agent/tool, inspect it before delegation. If unavailable, use simoder-researcher for bounded source/ABI investigation and explicitly disclose the fallback; don't claim it is REA or has disassembly tools.

## Dispatch

Use an existing verified specialist when it fits the goal better than a generic worker. Provide [the handoff contract](contracts.md), applicable policies, goal, allowed paths, relevant exact binaries/build hashes, permitted tools/effects, evidence required, budget and stop conditions. For REA ask for concrete binary identity, offsets/RVAs, calling convention, ownership/thread/lifetime, signature checks and counterevidence. A returned hypothesis cannot authorize patching a game process.

If the host supports custom role selection, select the verified role name. If spawn API accepts only a task/prompt, spawn with inherited model and explicitly instruct it to read the matching role file. Do not invent role/model parameters. Do not create user-visible chats for internal work. No recursive spawning by children unless separately delegated in task scope.

## Ownership and integration

Coordinator owns global scope and integration; workers own disjoint file sets. Auditors do not modify implementation. Reserve game/session/region/ports and shared services; one writer per resource, version preconditions and explicit conflict resolution. Parallel read-only analysis is allowed; concurrent edits to the same files aren't. A resource reservation is procedural until a lock implementation is verified.

Receiving agent rechecks actual baseline, grants and tools. Stop on overlap, unknown game context or unsupported ABI. Report evidence, confidence, blockers and next safe action. Coordinator reads actual artifacts, reconciles dependencies and validates the combined result. Separate subagent review improves independence of reasoning, not OS/process isolation; distinguish from required human acceptance.

