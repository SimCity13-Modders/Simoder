/** @file @summary Qualified concrete Windows primitives; common owners/interfaces are declared without platform headers in store_io.hpp. */
#pragma once
#include "platform/component_store/store_io.hpp"


/** @summary Preserve caller Windows macros while declaring portable operation names unchanged. */
#pragma push_macro("CreateFile")
#pragma push_macro("CreateDirectory")
#undef CreateFile
#undef CreateDirectory

namespace sc13::platform::component_store {
/** @summary Fresh local root/account facts for attachment comparison; these are neither consent nor a package issuer. */
struct LocalAttachment final {std::string backend;std::string normalizedRootUtf8;std::string volumeIdentity;ObjectIdentity root;std::vector<std::byte> account;};
/** @summary Non-authorizing Windows filesystem operations over exact retained ordinary roots.
 * @remarks The native coordinator independently verifies maintenance scope, portable metadata, package trust,
 * attachment and quiescence. This interface cannot mint or persist that authority. UTF-8 paths are strictly decoded;
 * store paths are portable relative paths and all ancestor/object aliases, links and special objects are refused.
 * These declarations do not provide a POSIX implementation or an unsupported durability fallback. */
class WindowsStore final {
public:
    /** @summary Open actual ordinary ancestry of an absolute selected parent with strict no-delete-sharing retention. */
    static Result OpenParent(std::string_view absoluteUtf8,Parent& output,Observation& observed)noexcept;
    /** @summary Acquire the SAME fixed coordination.lock exclusively inside an independently admitted retained store; never delete stale locks. */
    static Result Lock(Store& store,Coordination& output)noexcept;
    /** @summary Check dependent leases and explicitly close the same original lock; uncertain close is never success or a retry permission. */
    static Result ReleaseCoordination(Coordination& lock)noexcept;
    /** @summary Exclusively create absent fixed .simoder and its CREATE_NEW coordination.lock against the exact retained parent.
     * @remarks Only the successful directory creator creates its lock. Any existing root refuses without adoption.
     * A create-before-identity/lock capture failure is Unknown and preserved for explicit recovery, never silently retried. */
    static Result CreateStore(Parent& parent,const Observation& expectedParent,Store& output,Coordination& lock,Observation& created)noexcept;
    /** @summary Open existing ordinary fixed .simoder without adopting its contents or accepting copied history as consent. */
    static Result OpenStore(Parent& parent,Store& output,Observation& observed)noexcept;
    /** @summary Capture fresh retained-root, normalized-path and actual OS-account attachment facts without changing them. */
    static Result ObserveAttachment(Parent& parent,LocalAttachment& output)noexcept;
    /** @summary Enumerate one closed ordinary namespace with an explicit maximum1..2048 and no recursion.
     * @remarks Unknown, unreadable, aliased/special or excess entries fail closed. Output on non-Ok is incomplete diagnostic evidence,
     * never a complete inventory or quota certificate. No entry is removed, adopted or selected by its name. */
    static Result ListDirectory(Store& store,Coordination& lock,InventoryTarget target,std::uint32_t maximum,std::vector<NamedObservation>& output)noexcept;
    /** @summary Create one absent ordinary relative directory and capture its exact local identity; never reuse a collision. */
    static Result CreateDirectory(Store& store,Coordination& lock,std::string_view relativeUtf8,Observation& created)noexcept;
    /** @summary Open one existing ordinary relative file for bounded read with strict original preimage sharing. */
    static Result OpenRead(Store& store,Coordination& lock,std::string_view relativeUtf8,std::uint64_t maximum,File& output,Observation& observed)noexcept;
    /** @summary Read the same retained original file into caller-owned bounded storage; report exact received length. */
    static Result Read(File& file,std::span<std::byte> output,std::size_t& received)noexcept;
    /** @summary Exclusively create one absent relative ordinary file; capture its exact created identity before later disposition. */
    static Result CreateFile(Store& store,Coordination& lock,std::string_view relativeUtf8,std::uint64_t maximum,File& output,Observation& created)noexcept;
    /** @summary Append bounded caller-owned bytes to this exact exclusively created file without opening another path. */
    static Result Write(File& file,std::span<const std::byte> bytes)noexcept;
    /** @summary Flush actual file content; this never claims directory-entry persistence. */
    static Result Flush(File& file)noexcept;
    /** @summary Explicitly close one original file; no delete, rename or completion inference on failure. */
    static Result Close(File& file)noexcept;
    /** @summary Publish one flushed original immutable metadata source into a closed derived absent target without replacement.
     * @remarks Attachment requires empty identity; State requires64 nonzero lowercase hex; Plan/Journal require32.
     * Content hashes, schema, transaction linkage and authority are independently verified by the coordinator. */
    static Result PublishRecord(Store& store,Coordination& lock,File& candidate,RecordTarget target,PublicationRequirement requirement)noexcept;
    /** @summary Publish the same verified staged ordinary directory into an absent generation path without replacement or copy fallback.
     * @remarks Caller serializes owner access and persists the finite created-identity evidence before publication.
     * All original writer AND reader files must close first. Only this backend's descendant directory handles close explicitly;
     * the original source-root and protected ancestors stay retained. Closed child identities are historical observations,
     * not live leases, and later read-only reacquisition must match those exact physical identities.
     * Any started failed publication refuses replay/mutation; target collisions preserve all content. */
    static Result PublishGeneration(Store& store,Coordination& lock,std::string_view stagedRelativeUtf8,const Observation& expectedStaged,
        std::string_view generationRelativeUtf8,PublicationRequirement requirement)noexcept;
    /** @summary Conditionally publish an original flushed candidate selector against expected original current.json under coordination.
     * @remarks AtomicVisibility does not promise power-loss durability. Unsupported required semantics refuse before effects;
     * ambiguous effects return Unknown and never become success through readback. Expected absence is explicit, not an empty hash. */
    static Result PublishSelection(Store& store,Coordination& lock,File& candidate,const Observation* expectedCurrent,
        std::span<const std::byte> expectedCurrentBytes,PublicationRequirement requirement)noexcept;
    /** @summary Remove only an exact originally created ordinary empty directory after fresh identity verification, explicit original close and actual namespace absence.
     * @remarks Delete-pending, failed close, compatible outstanding readers or replacement entries return Unknown and preserve original evidence. */
    static Result RemoveCreatedDirectory(Store& store,Coordination& lock,std::string_view relativeUtf8,const Observation& expectedCreated)noexcept;
};
#if defined(SIMODER_COMPONENT_STORE_TEST)
namespace test {
/** @summary Isolated deterministic next-write cut after an actual first chunk; absent from every shipping compilation. */
void FailWriteAfterFirstChunk(bool enabled)noexcept;
}
#endif
}

#pragma pop_macro("CreateDirectory")
#pragma pop_macro("CreateFile")
