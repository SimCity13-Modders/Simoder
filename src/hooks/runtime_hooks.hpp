#pragma once
#include "reverse/game_build.hpp"
#include <string>
#include <atomic>

namespace sc13::hooks {
/** @summary Retain detour dependencies through native forwarding and suppress optional work after retirement. */
class RuntimeHookActivity final {
public:
    /** @summary Retain one actual hook entry, including forwarding after admission closes. */
    class Entry final {
    public:
        /** @summary Count before reading any original procedure or owned dependency.
         * @param owner Shared activity state for these hooks.
         */
        explicit Entry(RuntimeHookActivity& owner) noexcept : owner_(owner) {
            owner_.active_.fetch_add(1); admitted_ = !owner_.closed_.load();
        }
        /** @summary Release only after the original procedure and optional work return. */
        ~Entry() { owner_.active_.fetch_sub(1); }
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        /** @summary Report optional-work admission at this entry boundary.
         * @return False for retirement forwarding only.
         */
        [[nodiscard]] bool admitted() const noexcept { return admitted_; }
    private:
        RuntimeHookActivity& owner_;
        bool admitted_{};
    };
    /** @summary Close optional work without waiting for a callback. */
    void Close() noexcept { closed_.store(true); }
    /** @summary Open only an inactive activity state before enabling its hooks.
     * @return Whether all earlier entries had already returned.
     */
    [[nodiscard]] bool Open() noexcept { if (!Drained()) return false; closed_.store(false); return true; }
    /** @summary Observe current callback retention without waiting.
     * @return Whether native and optional work have all returned.
     */
    [[nodiscard]] bool Drained() const noexcept { return active_.load() == 0; }
private:
    std::atomic_uint active_{};
    std::atomic_bool closed_{true};
};
/** @summary Access the single activity owner for this runtime's resource/log detours.
 * @return Process-lifetime activity state owned by this DLL.
 */
RuntimeHookActivity& RuntimeHookCallbacks() noexcept;
/** @summary Preserve existing pre-hook resource preparation while refusing retirement re-entry.
 * @return Whether optional resource callbacks may operate before hook installation.
 */
[[nodiscard]] bool PrepareRuntimeHookCallbacks() noexcept;
/** @summary Install only supported game hooks after exact-build validation.
 * @param fingerprint Verified game image identity.
 * @param error Actionable diagnostic on failure.
 * @return Whether the complete required hook transaction was enabled.
 */
[[nodiscard]] bool InstallRuntimeHooks(const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept;
/** @summary Disable owned hooks and release optional services after drain; retain disabled trampolines until process exit.
 * @return True after optional teardown; false retains dependencies for a later retry. Reinstallation requires process restart. */
[[nodiscard]] bool UninstallRuntimeHooks() noexcept;
}
