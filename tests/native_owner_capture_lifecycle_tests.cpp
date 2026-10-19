#include "dev/session/owner_capture_lifecycle.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using sc13::dev::session::OwnerCaptureLifecycle;
using sc13::dev::session::OwnerCaptureObservation;
using sc13::dev::session::OwnerHookOperations;
using sc13::dev::session::OwnerHookResult;
using Kind = OwnerHookResult::Kind;
/** @summary Fail without accepting a partial transition or inventing native execution evidence. */
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
/** @summary Supply fake hook-control outcomes while using actual production lifecycle logic. */
struct Fixture final {
    sc13::hooks::RuntimeHookActivity activity;
    OwnerCaptureLifecycle owner{activity};
    Kind createKind{Kind::Success};
    Kind enableKind{Kind::Success};
    Kind disableKind{Kind::Success};
    bool nullOriginal{};
    bool unexpectedOriginalOnFailure{};
    int creates{};
    int enables{};
    int disables{};
    std::atomic_int forwards{};
    std::array<std::uintptr_t, 4> arguments{};
    std::function<void()> onEnable;
    std::function<void()> onForward;
    /** @summary Open only the existing shared runtime activity before installing its synthetic owner. */
    Fixture() { Require(activity.Open(), "fixture activity did not open"); }
    /** @summary Return this fixture's control adapter, containing no remove/uninitialize operation. */
    OwnerHookOperations Operations() noexcept { return {this, &Create, &Enable, &Disable}; }
    /** @summary Return the stable synthetic original address used only inside this host process. */
    static std::uintptr_t OriginalAddress() noexcept { return reinterpret_cast<std::uintptr_t>(&Forward); }
    /** @summary Model actual Create outcomes without patching a process instruction. */
    static OwnerHookResult Create(void* context, std::uintptr_t& original) noexcept {
        auto& fixture = *static_cast<Fixture*>(context); ++fixture.creates;
        if ((fixture.createKind == Kind::Success && !fixture.nullOriginal) || fixture.unexpectedOriginalOnFailure) original = OriginalAddress();
        return {fixture.createKind, 101, "synthetic create"};
    }
    /** @summary Inspect original publication at the exact point the lifecycle would expose its detour. */
    static OwnerHookResult Enable(void* context) noexcept {
        auto& fixture = *static_cast<Fixture*>(context); ++fixture.enables;
        if (fixture.onEnable) fixture.onEnable();
        return {fixture.enableKind, 102, "synthetic enable"};
    }
    /** @summary Report actual selected fake disable outcome while retaining original code. */
    static OwnerHookResult Disable(void* context) noexcept {
        auto& fixture = *static_cast<Fixture*>(context); ++fixture.disables;
        return {fixture.disableKind, 103, "synthetic disable"};
    }
    /** @summary Verify every original argument survives normal, in-flight and late forwarding. */
    static void Forward(void* self, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) {
        auto& fixture = *static_cast<Fixture*>(self); fixture.arguments = {a,b,c,d}; ++fixture.forwards;
        if (fixture.onForward) fixture.onForward();
    }
    /** @summary Enter the same production activity owner used by the native adapter, then invoke its immutable original. */
    void Invoke() {
        const sc13::hooks::RuntimeHookActivity::Entry entry(owner.Activity());
        using Callback = void(*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
        reinterpret_cast<Callback>(owner.Original())(this, 11, 22, 33, 44);
    }
    /** @summary Initialize only the synthetic exact target, never a real game address. */
    OwnerCaptureLifecycle::Outcome Start() { return owner.Initialize(1, Operations()); }
};
/** @summary Release a controlled original before jthread destruction even when an assertion throws. */
struct Release final {
    std::promise<void>& gate;
    bool released{};
    /** @summary Set one fixture gate exactly once. */
    void Now() { if (!released) { gate.set_value(); released = true; } }
    /** @summary Preserve original task ownership during failing fixture unwind. */
    ~Release() { if (!released) { try { gate.set_value(); } catch (...) {} } }
};
/** @summary Verify successful publication precedes exposure and original survives both stop and refused reinitialization. */
void SuccessAndRetirement() {
    Fixture fixture;
    bool published{};
    fixture.onEnable = [&] { published = fixture.owner.Original() == Fixture::OriginalAddress(); };
    Require(fixture.Start().ready && published, "original not published before enable");
    Require(fixture.Start().ready && fixture.creates == 1 && fixture.enables == 1, "active duplicate initialization recreated hook");
    fixture.Invoke();
    Require(fixture.arguments == std::array<std::uintptr_t,4>{11,22,33,44} && fixture.forwards == 1, "original arguments/forward count changed");
    fixture.owner.CloseObservation(); fixture.activity.Close();
    Require(fixture.owner.Retire().stopped, "owned disable not acknowledged");
    Require(fixture.owner.Original() == Fixture::OriginalAddress(), "retirement cleared original");
    Require(!fixture.Start().ready && fixture.creates == 1, "retired owner recreated");
    fixture.disableKind = Kind::AlreadyDisabled;
    Require(fixture.owner.Retire().stopped, "exact owned already-disabled outcome rejected");
}
/** @summary Cover declined, foreign and inconsistent Create outputs without any enable or ownership adoption. */
void CreateFailures() {
    for (auto kind : {Kind::Failure, Kind::Foreign}) {
        Fixture fixture; fixture.createKind = kind;
        const auto result = fixture.Start();
        Require(!result.ready && fixture.enables == 0 && fixture.owner.Original() == 0, "failed create exposed original");
        Require(!fixture.Start().ready && fixture.creates == 1, "declined create was retried");
        const auto stopped = fixture.owner.Retire();
        Require(stopped.stopped == (kind == Kind::Failure) && fixture.disables == 0, "foreign ownership was adopted or removed");
    }
    Fixture inconsistent; inconsistent.unexpectedOriginalOnFailure = true; inconsistent.createKind = Kind::Failure;
    Require(!inconsistent.Start().ready && !inconsistent.owner.Retire().stopped && inconsistent.owner.Original() == 0,
        "inconsistent failed create was adopted");
    Fixture missing; missing.nullOriginal = true;
    Require(!missing.Start().ready && missing.enables == 0 && missing.owner.Original() == 0, "null original was enabled");
    Require(!missing.Start().ready && missing.creates == 1, "quarantined null original recreated");
    Fixture unexpected; unexpected.createKind = Kind::AlreadyDisabled;
    Require(!unexpected.Start().ready && !unexpected.owner.Retire().stopped && unexpected.enables == 0,
        "unexpected Create status was treated as known no-hook ownership");
    Fixture badTarget;
    Require(!badTarget.owner.Initialize(0, badTarget.Operations()).ready && badTarget.creates == 0,
        "invalid target crossed hook creation");
}
/** @summary Keep both actual failed-enable and failed-cleanup evidence, without clearing original or replaying initialization. */
void EnableAndDisableFailures() {
    for (auto disable : {Kind::Success, Kind::AlreadyDisabled, Kind::Failure, Kind::Foreign}) {
        Fixture fixture; fixture.enableKind = Kind::Failure; fixture.disableKind = disable;
        const auto result = fixture.Start();
        Require(!result.ready && result.cleanupAttempted && result.result.nativeStatus == 102 && result.cleanup.nativeStatus == 103,
            "enable or cleanup status lost");
        Require(result.stopped == (disable == Kind::Success || disable == Kind::AlreadyDisabled), "failed disable reported stopped");
        Require(fixture.owner.Original() == Fixture::OriginalAddress() && fixture.disables == 1, "failed enable lost original or cleanup");
        Require(!fixture.Start().ready && fixture.creates == 1 && fixture.enables == 1, "failed enable was replayed");
        fixture.Invoke();
        Require(fixture.forwards == 1, "retained failed-enable original could not forward");
    }
    Fixture fixture; Require(fixture.Start().ready, "disable failure fixture start");
    fixture.disableKind = Kind::Failure;
    Require(!fixture.owner.Retire().stopped && fixture.owner.BeginEpoch() == 0, "failed disable regained observation");
    fixture.disableKind = Kind::Success;
    Require(fixture.owner.Retire().stopped && !fixture.Start().ready, "same owned disable did not reconcile");
}
/** @summary Prove a pre-entry callback is invisible to drains but still forwards after retirement through retained code. */
void PausedPreEntry() {
    Fixture fixture; Require(fixture.Start().ready, "pre-entry start");
    std::promise<void> paused, resume;
    auto pausedFuture = paused.get_future(); auto resumeFuture = resume.get_future().share();
    std::jthread original([&] { paused.set_value(); resumeFuture.wait(); fixture.Invoke(); });
    Release release{resume};
    pausedFuture.wait();
    Require(fixture.activity.Drained(), "pre-entry unexpectedly counted");
    fixture.activity.Close(); Require(fixture.owner.Retire().stopped, "pre-entry retirement failed");
    release.Now(); original.join();
    Require(fixture.forwards == 1 && fixture.activity.Drained(), "late original did not settle");
    Require(fixture.owner.Original() == Fixture::OriginalAddress(), "pre-entry original freed");
}
/** @summary Keep actual forwarding activity retained while stop revokes optional work. */
void OriginalInFlight() {
    Fixture fixture; Require(fixture.Start().ready, "in-flight start");
    std::promise<void> entered, resume;
    auto enteredFuture = entered.get_future(); auto resumeFuture = resume.get_future().share();
    fixture.onForward = [&] { entered.set_value(); resumeFuture.wait(); };
    std::jthread original([&] { fixture.Invoke(); });
    Release release{resume}; enteredFuture.wait();
    fixture.activity.Close();
    Require(!fixture.activity.Drained(), "in-flight original lost activity lease");
    Require(!fixture.owner.Retire().stopped && !fixture.activity.Drained(), "disable was confused with callback drain");
    release.Now(); original.join();
    Require(fixture.activity.Drained() && fixture.forwards == 1, "original did not retain through return");
    fixture.disableKind = Kind::AlreadyDisabled;
    Require(fixture.owner.Retire().stopped, "acknowledged disable did not settle after actual original returned");
}
/** @summary Global epoch revokes a real other thread's scope without writing its TLS, and nested scopes cannot restore stale data. */
void ObservationRevocation() {
    Fixture fixture; Require(fixture.Start().ready, "observation start");
    OwnerCaptureObservation local;
    Require(local.Begin(fixture.owner) && local.Allowed(fixture.owner), "scope did not open");
    Require(!local.Begin(fixture.owner) && !local.Allowed(fixture.owner), "nested scope inherited observation");
    local.End(); Require(!local.Allowed(fixture.owner), "nested end restored invalidated epoch"); local.End();
    Require(local.Begin(fixture.owner), "new independent scope did not open"); local.End();
    std::promise<void> acquired, resume;
    auto acquiredFuture = acquired.get_future(); auto resumeFuture = resume.get_future().share();
    std::atomic_bool admitted{}, staleAccepted{};
    std::jthread observer([&] {
        thread_local OwnerCaptureObservation scope;
        admitted = scope.Begin(fixture.owner); acquired.set_value(); resumeFuture.wait();
        staleAccepted = scope.Allowed(fixture.owner); scope.End();
    });
    Release release{resume}; acquiredFuture.wait();
    fixture.owner.CloseObservation(); release.Now(); observer.join();
    Require(admitted && !staleAccepted && fixture.owner.BeginEpoch() == 0, "other-thread TLS survived global revocation");
}
/** @summary A stop during Enable cannot open optional observation or clear the published original. */
void StopDuringEnable() {
    Fixture fixture;
    fixture.onEnable = [&] { fixture.owner.CloseObservation(); };
    Require(!fixture.Start().ready && fixture.owner.BeginEpoch() == 0 && fixture.disables == 1,
        "enable completion reopened after retirement");
    Require(fixture.owner.Original() == Fixture::OriginalAddress(), "racing stop cleared original");
}
/** @summary Synthetic original unwinding releases the existing activity entry without clearing retained original. */
void OriginalUnwind() {
    Fixture fixture; Require(fixture.Start().ready, "original unwind start");
    fixture.onForward = [] { throw std::runtime_error("synthetic original failure"); };
    bool observed{};
    try { fixture.Invoke(); } catch (const std::runtime_error&) { observed = true; }
    Require(observed && fixture.activity.Drained() && fixture.owner.Original() == Fixture::OriginalAddress(),
        "original unwind abandoned callback activity or original retention");
}
}
/** @summary Run isolated ownership fixtures; this executable never calls MinHook or native game code.
 * @return Zero only when every actual production lifecycle assertion succeeds. */
int main() {
    try {
        SuccessAndRetirement(); CreateFailures(); EnableAndDisableFailures(); PausedPreEntry(); OriginalInFlight();
        ObservationRevocation(); StopDuringEnable(); OriginalUnwind();
        std::cout << "PASS owner capture lifecycle: publication, failures, retention, late forwarding, epochs and reinitialization\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
