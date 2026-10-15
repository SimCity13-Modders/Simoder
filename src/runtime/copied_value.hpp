#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <span>

namespace sc13::runtime::copied {
/** @summary Private transport tags; references carry no resolved authority. */
enum class Tag : std::uint8_t { Null, Bool, String, F64, U64, I64, Bytes, List, Map, Ref };
/** @summary Exact immutable flat-arena record; indexes are local to its retained Envelope. */
struct Node final {
    Tag tag{};
    std::uint64_t bits{};
    std::uint32_t first{}, next{}, count{}, offset{}, size{}, keyOffset{}, keySize{}, generation{};
};
/** @summary Strict parse/allocation outcome; failures never return an envelope. */
enum class Status { Ok, Invalid, ResourceLimit };
struct State;
struct Block;
class Storage;
/** @summary Move-only preallocated charged native storage; acquired before effects. */
class Reservation final {
public:
    /** @summary Creates an empty reservation. */
    Reservation() noexcept = default;
    /** @summary Transfers reservation ownership. @param other Source. */
    Reservation(Reservation&& other) noexcept;
    /** @summary Replaces this reservation. @param other Source. @return This reservation. */
    Reservation& operator=(Reservation&& other) noexcept;
    /** @summary Frees allocated capacity and releases its accounting lease. */
    ~Reservation();
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    /** @summary Borrows reserved bytes. @return Writable capacity. */
    std::span<std::byte> Bytes() noexcept;
    /** @summary Tests allocated reservation. @return Presence. */
    explicit operator bool() const noexcept { return size_ != 0; }
private:
    void* memory_{};
    std::size_t size_{};
    std::uint64_t owner_{};
    std::shared_ptr<State> state_;
    friend class Storage;
};
/** @summary Immutable intrusive storage lease; copies share one charged allocation. */
class Envelope final {
public:
    /** @summary Creates an empty lease. */
    Envelope() noexcept = default;
    /** @summary Retains an existing immutable allocation. @param other Source lease. */
    Envelope(const Envelope& other) noexcept;
    /** @summary Transfers an existing lease. @param other Source lease. */
    Envelope(Envelope&& other) noexcept;
    /** @summary Replaces this lease. @param other Source lease. @return This lease. */
    Envelope& operator=(Envelope other) noexcept;
    /** @summary Releases storage only at the final lease. */
    ~Envelope();
    /** @summary Tests whether this lease holds a value. @return Allocation presence. */
    explicit operator bool() const noexcept { return block_ != nullptr; }
    /** @summary Encodes without a second allocation; valid original JSON preserves all semantics. @return Borrowed wire view, valid while this lease survives. */
    std::string_view Encode() const noexcept;
    /** @summary Returns the reviewed schema. @return Borrowed schema view. */
    std::string_view Schema() const noexcept;
    /** @summary Reads a validated node. @param index Arena index. @return Record or null for invalid index. */
    const Node* At(std::uint32_t index) const noexcept;
    /** @summary Returns a node's decoded string/bytes or reference kind. @param node Record from this envelope. @return Borrowed payload view. */
    std::string_view Text(const Node& node) const noexcept;
    /** @summary Returns a map child's decoded key. @param node Record from this envelope. @return Borrowed key view. */
    std::string_view Key(const Node& node) const noexcept;
    /** @summary Returns exact requested allocation charge. @return Bytes including header, nodes, wire and decoded payload. */
    std::size_t Charge() const noexcept;
    /** @summary Reads native charged owner; never establishes authority. @return Accounting generation. */
    std::uint64_t Owner() const noexcept;
private:
    explicit Envelope(Block* block) noexcept : block_(block) {}
    Block* block_{};
    friend class Storage;
};
/** @summary Fixed-housekeeping shared quota pool; owner values are host-issued generation identities, never decoded IDs. */
class Storage final {
public:
    static constexpr std::size_t GlobalLimit = 16 * 1024 * 1024;
    static constexpr std::size_t OwnerLimit = 4 * 1024 * 1024;
    static constexpr std::size_t EnvelopeLimit = 1024 * 1024;
    /** @summary Creates a fixed 256-owner accounting pool; no lazy per-owner allocations. */
    Storage();
    /** @summary Strictly validates and copies after reserving all storage. @param wire Borrowed input; caller must keep its bytes alive and immutable for the entire call. @param schema Exact reviewed schema, also alive and immutable during this call. @param owner Host-issued nonzero generation identity. @param output Cleared on all failures; an existing lease is retained internally until input copying completes, permitting aliased input views. @param methodLimit Optional stricter wire limit. @return Validation/admission outcome. */
    Status Decode(std::string_view wire, std::string_view schema, std::uint64_t owner,
        Envelope& output, std::size_t methodLimit = EnvelopeLimit) const noexcept;
    /** @summary Reads aggregate live retained allocation charges. @return Reserved bytes. */
    std::size_t Used() const noexcept;
    /** @summary Allocates quota-accounted native capacity before effects. @param owner Native generation. @param bytes Capacity. @param output Cleared reservation. @return Allocation status. */
    Status Allocate(std::uint64_t owner, std::size_t bytes, Reservation& output) const noexcept;
    /** @summary Charges externally allocated native container capacity before allocation. @param owner Native generation. @param bytes Conservative requested allocation bound. @param output Accounting lease without a backing buffer. @return Reservation status. */
    Status Account(std::uint64_t owner, std::size_t bytes, Reservation& output) const noexcept;
    /** @summary Bounds any valid decoded envelope for a wire limit. @param wireLimit Maximum JSON bytes. @return Required arena capacity. */
    static std::size_t DecodeCapacity(std::size_t wireLimit) noexcept;
    /** @summary Decodes using already allocated storage without allocation. @param wire Stable JSON. @param schema Exact schema. @param reservation Reserved arena, consumed only on success. @param output Result. @param methodLimit Maximum wire bytes. @return Strict status. */
    Status DecodeReserved(std::string_view wire, std::string_view schema, Reservation& reservation, Envelope& output, std::size_t methodLimit = EnvelopeLimit) const noexcept;
    /** @summary Checks exact accounting-pool and owner identity. @param value Lease. @param owner Native generation. @return Matching origin. */
    bool Owns(const Envelope& value, std::uint64_t owner) const noexcept;
    /** @summary Reads one generation's live charge. @param owner Host-issued identity. @return Reserved bytes. */
    std::size_t Used(std::uint64_t owner) const noexcept;
private:
    std::shared_ptr<State> state_;
};
}
