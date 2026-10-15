#include "runtime/copied_value.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace sc13::runtime::copied {
/** @summary Fixed metadata independent of payload count; its lifetime follows outstanding blocks. */
struct State final {
    struct Owner { std::uint64_t id{}; std::size_t used{}; };
    std::mutex mutex;
    std::array<Owner, 256> owners{};
    std::size_t used{};
};
/** @summary Single allocation header; trailing nodes, decoded bytes and wire have exact reserved sizes. */
struct alignas(Node) Block final {
    std::atomic_size_t refs{1};
    std::shared_ptr<State> state;
    std::size_t charge{}, wireSize{}, textSize{};
    std::uint64_t owner{};
    std::uint32_t nodes{}, schemaOffset{}, schemaSize{};
};
namespace {
/** @summary Returns the trailing node array. @param block Allocation header. @return Aligned node region. */
Node* Nodes(Block* block) noexcept { return reinterpret_cast<Node*>(block + 1); }
/** @summary Returns the trailing decoded payload region. @param block Allocation header. @return Payload bytes. */
char* Texts(Block* block) noexcept { return reinterpret_cast<char*>(Nodes(block) + block->nodes); }
/** @summary Releases a previously reserved allocation charge. @param state Shared pool. @param owner Host generation. @param bytes Exact reserved charge. */
void Release(State& state, std::uint64_t owner, std::size_t bytes) noexcept {
    std::lock_guard lock(state.mutex);
    state.used -= bytes;
    for (auto& entry : state.owners) if (entry.id == owner) {
        entry.used -= bytes;
        if (!entry.used) entry.id = 0;
        return;
    }
}
/** @summary Reserves both quotas atomically without allocating. @param state Pool. @param owner Host generation. @param bytes Exact charge. @return Successful reservation. */
bool Reserve(State& state, std::uint64_t owner, std::size_t bytes) noexcept {
    std::lock_guard lock(state.mutex);
    State::Owner* selected{};
    State::Owner* empty{};
    for (auto& entry : state.owners) {
        if (entry.id == owner) selected = &entry;
        if (!entry.id && !empty) empty = &entry;
    }
    if (!selected) selected = empty;
    if (!selected || bytes > Storage::GlobalLimit - state.used || bytes > Storage::OwnerLimit - selected->used) return false;
    selected->id = owner; selected->used += bytes; state.used += bytes; return true;
}
/** @summary Parses one strict JSON string as a streaming UTF-8 byte iterator without temporary allocations. */
struct StringReader final {
    std::string_view wire;
    std::size_t pos{};
    std::array<unsigned char, 4> pending{};
    unsigned length{}, cursor{};
    bool valid{true}, done{};
    /** @summary Decodes four hexadecimal digits. @return Code unit or sentinel. */
    unsigned Hex4() noexcept {
        unsigned value{};
        for (unsigned i = 0; i != 4; ++i) {
            if (pos == wire.size()) { valid = false; return 0; }
            const auto c = wire[pos++];
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) { valid = false; return 0; }
            value = value * 16 + static_cast<unsigned>(digit);
        }
        return value;
    }
    /** @summary Returns the next validated decoded UTF-8 byte. @return Byte or -1 at end/error. */
    int Next() noexcept {
        if (cursor != length) return pending[cursor++];
        cursor = length = 0;
        if (!valid || done || pos == wire.size()) { valid = false; return -1; }
        unsigned c = static_cast<unsigned char>(wire[pos++]);
        if (c == '"') { done = true; return -1; }
        if (c < 32) { valid = false; return -1; }
        if (c == '\\') {
            if (pos == wire.size()) { valid = false; return -1; }
            c = static_cast<unsigned char>(wire[pos++]);
            switch (c) {
            case '"': case '\\': case '/': return static_cast<int>(c);
            case 'b': return 8; case 'f': return 12; case 'n': return 10; case 'r': return 13; case 't': return 9;
            case 'u': {
                unsigned scalar = Hex4();
                if (scalar >= 0xd800 && scalar <= 0xdbff) {
                    if (pos + 2 > wire.size() || wire.substr(pos, 2) != "\\u") { valid = false; return -1; }
                    pos += 2; const auto low = Hex4();
                    if (low < 0xdc00 || low > 0xdfff) { valid = false; return -1; }
                    scalar = 0x10000 + ((scalar - 0xd800) << 10) + low - 0xdc00;
                } else if (scalar >= 0xdc00 && scalar <= 0xdfff) { valid = false; return -1; }
                if (!valid || !scalar) { valid = false; return -1; }
                if (scalar < 128) return static_cast<int>(scalar);
                if (scalar < 2048) { pending[0] = static_cast<unsigned char>(0xc0 | (scalar >> 6)); length = 2; }
                else if (scalar < 65536) { pending[0] = static_cast<unsigned char>(0xe0 | (scalar >> 12)); length = 3; }
                else { pending[0] = static_cast<unsigned char>(0xf0 | (scalar >> 18)); length = 4; }
                for (unsigned i = 1; i < length; ++i) pending[i] = static_cast<unsigned char>(0x80 | ((scalar >> (6 * (length - i - 1))) & 63));
                cursor = 1; return pending[0];
            }
            default: valid = false; return -1;
            }
        }
        if (c < 128) return static_cast<int>(c);
        unsigned extra{}, scalar{}, minimum{};
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; scalar = c & 31; minimum = 128; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; scalar = c & 15; minimum = 2048; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; scalar = c & 7; minimum = 65536; }
        else { valid = false; return -1; }
        pending[0] = static_cast<unsigned char>(c); length = extra + 1;
        for (unsigned i = 1; i <= extra; ++i) {
            if (pos == wire.size()) { valid = false; return -1; }
            const auto next = static_cast<unsigned char>(wire[pos++]);
            if ((next & 0xc0) != 0x80) { valid = false; return -1; }
            pending[i] = next; scalar = (scalar << 6) | (next & 63);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) { valid = false; return -1; }
        cursor = 1; return pending[0];
    }
};
/** @summary Two-pass parser; recursion is limited by tagged depth and JSON structure is fixed by each tag. */
class Parser final {
public:
    /** @summary Configures counting or writing mode. @param wire Source JSON. @param schema Reviewed identifier. @param block Optional exact preallocated arena. */
    Parser(std::string_view wire, std::string_view schema, Block* block = nullptr) noexcept : wire_(wire), schema_(schema), block_(block) {}
    /** @summary Parses the complete envelope. @return Strict outcome. */
    Status Parse() noexcept {
        unsigned seen{};
        if (!Eat('{')) return status_;
        for (unsigned i = 0; i != 3; ++i) {
            char key[8]{}; std::size_t size{};
            if (!String(key, sizeof key, size, false) || !Eat(':')) return status_;
            const std::string_view name(key, size);
            unsigned flag{};
            if (name == "v") { flag = 1; if (!Eat('1')) return status_; }
            else if (name == "schema") {
                flag = 2; const auto offset = text_;
                if (!String(nullptr, 65536, size, true, schema_)) return status_;
                if (block_) { block_->schemaOffset = static_cast<std::uint32_t>(offset); block_->schemaSize = static_cast<std::uint32_t>(size); }
            } else if (name == "value") { flag = 4; if (!Value(1, 0, 0)) return status_; }
            else return Invalid();
            if (seen & flag) return Invalid();
            seen |= flag;
            if (i != 2 && !Eat(',')) return status_;
        }
        if (!Eat('}')) return status_;
        Space(); if (pos_ != wire_.size() || seen != 7) return Invalid();
        return status_;
    }
    std::size_t nodes_{}, text_{};
private:
    /** @summary Rejects malformed input. @return Failure status. */
    Status Invalid() noexcept { status_ = Status::Invalid; return status_; }
    /** @summary Skips only JSON whitespace. */
    void Space() noexcept { while (pos_ < wire_.size() && (wire_[pos_] == ' ' || wire_[pos_] == '\t' || wire_[pos_] == '\n' || wire_[pos_] == '\r')) ++pos_; }
    /** @summary Consumes exact punctuation. @param c Required byte. @return Match. */
    bool Eat(char c) noexcept { Space(); if (pos_ == wire_.size() || wire_[pos_++] != c) { Invalid(); return false; } return true; }
    /** @summary Peeks punctuation. @param c Byte. @return Match without consuming. */
    bool Peek(char c) noexcept { Space(); return pos_ < wire_.size() && wire_[pos_] == c; }
    /** @summary Parses bounded string, optionally copying to arena and checking exact expected contents. @param out Fixed local output or null. @param limit Maximum decoded bytes. @param size Decoded length. @param retain Store in arena. @param expected Optional exact comparison. @return Valid string. */
    bool String(char* out, std::size_t limit, std::size_t& size, bool retain, std::string_view expected = {}) noexcept {
        if (!Eat('"')) return false;
        StringReader reader{wire_, pos_}; size = 0;
        for (int byte = reader.Next(); byte >= 0; byte = reader.Next()) {
            if (size == limit) { status_ = retain && !out ? Status::ResourceLimit : Status::Invalid; return false; }
            if (!expected.empty() && (size >= expected.size() || byte != static_cast<unsigned char>(expected[size]))) { Invalid(); return false; }
            if (out) out[size] = static_cast<char>(byte);
            if (retain && block_) {
                if (text_ >= block_->textSize || size >= block_->textSize - text_) { Invalid(); return false; }
                Texts(block_)[text_ + size] = static_cast<char>(byte);
            }
            ++size;
        }
        if (!reader.valid || !reader.done || (!expected.empty() && size != expected.size())) { Invalid(); return false; }
        pos_ = reader.pos; if (retain) text_ += size; return true;
    }
    /** @summary Compares decoded UTF-8 keys without allocations or recursion. @param previous Raw start after quote. @param current Raw start after quote. @return Strictly increasing byte order. */
    bool Ascending(std::size_t previous, std::size_t current) noexcept {
        StringReader left{wire_, previous}, right{wire_, current};
        for (;;) { const auto a = left.Next(), b = right.Next(); if (a != b) return a < b; if (a < 0) return false; }
    }
    /** @summary Parses exact canonical decimal range. @param text Decimal input. @param signedValue Accept signed range. @param max Unsigned upper bound. @param result Exact bits. @return Valid range and spelling. */
    bool Decimal(std::string_view text, bool signedValue, std::uint64_t max, std::uint64_t& result) noexcept {
        const bool negative = !text.empty() && text.front() == '-';
        if (negative && !signedValue) return false;
        if (negative) text.remove_prefix(1);
        if (text.empty() || (text.size() > 1 && text.front() == '0') || (negative && text == "0")) return false;
        std::uint64_t magnitude{};
        for (char c : text) { if (c < '0' || c > '9' || magnitude > (max - static_cast<unsigned>(c - '0')) / 10) return false; magnitude = magnitude * 10 + static_cast<unsigned>(c - '0'); }
        if (signedValue && magnitude > (negative ? (std::uint64_t{1} << 63) : (std::uint64_t{1} << 63) - 1)) return false;
        result = negative ? std::uint64_t{0} - magnitude : magnitude; return true;
    }
    /** @summary Parses a bounded tagged value into preallocated immutable records. @param depth Tagged depth. @param keyOffset Map key offset. @param keySize Map key length. @return Success. */
    bool Value(unsigned depth, std::uint32_t keyOffset, std::uint32_t keySize) noexcept {
        if (depth > 16 || nodes_ == 4096) { status_ = Status::ResourceLimit; return false; }
        if (block_ && nodes_ >= block_->nodes) { Invalid(); return false; }
        const auto index = static_cast<std::uint32_t>(nodes_++);
        Node node{}; node.keyOffset = keyOffset; node.keySize = keySize;
        char tag[8]{}; std::size_t size{};
        if (!Eat('[') || !String(tag, sizeof tag, size, false)) return false;
        const std::string_view name(tag, size);
        if (name == "null") node.tag = Tag::Null;
        else {
            if (!Eat(',')) return false;
            if (name == "bool") {
                node.tag = Tag::Bool; const auto literal = Peek('t') ? std::string_view("true") : std::string_view("false");
                if (wire_.substr(pos_, literal.size()) != literal) { Invalid(); return false; }
                pos_ += literal.size();
                node.bits = literal == "true";
            } else if (name == "string") {
                node.tag = Tag::String; node.offset = static_cast<std::uint32_t>(text_);
                if (!String(nullptr, 65536, size, true)) return false; node.size = static_cast<std::uint32_t>(size);
            } else if (name == "list" || name == "map") {
                node.tag = name == "map" ? Tag::Map : Tag::List;
                if (!Eat('[')) return false;
                std::uint32_t previous{}; std::size_t previousKey{};
                while (!Peek(']')) {
                    if (node.count == (node.tag == Tag::Map ? 128u : 1024u)) { status_ = Status::ResourceLimit; return false; }
                    if (node.count && !Eat(',')) return false;
                    std::uint32_t offset{}, keyLength{};
                    if (node.tag == Tag::Map) {
                        if (!Eat('[')) return false;
                        Space(); const auto key = pos_ + 1; offset = static_cast<std::uint32_t>(text_);
                        if (!String(nullptr, 65536, size, true)) return false;
                        if (node.count && !Ascending(previousKey, key)) { Invalid(); return false; }
                        previousKey = key; keyLength = static_cast<std::uint32_t>(size);
                        if (!Eat(',')) return false;
                    }
                    const auto child = static_cast<std::uint32_t>(nodes_);
                    if (!Value(depth + 1, offset, keyLength)) return false;
                    if (!node.count) node.first = child;
                    else if (block_) Nodes(block_)[previous].next = child;
                    previous = child; ++node.count;
                    if (node.tag == Tag::Map && !Eat(']')) return false;
                }
                if (!Eat(']')) return false;
            } else if (name == "bytes") {
                node.tag = Tag::Bytes; node.offset = static_cast<std::uint32_t>(text_);
                if (!Eat('"')) return false;
                StringReader reader{wire_, pos_}; int high = -1;
                for (int c = reader.Next(); c >= 0; c = reader.Next()) {
                    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                    if (digit < 0) { Invalid(); return false; }
                    if (high < 0) high = digit;
                    else { if (node.size == 65536) { status_ = Status::ResourceLimit; return false; } if (block_) { if (text_ >= block_->textSize) { Invalid(); return false; } Texts(block_)[text_] = static_cast<char>(high * 16 + digit); } ++text_; ++node.size; high = -1; }
                }
                if (!reader.valid || !reader.done || high >= 0) { Invalid(); return false; } pos_ = reader.pos;
            } else if (name == "f64" || name == "u64" || name == "i64") {
                char number[32]{};
                if (!String(number, sizeof number, size, false)) return false;
                const std::string_view value(number, size);
                if (name == "f64") {
                    node.tag = Tag::F64; if (size != 16) { Invalid(); return false; }
                    for (char c : value) { const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (digit < 0) { Invalid(); return false; } node.bits = (node.bits << 4) | static_cast<unsigned>(digit); }
                    if (!std::isfinite(std::bit_cast<double>(node.bits))) { Invalid(); return false; }
                } else { node.tag = name == "u64" ? Tag::U64 : Tag::I64; if (!Decimal(value, node.tag == Tag::I64, UINT64_MAX, node.bits)) { Invalid(); return false; } }
            } else if (name == "ref") {
                node.tag = Tag::Ref; node.offset = static_cast<std::uint32_t>(text_); char kind[8]{};
                if (!String(kind, sizeof kind, size, true)) return false;
                const std::string_view type(kind, size);
                if (type != "runtime" && type != "region" && type != "city" && type != "entity") { Invalid(); return false; }
                node.size = static_cast<std::uint32_t>(size);
                for (unsigned i = 0; i != 2; ++i) {
                    char number[16]{}; std::uint64_t value{};
                    if (!Eat(',') || !String(number, sizeof number, size, false)) return false;
                    if (!Decimal({number, size}, false, UINT32_MAX, value) || !value) { Invalid(); return false; }
                    if (!i) node.bits = value; else node.generation = static_cast<std::uint32_t>(value);
                }
            } else { Invalid(); return false; }
        }
        if (!Eat(']')) return false;
        if (block_) Nodes(block_)[index] = node;
        return true;
    }
    std::string_view wire_, schema_;
    Block* block_{};
    std::size_t pos_{};
    Status status_{Status::Ok};
};
}

Reservation::Reservation(Reservation&& other) noexcept : memory_(std::exchange(other.memory_, nullptr)), size_(std::exchange(other.size_, 0)), owner_(other.owner_), state_(std::move(other.state_)) {}
Reservation& Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) { Reservation previous(std::move(*this)); memory_ = std::exchange(other.memory_, nullptr); size_ = std::exchange(other.size_, 0); owner_ = other.owner_; state_ = std::move(other.state_); }
    return *this;
}
Reservation::~Reservation() { if (size_) { ::operator delete(memory_); Release(*state_, owner_, size_); } }
std::span<std::byte> Reservation::Bytes() noexcept { return memory_ ? std::span<std::byte>(static_cast<std::byte*>(memory_), size_) : std::span<std::byte>{}; }
Envelope::Envelope(const Envelope& other) noexcept : block_(other.block_) { if (block_) block_->refs.fetch_add(1, std::memory_order_relaxed); }
Envelope::Envelope(Envelope&& other) noexcept : block_(std::exchange(other.block_, nullptr)) {}
Envelope& Envelope::operator=(Envelope other) noexcept { std::swap(block_, other.block_); return *this; }
Envelope::~Envelope() {
    if (block_ && block_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        auto state = block_->state; const auto charge = block_->charge; const auto owner = block_->owner;
        block_->~Block(); ::operator delete(block_); Release(*state, owner, charge);
    }
}
std::string_view Envelope::Encode() const noexcept { return block_ ? std::string_view(Texts(block_) + block_->textSize, block_->wireSize) : std::string_view{}; }
std::string_view Envelope::Schema() const noexcept { return block_ ? std::string_view(Texts(block_) + block_->schemaOffset, block_->schemaSize) : std::string_view{}; }
const Node* Envelope::At(std::uint32_t index) const noexcept { return block_ && index < block_->nodes ? Nodes(block_) + index : nullptr; }
std::string_view Envelope::Text(const Node& node) const noexcept { return block_ && node.offset <= block_->textSize && node.size <= block_->textSize - node.offset ? std::string_view(Texts(block_) + node.offset, node.size) : std::string_view{}; }
std::string_view Envelope::Key(const Node& node) const noexcept { return block_ && node.keyOffset <= block_->textSize && node.keySize <= block_->textSize - node.keyOffset ? std::string_view(Texts(block_) + node.keyOffset, node.keySize) : std::string_view{}; }
std::size_t Envelope::Charge() const noexcept { return block_ ? block_->charge : 0; }
std::uint64_t Envelope::Owner() const noexcept { return block_ ? block_->owner : 0; }
Status Storage::Allocate(std::uint64_t owner, std::size_t bytes, Reservation& output) const noexcept {
    output = {};
    if (!owner || !bytes) return Status::Invalid;
    if (!Reserve(*state_, owner, bytes)) return Status::ResourceLimit;
    auto* memory = ::operator new(bytes, std::nothrow);
    if (!memory) { Release(*state_, owner, bytes); return Status::ResourceLimit; }
    output.memory_ = memory; output.size_ = bytes; output.owner_ = owner; output.state_ = state_;
    return Status::Ok;
}
Status Storage::Account(std::uint64_t owner, std::size_t bytes, Reservation& output) const noexcept {
    output = {};
    if (!owner || !bytes) return Status::Invalid;
    if (!Reserve(*state_, owner, bytes)) return Status::ResourceLimit;
    output.size_ = bytes; output.owner_ = owner; output.state_ = state_;
    return Status::Ok;
}
std::size_t Storage::DecodeCapacity(std::size_t wireLimit) noexcept {
    return wireLimit <= EnvelopeLimit ? sizeof(Block) + 4096 * sizeof(Node) + 2 * wireLimit : 0;
}
bool Storage::Owns(const Envelope& value, std::uint64_t owner) const noexcept {
    return value.block_ && value.block_->state == state_ && value.block_->owner == owner;
}
Status Storage::DecodeReserved(std::string_view wire, std::string_view schema, Reservation& reservation, Envelope& output, std::size_t methodLimit) const noexcept {
    Envelope previous(std::move(output));
    if (!reservation.memory_ || reservation.state_ != state_ || !reservation.owner_ || schema.empty() || schema.size() > 65536) return Status::Invalid;
    if (wire.size() > EnvelopeLimit || wire.size() > methodLimit) return Status::ResourceLimit;
    // @summary Arena writes cannot overlap either borrowed input; pointer comparisons use integer address distances.
    const auto overlaps = [&](std::string_view input) noexcept {
        if (input.empty()) return false;
        const auto start = reinterpret_cast<std::uintptr_t>(input.data());
        const auto arena = reinterpret_cast<std::uintptr_t>(reservation.memory_);
        return start >= arena ? start - arena < reservation.size_ : arena - start < input.size();
    };
    if (overlaps(wire) || overlaps(schema)) return Status::Invalid;
    Parser count(wire, schema); const auto status = count.Parse(); if (status != Status::Ok) return status;
    const auto required = sizeof(Block) + count.nodes_ * sizeof(Node) + count.text_ + wire.size();
    if (required > reservation.size_) return Status::ResourceLimit;
    auto* block = new (reservation.memory_) Block;
    block->state = state_; block->charge = reservation.size_; block->wireSize = wire.size(); block->textSize = count.text_; block->owner = reservation.owner_; block->nodes = static_cast<std::uint32_t>(count.nodes_);
    for (std::size_t i = 0; i < count.nodes_; ++i) new (Nodes(block) + i) Node;
    Parser write(wire, schema, block); const auto written = write.Parse();
    if (written != Status::Ok || write.nodes_ != count.nodes_ || write.text_ != count.text_) { block->~Block(); return Status::Invalid; }
    std::memcpy(Texts(block) + count.text_, wire.data(), wire.size());
    reservation.memory_ = nullptr; reservation.size_ = 0; reservation.state_.reset();
    output = Envelope(block); return Status::Ok;
}
Storage::Storage() : state_(std::make_shared<State>()) {}
Status Storage::Decode(std::string_view wire, std::string_view schema, std::uint64_t owner, Envelope& output, std::size_t methodLimit) const noexcept {
    Envelope previous(std::move(output));
    if (wire.size() > EnvelopeLimit || wire.size() > methodLimit) return Status::ResourceLimit;
    if (!owner || schema.empty() || schema.size() > 65536) return Status::Invalid;
    Parser count(wire, schema); const auto status = count.Parse(); if (status != Status::Ok) return status;
    const auto charge = sizeof(Block) + count.nodes_ * sizeof(Node) + count.text_ + wire.size();
    if (!Reserve(*state_, owner, charge)) return Status::ResourceLimit;
    auto* memory = ::operator new(charge, std::nothrow);
    if (!memory) { Release(*state_, owner, charge); return Status::ResourceLimit; }
    auto* block = new (memory) Block;
    block->state = state_; block->charge = charge; block->wireSize = wire.size(); block->textSize = count.text_; block->owner = owner; block->nodes = static_cast<std::uint32_t>(count.nodes_);
    for (std::size_t i = 0; i < count.nodes_; ++i) new (Nodes(block) + i) Node;
    Parser write(wire, schema, block);
    const auto written = write.Parse();
    if (written != Status::Ok || write.nodes_ != count.nodes_ || write.text_ != count.text_) { Envelope cleanup(block); return Status::Invalid; }
    std::memcpy(Texts(block) + count.text_, wire.data(), wire.size()); output = Envelope(block); return Status::Ok;
}
std::size_t Storage::Used() const noexcept { std::lock_guard lock(state_->mutex); return state_->used; }
std::size_t Storage::Used(std::uint64_t owner) const noexcept { std::lock_guard lock(state_->mutex); for (const auto& entry : state_->owners) if (entry.id == owner) return entry.used; return 0; }
}
