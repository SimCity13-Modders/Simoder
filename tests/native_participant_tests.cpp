/** @file @summary Isolated actual-loader lifetime tests; execution requires separate frozen-image audit. */
#include "mods/native/native_participant.hpp"
#include "platform/runtime_owner.hpp"
#include "core/sha256.hpp"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace sc13;
using namespace sc13::mods::native;
static_assert(!std::is_default_constructible_v<NativeParticipant>);
static_assert(!std::is_constructible_v<NativeParticipant, SdHandle>);
namespace {
/** @summary Stops a test on an observed invariant failure. @param value Actual result. @param message Diagnostic. */
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
/** @summary Hashes the exact previously approved synthetic file without supplying production trust.
 * @param path Actual image. @return Isolated reviewed fixture registration. */
ReviewedModule Review(const std::filesystem::path& path) {
    ReviewedModule result; result.moduleId = "fixture.participant"; result.absolutePath = path; result.nativeConsentApproved = true;
    core::Sha256Digest digest{}; std::string error;
    Check(core::Sha256File(path, digest, error), "fixture SHA-256 read");
    for (std::size_t i = 0; i < digest.size(); ++i) result.sha256[i] = std::to_integer<unsigned char>(digest[i]);
    return result;
}
/** @summary Clears caller output on stop while preserving the captured original lifetime.
 * @param module Actual returned generation. @param original Exact prior capture. */
void Closed(const NativeModule& module, const NativeParticipant& original) {
    std::optional<NativeParticipant> output = original;
    Check(module.CaptureParticipant(output) == SD_STALE && !output, "stopped capture clears original output");
    Check(!original.ObserveLocalFence().LocallyLive(), "original local fence denies");
}
/** @summary Synchronously throws on a real lifecycle callback, never a native access violation. */
void SD_CALL Throw(void*, const SdState*) { throw std::runtime_error("ordinary callback failure"); }
/** @summary One retained callback barrier; no detached or replacement participant is permitted. */
struct Barrier final {
    std::mutex mutex;
    std::condition_variable changed;
    bool armed{}, entered{}, release{};
    std::size_t deliveries{};
};
/** @summary Blocks the original admitted lifecycle callback until its original host releases it.
 * @param user Original barrier retained beyond publisher join. */
void SD_CALL Wait(void* user, const SdState*) {
    auto& barrier = *static_cast<Barrier*>(user);
    std::unique_lock lock(barrier.mutex);
    ++barrier.deliveries;
    if (!barrier.armed) return;
    barrier.entered = true; barrier.changed.notify_all();
    barrier.changed.wait(lock, [&] { return barrier.release; });
}
/** @summary Joins the same original publisher even when an assertion/reporting operation fails. */
struct Publisher final {
    Barrier& barrier;
    std::thread thread;
    ~Publisher() {
        { const std::lock_guard lock(barrier.mutex); barrier.release = true; }
        barrier.changed.notify_all();
        if (thread.joinable()) thread.join();
    }
};
}
/** @summary Runs one isolated permanent-registry scenario without game, VM or production authority.
 * @param argc Scenario, absolute owned root and one approved image are mandatory.
 * @param argv Exact independently reviewed arguments. @return Zero only after actual terminal observations. */
int main(int argc, char** argv) {
    try {
        Check(argc == 4, "expected scenario, owned root, exact fixture DLL");
        const std::string mode = argv[1];
        const std::filesystem::path parent = argv[2];
        Check(parent.is_absolute() && std::filesystem::is_directory(parent), "explicit existing owned root");
        const auto root = parent / ("participant-" + mode + "-" + std::to_string(GetCurrentProcessId()));
        Check(std::filesystem::create_directory(root), "fresh non-overwriting scenario root");
        const auto stage = root / "stage"; Check(std::filesystem::create_directory(stage), "plain stage created");
        const auto source = root / "fixture.dll";
        Check(std::filesystem::copy_file(argv[3], source), "copy exact approved image");
        auto owner = platform::RuntimeOwnerTestAccess::Acquire(L"Global\\Simoder.TEST.U1.Participant." + std::to_wstring(GetCurrentProcessId()));
        Check(owner && owner->IsAcquired(), "actual TEST-only runtime owner");
        auto domain = std::make_shared<runtime::RuntimeCommitDomain>();
        auto lifecycle = std::make_shared<runtime::RuntimeLifecycle>();
        NativeModuleLoader loader(stage, owner, domain, lifecycle);
        const auto registration = Review(source);
        std::shared_ptr<NativeModule> module;
        if (mode == "query-failure" || mode == "prepare-failure") {
            const auto result = loader.Prepare(registration, module);
            Check(result != SD_OK && !module, "failed native preparation exposes no capture source");
            Check(NativeModuleLoader::Usage().count == 1, "failed image retained");
            Check(NativeModuleLoader::Usage().generations == (mode == "query-failure" ? 0U : 1U), "only actual allocated States counted");
        } else {
            Check(loader.Prepare(registration, module) == SD_OK && module, "actual native Prepare succeeds");
            std::optional<NativeParticipant> participant, second;
            Check(module->CaptureParticipant(participant) == SD_OK && participant, "success-only typed capture");
            Check(module->CaptureParticipant(second) == SD_OK && second && participant->SameIdentity(*second), "same original State identity");
            const auto original = participant->ObserveLocalFence();
            Check(original.LocallyLive() && original.phase == ModulePhase::Prepared && !original.retired, "prepared is locally live, never Active");
            Check(original.identity.owner.slot == module->Host().owner.slot && original.identity.owner.generation == 1 &&
                original.identity.incarnation == original.identity.owner.slot && original.identity.scopeGeneration &&
                original.identity.imageSha256 == registration.sha256, "exact original immutable identity");
            Check(!module->Host().semantic && !module->Host().credentials && !module->Host().dispatch && module->Admit() == SD_NOT_READY, "no production authority or VM facade");
            Check(module->Drain() == SD_NOT_READY, "drain before stop is not completion");
            if (mode == "lifetime" || mode == "generations") {
                const unsigned count = mode == "generations" ? 128 : 2;
                auto previous = participant;
                for (unsigned i = 0; i < count; ++i) {
                    if (i) {
                        Check(loader.Prepare(registration, module) == SD_OK, "same digest admits distinct State after drain");
                        Check(module->CaptureParticipant(participant) == SD_OK && !participant->SameIdentity(*previous), "new State cannot renew old identity");
                        const auto next = participant->ObserveLocalFence();
                        Check(next.identity.imageSha256 == original.identity.imageSha256 && next.identity.owner.slot != previous->ObserveLocalFence().identity.owner.slot &&
                            next.identity.scopeGeneration != previous->ObserveLocalFence().identity.scopeGeneration, "same image distinct slot and scope");
                    }
                    Check(module->AdmitSynthetic() == SD_OK && module->Phase() == ModulePhase::Active, "existing fixture-only activation");
                    Check(module->Quiesce() == SD_OK, "actual cooperative quiesce"); Closed(*module, *participant);
                    Check(module->Drain() == SD_OK && module->Phase() == ModulePhase::Drained, "all original drain conditions complete");
                    Check(participant->ObserveLocalFence().retired, "retirement remains sticky after drain"); previous = participant;
                }
                module.reset(); second.reset();
                Check(participant->ObserveLocalFence().identity.imageSha256 == registration.sha256 && previous->SameIdentity(*participant), "drop module before retained view");
                if (mode == "generations") Check(loader.Prepare(registration, module) == SD_RESTART_REQUIRED && !module &&
                    NativeModuleLoader::Usage().generations == 128, "exact retained generation bound without reset/reuse");
                Check(NativeModuleLoader::Usage().count == 1, "identical image is retained once");
            } else if (mode == "domain") {
                domain->RequestStop();
                Check(participant->ObserveLocalFence().domainStopping && !participant->ObserveLocalFence().retired, "genuine domain atomic distinct from local retirement");
                Closed(*module, *participant);
            } else if (mode == "lifecycle") {
                lifecycle->Publish({0, false, false, false, true});
                Check(participant->ObserveLocalFence().LocallyLive(), "local fence honestly excludes external lifecycle freshness");
                Check(module->CaptureParticipant(second) == SD_STALE && !second && module->Admit() == SD_STALE, "off-guard original lifecycle observation denies");
            } else if (mode == "callback-failure") {
                SdHandle subscription{};
                Check(module->Host().lifecycle->subscribe_current(module->Host().owner, Throw, nullptr, &subscription) == SD_STALE && !subscription.slot, "actual initial callback failure closes registration and clears output");
                Closed(*module, *participant);
                Check(module->Quiesce() == SD_OK && module->Drain() == SD_OK, "callback-failure original scope cleanup");
            } else if (mode == "inflight") {
                Barrier barrier; SdHandle subscription{};
                Check(module->Host().lifecycle->subscribe_current(module->Host().owner, Wait, &barrier, &subscription) == SD_OK, "original callback registration");
                Check(barrier.deliveries == 1, "initial original callback returns before arming host gate");
                { const std::lock_guard lock(barrier.mutex); barrier.armed = true; }
                bool published{};
                Publisher publisher{barrier, std::thread([&] { published = lifecycle->Publish({0, true, true, false, false}); })};
                bool entered{};
                { std::unique_lock lock(barrier.mutex); entered = barrier.changed.wait_for(lock, std::chrono::seconds(5), [&] { return barrier.entered; }); }
                Check(entered, "bounded original publisher barrier entry");
                Check(module->Quiesce() == SD_OK && module->Drain() == SD_BUSY, "inflight callback retains original State through Busy"); Closed(*module, *participant);
                Check(module->Phase() == ModulePhase::Stopping && NativeParticipantTestAccess::ForeignCloseCalls(*module) == std::array<std::uint32_t, 2>{1, 1}, "foreign drain succeeds once before actual host callback fence");
                std::shared_ptr<NativeModule> replacement;
                Check(loader.Prepare(registration, replacement) == SD_CONFLICT && !replacement, "original host callback prevents successor generation");
                Check(module->Drain() == SD_BUSY && module->Quiesce() == SD_OK && module->Drain() == SD_BUSY,
                    "repeated residual observations preserve actual callback Busy");
                Check(NativeParticipantTestAccess::ForeignCloseCalls(*module) == std::array<std::uint32_t, 2>{1, 1}, "successful foreign borrow is never replayed while host cleanup is pending");
                { const std::lock_guard lock(barrier.mutex); barrier.release = true; }
                barrier.changed.notify_all(); publisher.thread.join();
                Check(published && barrier.deliveries == 2, "same original publisher and host callback reach genuine terminal");
                Check(module->Drain() == SD_OK && module->Phase() == ModulePhase::Drained, "only genuine terminal callback permits drain");
                Check(module->Quiesce() == SD_OK && module->Drain() == SD_OK &&
                    NativeParticipantTestAccess::ForeignCloseCalls(*module) == std::array<std::uint32_t, 2>{1, 1}, "final host close and drain do not replay foreign exports");
                Check(lifecycle->Publish({0, true, false, true, false}) && barrier.deliveries == 2, "no callback enters after actual original unsubscribe fence");
                Check(module->Host().lifecycle->unsubscribe(subscription) == SD_STALE, "exact original token is retired only after callback fence");
            } else if (mode == "capture-race") {
                std::atomic_bool start{}; std::atomic_bool coherent{true};
                std::thread reader([&] {
                    while (!start.load()) std::this_thread::yield();
                    bool retired{};
                    for (unsigned i = 0; i != 4096; ++i) {
                        std::optional<NativeParticipant> captured;
                        const auto result = module->CaptureParticipant(captured);
                        if (result == SD_OK && (!captured || !captured->SameIdentity(*participant))) coherent = false;
                        if (result != SD_OK && (result != SD_STALE || captured)) coherent = false;
                        const auto observed = participant->ObserveLocalFence();
                        if ((retired && !observed.retired) || ((observed.phase == ModulePhase::Stopping || observed.phase == ModulePhase::Drained || observed.phase == ModulePhase::Faulted) && !observed.retired)) coherent = false;
                        retired = observed.retired;
                    }
                });
                start = true; const auto quiesced = module->Quiesce(); reader.join();
                Check(quiesced == SD_OK && coherent.load(), "capture race retains exact identity and sticky coherent retirement"); Closed(*module, *participant);
                Check(module->Drain() == SD_OK, "race participants genuinely terminal");
            } else if (mode == "busy") {
                Check(module->Quiesce() == SD_OK && module->Drain() == SD_BUSY && module->Drain() == SD_BUSY, "foreign cooperative Busy never represented as drained"); Closed(*module, *participant);
                Check(NativeParticipantTestAccess::ForeignCloseCalls(*module) == std::array<std::uint32_t, 2>{1, 2}, "foreign Busy preserves genuine retry attempts before success");
                Check(module->Phase() == ModulePhase::Stopping && NativeModuleLoader::Usage().generations == 1, "busy original image/state retained");
            } else if (mode == "admit-failure") {
                Check(module->AdmitSynthetic() != SD_OK, "actual synthetic Admit failure"); Closed(*module, *participant);
            } else throw std::runtime_error("unknown scenario");
        }
        std::cout << "PASS " << mode << " retained-images=" << NativeModuleLoader::Usage().count << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
