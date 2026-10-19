# Native bindings diagnostic status

Successful world-unit replies retain `nativeBindings:{"available":false}` as the supported status. World-unit enumeration, entity references, positions, roads and resource data remain available through the existing router and tools. The existing serialized request/result augmentation predicate remains unchanged.

The optional raw native-owner diagnostic has been retired by explicit approval. The runtime no longer installs its callback detour, stores its owner pointer in thread-local state, or reads its owner/vtable/method addresses during reply serialization. Historical `available:true` address observations are intentionally no longer produced. They were observations, never object leases or a callable mod API.

The private, platform-independent `AppendNativeBindingsStatus` helper adds only the literal unavailable status. Its isolated test exercises the actual production helper, preserving existing world data and leaving error, non-world and non-matching serialized replies unchanged. It does not inspect Windows memory, establish game ABI ownership or certify unrelated native bridges.

Main runtime hooks retain their existing callback admission, process-retained originals, global close/drain, window/transport drain and dependent-resource shutdown responsibilities. Removing the optional observer does not establish universal process quiescence or permission to unload remaining hook code.

The retired capture and lifecycle sources, fixture and build registrations are removed within their owning stage. Historical investigation and audit evidence remain in ignored `.analysis/` and Git history. Actual fresh x86 builds, surviving regressions and independent audit are separate requirements; isolated serialization tests are not gameplay or deployment acceptance.
