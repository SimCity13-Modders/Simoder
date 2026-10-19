#include "mods/mod_manager.hpp"
#include "mods/owned_publication.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "mod_fixture.hpp"
#include "test_harness.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using sc13::mods::ModManager;
using Access = sc13::mods::ManagerLifetimeTestAccess;
using sc13::runtime::ScopeResult;
using namespace std::chrono_literals;
std::atomic_uint checks{};
/** @summary Requires an observed behavioral result. @param value Actual result. @param message Failure diagnostic. */
void Check(bool value, const char* message) {
    ++checks; if (!value) throw std::runtime_error(message);
}
/** @summary Owns ordinary local fixtures, never a game or an operating-system VM. */
struct Fixture final {
    std::filesystem::path root = sc13::tests::CreateTemporaryDirectory();
    std::unique_ptr<sc13::mods::PatchRegistry> registry = std::make_unique<sc13::mods::PatchRegistry>();
    std::unique_ptr<sc13::runtime::RuntimeResourceCache> cache = std::make_unique<sc13::runtime::RuntimeResourceCache>();
    std::shared_ptr<sc13::runtime::RuntimeCommitDomain> domain = std::make_shared<sc13::runtime::RuntimeCommitDomain>();
    std::unique_ptr<ModManager> manager;
    /** @summary Discovers one real canonical entry through the unchanged public legacy initializer. */
    Fixture() {
        Write();
        manager = std::make_unique<ModManager>(root / "mods", root / "state.toon", *registry, *cache,
            sc13::mods::RuntimeResourceRefreshCallback{}, sc13::scripting::ScriptScheduler::LogSink{}, nullptr, domain);
        std::string error; Check(manager->Initialize(error), "Actual manager initialization");
        Check(manager->Snapshot().size() == 1, "Actual canonical map discovery");
    }
    /** @summary Releases native owner before its real borrowed resources and fixture files. */
    ~Fixture() {
        manager.reset(); cache.reset(); registry.reset();
        std::error_code error; std::filesystem::remove_all(root, error);
    }
    /** @summary Recreates exactly the same textual owner through real filesystem discovery. */
    void Write() { sc13::tests::WriteModFixture(root / "mods" / "one", "fixture.owner", "Lifetime", 5); }
};
/** @summary Checks actual mutex contention, sole stamp replacement and erased/recreated entry identity. */
void Membership() {
    Fixture fixture;
    auto plan = Access::Capture(*fixture.manager, "fixture.owner");
    Check(Access::Validate(plan) == ScopeResult::Ok, "Actual entry accepted");
    {
        auto lock = Access::HoldManager(*fixture.manager);
        auto result = std::async(std::launch::async, [&] { return Access::Validate(plan); });
        Check(result.wait_for(1s) == std::future_status::ready, "M conflict returns without waiting");
        Check(result.get() == ScopeResult::Busy, "Actual manager-wide M is reserved");
    }
    Check(Access::ReplaceStamp(*fixture.manager, "fixture.owner"), "Actual participant stamp replacement");
    Check(Access::Validate(plan) == ScopeResult::Stale, "Queued old-stamp observation rejected");
    plan = Access::Capture(*fixture.manager, "fixture.owner");
    std::filesystem::remove_all(fixture.root / "mods" / "one");
    std::string error;
    Check(fixture.manager->Refresh(error), "Actual entry erase through discovery");
    Check(Access::Validate(plan) == ScopeResult::Stale, "Erased actual membership rejected");
    fixture.Write(); Check(fixture.manager->Refresh(error), "Same textual key recreated");
    Check(Access::Validate(plan) == ScopeResult::Stale, "Same-ID entry ABA rejected");
    Check(Access::Validate(Access::Capture(*fixture.manager, "fixture.owner")) == ScopeResult::Ok,
        "Fresh actual entry accepted");
    fixture.manager->RequestRuntimeStop();
    Check(fixture.manager->RuntimeScopesDrained(), "Runtime drain remains callable after runtime stop");
}
/** @summary Pauses one admitted operation while an external owner closes and drains actual borrowed storage.
 * @param reader True selects an actual public reader; false selects the real ScriptHost callback. */
void BorrowedLifetime(bool reader) {
    Fixture fixture;
    auto plan = Access::Capture(*fixture.manager, "fixture.owner");
    auto host = Access::Host(*fixture.manager);
    auto sink = Access::Sink(*fixture.manager);
    auto actualScope = Access::AttachScope(*fixture.manager, "fixture.owner");
    Check(!actualScope.expired(), "Actual record retains its native scope");
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future().share();
    auto* manager = fixture.manager.get();
    auto operation = std::async(std::launch::async, [&] {
        /** @summary Bound a fixture-only pause before real borrowed access; no publication lock is held. */
        auto barrier = [&] {
            entered.set_value();
            if (released.wait_for(5s) != std::future_status::ready) throw std::runtime_error("Lease pause timeout");
        };
        if (reader) {
            Access::ReaderBarrier(barrier);
            static_cast<void>(manager->RuntimeObservations());
            Access::ReaderBarrier({});
        } else {
            Access::CallbackBarrier(barrier);
            static_cast<void>(host.readFloat({1, 2, 3}, 4));
            Access::CallbackBarrier({});
        }
    });
    Check(enteredFuture.wait_for(2s) == std::future_status::ready, "Actual admitted operation reached borrowed-access pause");
    std::atomic_bool resourcesDestroyed{};
    auto owner = std::async(std::launch::async, [&] {
        fixture.manager.reset();
        fixture.cache.reset(); fixture.registry.reset(); resourcesDestroyed.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!Access::Closed(plan) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    Check(Access::Closed(plan), "External teardown closed native admission");
    Check(!resourcesDestroyed.load(), "Borrowed resources retained while actual operation is admitted");
    Check(owner.wait_for(20ms) == std::future_status::timeout, "Destructor drains admitted native operation");
    Check(Access::ManagerAvailable(plan), "Drain holds no actual manager M");
    {
        auto writer = fixture.domain->ReserveWriter();
        Check(writer.Acquired(), "Drain holds no original domain D");
    }
    Check(Access::Validate(plan) == ScopeResult::Cancelled, "Retained plan denies before borrowed access after close");
    Check(!host.readFloat({1, 2, 3}, 4), "Copied ScriptHost is revoked during close");
    std::string error;
    Check(!sink("fixture.owner", {}, {}, error), "Copied actual scheduler sink revoked during close");
    Check(error == "Mod manager is closed", "Revoked sink diagnostic");
    release.set_value(); operation.get();
    Check(owner.wait_for(2s) == std::future_status::ready, "Teardown completes after operation lease release");
    owner.get();
    Check(resourcesDestroyed.load(), "Real borrowed resources destroyed only after drain");
    Check(actualScope.expired(), "Closed retained plan does not keep actual record scope alive");
    Check(Access::Validate(plan) == ScopeResult::Cancelled, "Retained plan safe after manager and borrowed storage destruction");
    Check(!host.readFloat({1, 2, 3}, 4), "Retained callback denies after borrowed storage destruction");
    Check(!sink("fixture.owner", {}, {}, error), "Retained sink denies after manager destruction");
}
/** @summary Checks that copied callbacks weakly retain the anchor without a map/VM/callback ownership cycle. */
void ExpiredCallbacks() {
    sc13::scripting::ScriptHost host;
    sc13::scripting::ScriptScheduler::CommandSink sink;
    std::weak_ptr<const void> anchor;
    {
        Fixture fixture;
        host = Access::Host(*fixture.manager); sink = Access::Sink(*fixture.manager);
        anchor = Access::Anchor(Access::Capture(*fixture.manager, "fixture.owner"));
    }
    Check(anchor.expired(), "Weak callbacks do not create a retained map/host/state ownership cycle");
    Check(!host.readFloat({1, 2, 3}, 4), "Weak callback denies when no retained state remains");
    std::string error;
    Check(!sink("fixture.owner", {}, {}, error), "Weak sink denies after anchor expiration");
}
}

/** @summary Runs isolated actual-manager lifetime and participant tests. @return Zero only after every oracle passes. */
int main() {
    try {
        Membership(); BorrowedLifetime(false); BorrowedLifetime(true); ExpiredCallbacks();
        Check(sc13::tests::FailureCount() == 0, "Shared fixture setup checks");
        std::cout << checks.load() << " manager lifetime checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
