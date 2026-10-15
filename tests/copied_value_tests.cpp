#include "runtime/copied_value.hpp"
#include <atomic>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace sc13::runtime::copied;
namespace allocation_probe {
std::atomic_bool fail{};
std::atomic_size_t attempts{};
}
/** @summary Test-only deterministic payload allocation interception; no production failure control. @param size Requested bytes. @return Allocation or null under injected failure. */
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    ++allocation_probe::attempts;
    if (allocation_probe::fail.exchange(false)) return nullptr;
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
/** @summary Matches the test-only nothrow allocation overload. @param memory Allocation to release. */
void operator delete(void* memory, const std::nothrow_t&) noexcept { ::operator delete(memory); }
namespace {
/** @summary Asserts a behavioral condition. @param condition Observed result. @param message Failure explanation. */
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
/** @summary Wraps a tagged fixture in its reviewed schema. @param value Tagged JSON. @return Test envelope. */
std::string Wire(std::string_view value) { return "{\"v\":1,\"schema\":\"test.echo@1\",\"value\":" + std::string(value) + "}"; }
/** @summary Checks exact value semantics and allocation-free retained encoding. */
void Values() {
    Storage storage;
    const std::vector<std::string> values{
        R"(["null"])", R"(["bool",true])", R"(["bool",false])", R"(["string","a\uD83D\uDE00\n"])",
        R"(["f64","8000000000000000"])", R"(["f64","7fefffffffffffff"])", R"(["u64","18446744073709551615"])",
        R"(["i64","-9223372036854775808"])", R"(["i64","9223372036854775807"])", R"(["bytes","0001ff"])",
        R"(["list",[["null"],["string",""]]])", R"(["map",[["",["null"]],["a",["list",[]]],["é",["bool",true]]]])",
        R"(["ref","entity","4294967295","4294967295"])", R"(["ref","runtime","1","1"])", R"(["map",[]])"};
    for (const auto& value : values) {
        const auto wire = Wire(value); Envelope envelope;
        Require(storage.Decode(wire, "test.echo@1", 1, envelope) == Status::Ok, "valid value denied");
        Require(envelope.Encode() == wire && envelope.Schema() == "test.echo@1", "wire/schema changed");
        Require(envelope.At(0) && !envelope.At(4096), "node index bounds");
        Require(storage.Used() == envelope.Charge(), "charge mismatch");
        auto retained = envelope; envelope = {}; Require(storage.Used() == retained.Charge(), "early release");
        Envelope second; Require(storage.Decode(retained.Encode(), "test.echo@1", 1, second) == Status::Ok, "round trip denied");
        Require(storage.Used() == retained.Charge() + second.Charge(), "simultaneous copy uncharged");
    }
    Require(!storage.Used(), "final lease not released");
    Envelope exact;
    Require(storage.Decode(Wire(R"(["u64","18446744073709551615"])"), "test.echo@1", 1, exact) == Status::Ok && exact.At(0)->bits == UINT64_MAX, "u64 precision");
    Require(storage.Decode(Wire(R"(["f64","8000000000000000"])"), "test.echo@1", 1, exact) == Status::Ok && exact.At(0)->bits == (std::uint64_t{1} << 63), "signed zero lost");
    Require(storage.Decode(Wire(R"(["map",[["a",["string","x"]],["b",["list",[["null"]]]]]])"), "test.echo@1", 1, exact) == Status::Ok, "nested model denied");
    const auto* first = exact.At(exact.At(0)->first);
    Require(first && exact.Key(*first) == "a" && exact.Text(*first) == "x" && exact.Key(*exact.At(first->next)) == "b", "flat model child/key links");
    Envelope survivor;
    { Storage transient; Require(transient.Decode(Wire(R"(["null"])"), "test.echo@1", 2, survivor) == Status::Ok, "transient pool failed"); }
    Require(survivor.At(0)->tag == Tag::Null, "pool lifetime lost");
}
/** @summary Rejects malformed, ambiguous and authority-shaped values with empty outputs. */
void Invalid() {
    Storage storage; Envelope output;
    const std::vector<std::string> invalid{
        R"(["bool",t r u e])", R"(["bool",1])", R"(["string","\u0000"])", R"(["string","\uD800"])", R"(["string","\uDC00"])",
        R"(["f64","7ff0000000000000"])", R"(["f64","fff8000000000000"])", R"(["f64","800000000000000A"])",
        R"(["u64","18446744073709551616"])", R"(["i64","9223372036854775808"])", R"(["i64","-9223372036854775809"])",
        R"(["u64","-1"])", R"(["i64","-0"])", R"(["u64","01"])", R"(["u64","+1"])",
        R"(["unknown-tag-too-long"])", R"(["u64","0000000000000000000000000000000000000000"])",
        R"(["bytes","0"])", R"(["bytes","AA"])", R"(["map",[["a",["null"]],["\u0061",["null"]]]])",
        R"(["map",[["b",["null"]],["a",["null"]]]])", R"(["ref","invocation","1","1"])", R"(["ref","city","0","1"])",
        R"(["ref","city","1","4294967296"])", R"(["null",0])", R"(["facade"])", R"(["list",[null]])", R"(["map",[["a",["null"],0]]])"};
    for (const auto& value : invalid) {
        Require(storage.Decode(Wire(R"(["null"])"), "test.echo@1", 1, output) == Status::Ok, "seed failed");
        Require(storage.Decode(Wire(value), "test.echo@1", 1, output) == Status::Invalid && !output && !storage.Used(), "invalid accepted/output retained");
    }
    const std::vector<std::string> envelopes{
        R"({"v":1.0,"schema":"test.echo@1","value":["null"]})", R"({"v":1e0,"schema":"test.echo@1","value":["null"]})",
        R"({"v":1,"v":1,"value":["null"]})", R"({"v":1,"\u0076":1,"value":["null"]})",
        R"({"v":1,"schema":"other","value":["null"]})", R"({"v":1,"schema":"test.echo@1","value":["null"],"extra":0})",
        R"({"v":1,"schema":"test.echo@1"})", Wire(R"(["null"])") + "x"};
    for (const auto& wire : envelopes) Require(storage.Decode(wire, "test.echo@1", 1, output) == Status::Invalid && !output, "invalid envelope accepted");
    for (const auto raw : { std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string(1, '\0') })
        Require(storage.Decode(Wire("[\"string\",\"" + raw + "\"]"), "test.echo@1", 1, output) == Status::Invalid, "invalid UTF8/NUL accepted");
    const auto shuffled = R"( { "value" : ["null"], "schema":"test.echo@1", "v":1 } )";
    Require(storage.Decode(shuffled, "test.echo@1", 1, output) == Status::Ok, "key order/whitespace rejected");
}
/** @summary Verifies inclusive and exceeded contract bounds and simultaneous quota exhaustion. */
void Bounds() {
    Storage storage; Envelope value;
    auto decode = [&](const std::string& wire) { return storage.Decode(wire, "test.echo@1", 1, value); };
    Require(decode(Wire("[\"string\",\"" + std::string(65536, 'x') + "\"]")) == Status::Ok, "string inclusive limit");
    Require(decode(Wire("[\"string\",\"" + std::string(65537, 'x') + "\"]")) == Status::ResourceLimit, "string excess");
    Require(decode(Wire("[\"bytes\",\"" + std::string(131072, 'a') + "\"]")) == Status::Ok, "bytes inclusive limit");
    Require(decode(Wire("[\"bytes\",\"" + std::string(131074, 'a') + "\"]")) == Status::ResourceLimit, "bytes excess");
    std::string nested = R"(["null"])";
    for (unsigned i = 1; i < 16; ++i) nested = "[\"list\",[" + nested + "]]";
    Require(decode(Wire(nested)) == Status::Ok, "depth inclusive");
    Require(decode(Wire("[\"list\",[" + nested + "]]")) == Status::ResourceLimit, "depth excess");
    std::string list = "[\"list\",[";
    for (unsigned i = 0; i < 1024; ++i) { if (i) list += ','; list += R"(["null"])"; }
    Require(decode(Wire(list + "]]")) == Status::Ok, "list inclusive");
    Require(decode(Wire(list + R"(,["null"]]])")) == Status::ResourceLimit, "list excess");
    std::string map = "[\"map\",[";
    for (unsigned i = 0; i < 128; ++i) { if (i) map += ','; map += "[\"" + std::to_string(1000 + i) + "\",[\"null\"]]"; }
    Require(decode(Wire(map + "]]")) == Status::Ok, "map inclusive");
    Require(decode(Wire(map + R"(,["9999",["null"]]]])")) == Status::ResourceLimit, "map excess");
    Require(decode(Wire("[\"map\",[[\"" + std::string(65536, 'x') + "\",[\"null\"]]]]")) == Status::Ok, "key inclusive");
    Require(decode(Wire("[\"map\",[[\"" + std::string(65537, 'x') + "\",[\"null\"]]]]")) == Status::ResourceLimit, "key excess");
    auto nodesFixture = [](unsigned last) {
        std::string root = "[\"list\",[";
        for (unsigned group = 0; group < 4; ++group) {
            if (group) root += ','; root += "[\"list\",[";
            const auto count = group == 3 ? last : 1024;
            for (unsigned i = 0; i < count; ++i) { if (i) root += ','; root += R"(["null"])"; }
            root += "]]";
        }
        return Wire(root + "]]");
    };
    Require(decode(nodesFixture(1019)) == Status::Ok && value.At(4095) && !value.At(4096), "node inclusive");
    Require(decode(nodesFixture(1020)) == Status::ResourceLimit, "node excess");
    auto wire = Wire(R"(["null"])"); wire.insert(0, Storage::EnvelopeLimit - wire.size(), ' ');
    Require(decode(wire) == Status::Ok, "envelope inclusive");
    Require(decode(wire + ' ') == Status::ResourceLimit, "envelope excess");
    Require(storage.Decode(Wire(R"(["null"])"), "test.echo@1", 1, value, 1) == Status::ResourceLimit && !value, "method tighter limit");
    Require(decode(Wire(R"(["string","alias"] )")) == Status::Ok, "alias seed");
    Require(storage.Decode(value.Encode(), value.Schema(), 1, value) == Status::Ok && value.Text(*value.At(0)) == "alias", "aliased input/output lifetime");
    value = {};
    std::vector<Envelope> retained;
    const auto large = Wire("[\"string\",\"" + std::string(65536, 'x') + "\"]");
    for (unsigned i = 0; i < 100; ++i) { Envelope current; const auto status = storage.Decode(large, "test.echo@1", 1, current); if (status == Status::ResourceLimit) break; Require(status == Status::Ok, "quota fixture invalid"); retained.push_back(std::move(current)); }
    Require(!retained.empty() && storage.Used(1) <= Storage::OwnerLimit && storage.Used(1) + retained.front().Charge() > Storage::OwnerLimit, "owner quota bound");
    retained.clear(); Require(!storage.Used(), "owner quota release");
    for (unsigned i = 1; i <= 256; ++i) { Envelope current; Require(storage.Decode(Wire(R"(["null"])"), "test.echo@1", i, current) == Status::Ok, "owner table inclusive"); retained.push_back(std::move(current)); }
    Require(storage.Decode(Wire(R"(["null"])"), "test.echo@1", 257, value) == Status::ResourceLimit, "owner table excess");
    retained.clear(); Require(!storage.Used(), "owner table recycle");
    for (unsigned owner = 1; owner < 20; ++owner) {
        for (unsigned i = 0; i < 100; ++i) { Envelope current; const auto status = storage.Decode(large, "test.echo@1", owner, current); if (status == Status::ResourceLimit) break; Require(status == Status::Ok, "global fixture invalid"); retained.push_back(std::move(current)); }
    }
    Require(storage.Used() <= Storage::GlobalLimit && storage.Used() + retained.front().Charge() > Storage::GlobalLimit, "global quota bound");
    retained.clear(); Require(!storage.Used(), "global release");
}
/** @summary Exercises atomic owner/global reservations and intrusive lease destruction across threads. */
void Concurrent() {
    Storage storage; const auto wire = Wire("[\"string\",\"" + std::string(65536, 'x') + "\"]");
    std::atomic_bool failed{}; std::vector<std::thread> workers;
    for (unsigned t = 0; t < 12; ++t) workers.emplace_back([&, t] {
        for (unsigned i = 0; i < 100; ++i) { Envelope value; const auto status = storage.Decode(wire, "test.echo@1", t + 1, value); if (status == Status::Invalid) failed = true; if (value) { auto copy = value; if (copy.Encode() != wire) failed = true; } }
    });
    for (auto& worker : workers) worker.join();
    Require(!failed && !storage.Used(), "concurrent reservation/lease failure");
}
/** @summary Proves invalid input never reaches payload allocation and failed allocation releases its reservation. */
void AllocationFailure() {
    Storage storage; Envelope value;
    const auto valid = Wire(R"(["null"])"); const auto invalid = Wire(R"(["bad"])");
    allocation_probe::attempts = 0;
    Require(storage.Decode(invalid, "test.echo@1", 1, value) == Status::Invalid && allocation_probe::attempts == 0, "preflight allocated payload");
    allocation_probe::fail = true;
    Require(storage.Decode(valid, "test.echo@1", 1, value) == Status::ResourceLimit && !value && !storage.Used() && allocation_probe::attempts == 1, "failed allocation leaked quota");
    Require(storage.Decode(valid, "test.echo@1", 1, value) == Status::Ok && storage.Used() == value.Charge(), "failed allocation poisoned pool");
}
/** @summary Verifies physical result arenas, shared ownership, accounting-only leases and allocation-free decode. */
void ReservedStorage() {
    Storage storage, foreign;
    const auto wire = Wire(R"(["u64","18446744073709551615"])");
    Reservation arena;
    const auto capacity = Storage::DecodeCapacity(wire.size());
    Require(storage.Allocate(17, capacity, arena) == Status::Ok && arena.Bytes().size() == capacity && storage.Used(17) == capacity, "physical arena reservation");
    Envelope output;
    Require(foreign.DecodeReserved(wire, "test.echo@1", arena, output) == Status::Invalid && arena && !output, "foreign reservation");
    Require(storage.DecodeReserved(Wire(R"(["bad"])"), "test.echo@1", arena, output) == Status::Invalid && arena && !output, "invalid leaves reservation");
    allocation_probe::attempts = 0;
    Require(storage.DecodeReserved(wire, "test.echo@1", arena, output) == Status::Ok && !arena && output.At(0)->bits == UINT64_MAX && allocation_probe::attempts == 0, "reserved decode allocated");
    Require(output.Charge() == capacity && output.Owner() == 17 && storage.Owns(output, 17) && !storage.Owns(output, 18) && !foreign.Owns(output, 17), "charged arena identity");
    auto retained = output; output = {}; Require(storage.Used(17) == capacity, "shared reserved lease");
    retained = {}; Require(!storage.Used(), "reserved final release");
    Reservation account;
    Require(storage.Account(17, 1234, account) == Status::Ok && account && account.Bytes().empty() && storage.Used() == 1234, "external capacity accounting");
    Require(storage.DecodeReserved(wire, "test.echo@1", account, output) == Status::Invalid, "accounting lease is not arena");
    account = {}; Require(!storage.Used(), "account release");
    Require(storage.Allocate(17, capacity, arena) == Status::Ok, "alias arena");
    const auto bytes = arena.Bytes();
    const std::string_view alias(reinterpret_cast<const char*>(bytes.data()), wire.size());
    Require(storage.DecodeReserved(alias, "test.echo@1", arena, output) == Status::Invalid && arena, "arena wire alias");
    arena = {};
    Require(storage.Allocate(17, 1, arena) == Status::Ok && storage.DecodeReserved(wire, "test.echo@1", arena, output) == Status::ResourceLimit && arena, "too small arena");
    arena = {}; Require(!storage.Used(), "failed reserved decode cleanup");
}
}
/** @summary Runs private codec conformance and adversarial storage tests. @return Zero on success. */
int main() { try { Values(); Invalid(); Bounds(); Concurrent(); AllocationFailure(); ReservedStorage(); std::cout << "copied-value conformance PASS\n"; return 0; } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
