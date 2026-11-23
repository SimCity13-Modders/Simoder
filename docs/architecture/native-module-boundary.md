# Native module host boundary

The Core-owned boundary in `src/mods/native/native_module_loader.*` prepares trusted
native modules using host ABI version 2. It reuses `RuntimeOwner`, `RuntimeScope`,
the shared `RuntimeCommitDomain` and factual `RuntimeLifecycle`. It is not wired
into manifest discovery or `ModManager` activation yet. No native callback is added
under the manager mutex. This stage does not move MCP or implement `base`.

## Qualified alpha profile

The reviewed registration comes from Core, not a module's own manifest claims. It
binds a stable module ID, exact SHA-256 and an already approved native-code trust
decision. This internal seam does not implement the user consent UI, signatures,
publisher trust or a permission grant. Native DLLs execute unrestricted machine
code; host API gates are not a sandbox.

The current profile accepts PE32 x86 DLLs with no entry point, TLS, resources,
bound/delay imports or CLR metadata. Imports are named `kernel32.dll` functions
resolved to the observed Windows system modules. Other dependency profiles require
separate validation before support. Fixtures use `/NOENTRY /NODEFAULTLIB /MANIFEST:NO`.
This restriction ensures module initialization happens through Query/Prepare after
the host retains the loaded image; an ordinary failed `DllMain` could otherwise
cause the Windows loader to unmap it before returning a handle.

The host verifies absolute plain paths, ancestor/file leases, absence of reparse
points and multiple hard links, exact content, bounded PE tables and exports before
loading. A unique staging directory receives reviewed bytes. The staged file is
reopened, rehashed and identity-checked while retained read leases prevent writing
or deletion. Load uses a fully qualified path and `LOAD_LIBRARY_SEARCH_SYSTEM32`;
no current directory, PATH, sibling DLL or process-wide search policy is added.
Loaded identity, export addresses and resolved imports are checked again. Windows
system modules are the OS trust boundary, not redistributed dependencies.

These choices follow the [LoadLibraryExW contract](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw),
[file sharing rules](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
and [DLL security guidance](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-security).

## Ownership and lifecycle

Required undecorated exports are `SimoderModule_QueryV2`, `SimoderModule_PrepareV2`,
`SimoderModule_AdmitV2`, `SimoderModule_QuiesceV2` and `SimoderModule_DrainV2`.
Descriptor size/version, reserved fields, base-contract version and full normative
bundle digest must match exactly. No alpha compatibility fallback is provided.
ABI handles use a checked host registry; U1's 64-bit identities are not truncated.

Prepare creates a fresh owner and immutable borrowed tables. Lifecycle facts are
available; dispatch, credential and semantic facets are NULL. Production Admit
returns NOT_READY because a qualified base provider does not exist. Only the
separately compiled fixture host has synthetic admission. Reusing identical image
bytes can create a new logical owner after the previous owner for that module ID
has drained, never a second active service for the same ID.

Quiesce closes host admission before invoking module code, outside host locks.
Drain returns BUSY while callbacks, registration or cooperative resources remain.
The module drains its own registrations before Core cleans up residual host
registrations. Core retains the first successful module Drain in the original
State. If residual callback or scope cleanup is still BUSY, subsequent attempts
retry that host cleanup without calling module Drain or Quiesce again: the
module's successful Drain has already ended its borrow. This private milestone
does not prove callback completion or final Core Drained; both still require
their original fences. No host lock spans a module export call.
A successful drain does not authorize `FreeLibrary`. Every loaded image, failed
preparation and borrowed host state stays retained to process exit. Unknown native
threads cannot be forcibly proven safe or stopped by a cooperative ABI. Native
hangs/crashes remain process risks. A failed OS load closes later loading until
restart rather than claiming retention of an unavailable handle.

Finite process bounds are eight retained image slots, 64 MiB of declared mapped
image space, 16 MiB per source file, 128 logical generations/staging attempts and
64 native lifecycle subscriptions per generation. Image metadata, pinned file
bytes and retained host state also consume bounded storage outside mapped-image
accounting. These are conservative admission ceilings, not measured performance
or a qualified Beta workload. Fixture builds lower the image count to two and
exercise exhaustion; production activation remains closed. Exhaustion requests a
process restart before another image executes. Existing U1 quotas are unchanged.

## Repository and verification boundaries

The private [semantic participant transport](semantic-participants.md) distinguishes
genuine script VMs from original retained native State. This lifetime foundation
does not enable the currently closed production semantic facet or issue authority.

DevTools owns `tests/native-module`, a standalone synthetic-host project. Its DLLs
include only ABI declarations and kernel32 imports; they do not link Core. The host
compiles the actual Core boundary with explicitly isolated test macros. Product
Core has no synthetic authority/base flag. The canonical declaration source is
`Simoder-SDK/Native`. Core carries two byte-identical, hash-pinned copies, verified
by `cmake/VerifyNativeSdk.cmake` on every configure. The optional explicit
`SIMODER_NATIVE_SDK_SOURCE_DIR` also checks the exact clean SDK revision; there is
no sibling discovery or network fallback. SDK changes require fixture compatibility
and consumer pin updates. The frozen semantic table layout does not make its
currently NULL facet callable.

Native fixtures verify loading, negotiation, lifetime and failure cases. Existing
runtime/bootstrap regressions verify the unchanged consumers. Neither establishes
working base tasks/await, agent authentication, gameplay parity or the installation
workflow. Later stages implement those providers and then cut over MCP. PR #7 is
unrelated and remains parked.

For an isolated source-integrity regression, run
`rtk proxy python tests/native_sdk_integrity_tests.py --work-dir <new-owned-directory> --sdk-source <explicit-sdk-checkout>`.
It performs a local exact-byte checkout and actual positive/negative CMake
configurations; it does not modify the source SDK or contact a remote.
