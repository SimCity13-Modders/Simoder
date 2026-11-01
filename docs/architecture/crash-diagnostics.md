# Beta crash handling and diagnostic logging

Status: user-requested Beta requirements; architecture discovery and implementation
are pending. This document grants no new process, dump, upload or capture permission.

## Required outcome

- A crash window presents a useful error report and the captured crash-thread call
  stack, with precise build identity and explicit symbol/unwind limitations.
- Buttons copy the shareable report to the clipboard, open the crash-report folder
  and open the log folder. Clipboard output follows the same redaction rules as
  the shareable text; opening a folder does not transmit its contents.
- Sensitive fields are removed or replaced in the shareable report. Default
  collection must remain limited; existing separate consent for full dumps remains.
  The user defers the default text-versus-minidump policy to discovery; separate
  consent for full dumps remains required. No new default collection is authorized.
- Logging covers loader, base, mods and qualified engine channels, and supports
  diagnostic logging across the frontend/engine bridge. stderr and other engine
  channels require discovery of the real producers before capture is implemented.

Current logging already uses a central bounded queue and captures qualified
OutputDebugString channels; see [logging](../logging.md). That does not establish
stderr capture or complete frontend/engine tracing.

## Decisions and open architecture questions

CRASH-001: the user requests discovery before deciding whether to allow a separate
crash-helper EXE. Compare reliability, installed/archive size, dependencies, startup,
offline behavior and the variant retaining the single-DLL distribution rule.
No EXE exception has been granted. Investigate handler coexistence and failure
modes, including stack exhaustion, damaged heap, reentry and abruptly lost process.
Do not promise that any in-process window survives every fatal failure.

CRASH-002: distinguish native fatal failure from an isolated script error. Existing
DEV/REGULAR isolation rules remain; the crash window must not replace ordinary
mod error handling or claim that the top stack frame proves fault ownership.
Crash coverage, hangs/watchdogs and forced termination limits need discovery.

CRASH-003: symbolicate authored binaries against exact matching symbols/build IDs.
For modules without usable symbols, preserve module identity plus relative address
and mark missing/partial frames. Capture the faulting context, not merely the
reporting thread's stack. Source line availability and engine symbol availability
require verification. Do not invent function names or guarantee a complete causal
history from a stack alone. Offline reports must remain useful without a download.

CRASH-004: propose an allowlisted shareable schema and producer-side redaction of
credentials, user paths and other sensitive fields; validate before clipboard
export. Raw memory is not assumed anonymous. No automatic upload or telemetry is
authorized. Full dumps retain explicit separate consent. Retention, size limits,
raw-versus-shareable storage and lower-detail dump consent await detailed design.

CRASH-005: preserve one semantic logging pipeline with bounded queues, rotation and
nonblocking producers. Bridge records should correlate request/response, direction,
thread, owner/generation, time and failure status without logging arbitrary payloads
or secrets by default. Detailed capture must be configurable and bounded. Qualify
stderr/CRT/debug/engine channels without a blanket interception of unrelated I/O.

## Scheduling proposal

1. During U3 planning, record emergency diagnostics and correlation requirements
   at the DLL/base boundary; do not implement crash UI or alter current U2b.
2. After base/bridge lifetime contracts settle, run a separate diagnostic discovery
   and architecture comparison. Resolve the helper-process and report-consent
   decisions, symbol packaging, privacy schema and engine-channel evidence.
3. Implement crash capture/logging hardening as its own audited acceptance unit
   before U6 game qualification. Keep minimal failure capture Core-owned so missing
   base/DevTools cannot disable it; place presentation/symbol tooling according to
   the approved architecture. MCP consumes qualified diagnostics rather than owning
   the fatal-process safety mechanism.
4. Include the crash window and bridge/engine logging in the public Beta release
   gate, clean-install tests and U6 regression evidence.

This is sequencing, not authorization to implement new capture hooks or processes.

## Acceptance evidence to prepare

Use disposable synthetic crash processes first, then separately approved game
sandbox tests. Verify crash context, matched/missing symbols, damaged-state fallback,
bounded reentry/low-memory behavior, log-tail preservation, consent rejection,
redaction and clipboard contents, all three window actions, write-denied folders,
storage/rotation bounds and absent network transfer. Test qualified engine and
bridge channels with known marker messages and correlate both ends. Distinguish
executed coverage from unsupported crash classes; no test in this document has run.

## Primary reference

Microsoft recommends a separate process for MiniDumpWriteDump where possible and
documents instability/deadlock and crash-thread context concerns. This informs the
comparison; it does not select a new dependency or override the distribution rule.
See [MiniDumpWriteDump](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump).
