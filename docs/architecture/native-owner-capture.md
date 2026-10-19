# Native query owner capture: code retention boundary

The optional native query observer belongs to Simoder Core's existing exact-build session adapter. Its callback original, MinHook entry/trampoline and integrated DLL remain process-owned after creation. Ordinary shutdown and failed-enable cleanup never remove the hook entry, uninitialize MinHook or clear the published original. A callback paused before entry accounting may still resume through that same original after optional observation closes; a zero activity count does not authorize unloading it.

The private `OwnerCaptureLifecycle` uses only Create/Enable/Disable control operations, publishes the original before Enable, denies foreign/inconsistent ownership and refuses reinitialization after decline/retirement. Failed enable conservatively attempts the exact owned Disable and retains both actual outcomes. Failed/uncertain disable retains dependencies and reports failure. An acknowledged disable is still Busy while original forwarding activity remains. Repeated stop can reconcile the same owned disable/drain without recreating or removing the hook.

Actual callbacks reuse `RuntimeHookActivity::Entry` through original return. No new counter, VM, scheduler, transport or permission authority is introduced. Global observation retirement closes all optional scopes through an epoch; writing one thread's TLS is not treated as revoking other threads. Nested scopes invalidate observation rather than inheriting or restoring an old owner. Stop closes optional observation before ordinary runtime drain/disable, and an uncertain capture shutdown prevents optional bridge dependency clearing.

## Remaining borrowed-data limitation

This code-retention boundary **does not prove** the native owner object's lifetime. The existing diagnostic path borrows `self+0xc` and later reads owner/vtable/method addresses; virtual-memory readability, correct thread identity and retained trampoline code do not establish semantic ownership of those native data. That behavior is retained as the explicitly unresolved E5 borrowed-owner scope, not certified safe or newly enabled by host tests.

The `nativeBindings` world-units diagnostic remains observational and keeps its existing available/unavailable shapes. An address is not an object lease or a callable API. Complete E5 closure requires exact native ownership evidence and a qualified bounded copied-observation implementation, or an explicitly approved complete retirement of the optional diagnostic feature. No guessed retain/release method or game ABI is permitted. U5 deployment remains blocked while that mandatory qualification is unresolved.

## Validation boundary

The isolated `simoder-native-owner-capture-lifecycle-tests` executable links the actual private production lifecycle implementation. It injects hook-control outcomes and synthetic original functions to test publication, failed Create/Enable/Disable, retained originals, duplicate/reinitialization refusal, pre-entry pause, in-flight original ownership, nested scopes and other-thread epoch revocation. It never calls MinHook, patches game code, acquires game pointers or qualifies x86/native object lifetime.

Native runtime compilation and ordinary session/router/window regressions are separate evidence. Successful host tests are not actual MinHook scheduling, process quiescence, native object ownership, game functionality, SDK/HTTP authentication or production activation acceptance. Raw investigation and independent audits remain in ignored `.analysis/`.
