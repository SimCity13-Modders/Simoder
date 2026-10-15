#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace sc13::platform {
/** @summary Core-local identity of this freshly created process-lifetime owner; never a PID-file claim. */
struct RuntimeOwnerIdentity final {
    std::uint32_t processId{};
    std::uint64_t processCreationTime{};
    std::array<std::uint32_t, 4> nonce{};
};

/** @summary Holds only a freshly created restrictive machine-wide object; existing objects always deny. */
class RuntimeOwner final {
public:
    /** @summary Attempts the one stable production name without waiting, recovery or privilege changes. */
    RuntimeOwner();
    /** @summary Closes only the handle created by this instance after retained owners release it. */
    ~RuntimeOwner();
    RuntimeOwner(const RuntimeOwner&) = delete;
    RuntimeOwner& operator=(const RuntimeOwner&) = delete;
    /** @summary Reports fresh ownership. @return True only after fresh creation and local identity validation. */
    bool IsAcquired() const noexcept;
    /** @summary Returns diagnostic Win32 failure. @return Zero for success, otherwise the captured error. */
    std::uint32_t Error() const noexcept;
    /** @summary Returns immutable core-local identity. @return Zero-valued identity on failure. */
    const RuntimeOwnerIdentity& Identity() const noexcept;
private:
    /** @summary Uses a supplied name only from the test-only friend factory. @param name Stable production or restricted TEST name. */
    explicit RuntimeOwner(const std::wstring& name);
    void* handle_{};
    std::uint32_t error_{};
    RuntimeOwnerIdentity identity_{};
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    friend struct RuntimeOwnerTestAccess;
#endif
};

#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
/** @summary Test-only construction, excluded from production targets and limited to TEST namespace. */
struct RuntimeOwnerTestAccess final {
    /** @summary Acquires a bounded TEST name with the production security policy.
     * @param name Unique Global\\Simoder.TEST.U1.* name. @return Owner or null for invalid namespace. */
    static std::shared_ptr<RuntimeOwner> Acquire(const std::wstring& name) {
        if (!name.starts_with(L"Global\\Simoder.TEST.U1.") || name.size() > 160) return {};
        return std::shared_ptr<RuntimeOwner>(new RuntimeOwner(name));
    }
};
#endif
}
