#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace sc13::mods {
class ManagerCommands;
class ManagerCommandRoute;
class PublicationOrigin;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Genuine original queue scalars; no authority or payload copy. */
struct ActionQueueObservation final{bool scheduled{},busy{},stopping{},closed{};std::uint32_t thread{};};
#endif
/** @summary Carry one validated UI operation without coupling worker to lifecycle state. */
struct ModAction { std::string kind; std::string id; std::string revision; };
/** @summary Serialize the original single occupancy and join before owner destruction. */
class ActionWorker final {
public:
    using Handler = std::function<std::string(const ModAction&)>;
    /** @summary Original occupancy retained inertly by associated VMs after join. */
    struct Control;
    /** @summary Start the sole action thread. @param handler Existing weak-owner UI handler. */
    explicit ActionWorker(Handler handler);
    /** @summary Close ingress, dispose queued ownership off guards and join actual execution. */
    ~ActionWorker();
    /** @summary Submit off-lane only to empty original occupancy; lane reentry cannot acquire W under V. @param action UI request. @return Accepted once. */
    [[nodiscard]] bool Submit(ModAction action);
    /** @summary Copy progress off-lane under original guard. @return Current status, or empty for prohibited lane reentry. */
    [[nodiscard]] std::string Status() const;
    /** @summary Observe pending/executing/disposal occupancy. @return Actual busy. */
    [[nodiscard]] bool Busy() const noexcept;
private:
    friend class ModManager;
    friend class ManagerClose;
    friend class ManagerCommands;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct ManagerCloseTestAccess;
    friend struct ManagerCommandsTestAccess;
#endif
    /** @summary Retain genuine original cohort without queued manager borrower or permission ticket. */
    struct ScheduledJob final {
        std::shared_ptr<ManagerCommandRoute> route;
        std::shared_ptr<PublicationOrigin> origin;
        std::uint64_t registration{},generation{};
    };
    /** @summary One tagged alternative replaces the existing optional, never another queue. */
    using Pending = std::variant<std::monostate,ModAction,ScheduledJob>;
    static_assert(std::is_nothrow_default_constructible_v<ScheduledJob>);
    static_assert(std::is_nothrow_move_constructible_v<ScheduledJob>);
    static_assert(std::is_nothrow_move_constructible_v<Pending>);
    static_assert(std::is_nothrow_swappable_v<Pending>);
public:
    /** @summary Same guard/wake/slot/latches; joined storage retains no callbacks or payload. */
    struct Control final {
    private:
        friend class ActionWorker;
        friend class ManagerCommands;
        friend class ModManager;
        friend class ManagerClose;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        friend struct ManagerCloseTestAccess;
        friend struct ManagerCommandsTestAccess;
#endif
        Handler handler;
        mutable std::mutex mutex;
        std::condition_variable wake;
        Pending pending;
        std::string status;
        std::atomic_bool busy{};
        bool stopping{};
        std::shared_ptr<std::atomic_bool> closeAdmission{std::make_shared<std::atomic_bool>(false)};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        std::shared_ptr<const std::function<void()>> executionBarrier;
        std::shared_ptr<const std::function<void(ActionQueueObservation)>> beforeDetach;
#endif
    };
private:
    /** @summary Pre-refactor storage baseline excludes the preexisting thread object. */
    struct PriorStorage final {
        Handler handler;
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::optional<ModAction> pending;
        std::string status;
        std::atomic_bool busy{};
        bool stopping{};
        std::shared_ptr<std::atomic_bool> closeAdmission;
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
        std::shared_ptr<const std::function<void()>> executionBarrier;
#endif
    };
    /** @summary Try truthful original occupancy. @param busy Actual busy. @return Guard acquired. */
    bool TryCloseObservation(bool& busy) noexcept;
    /** @summary Execute and dispose accepted work outside W. @param control Original storage. @param stop Thread stop request. */
    static void Run(const std::shared_ptr<Control>& control,std::stop_token stop);
    std::shared_ptr<Control> control_;
    /** @summary Preserve the original admission latch shared with State. */
    std::shared_ptr<std::atomic_bool> closeAdmission_;
    std::jthread thread_;
};
}
