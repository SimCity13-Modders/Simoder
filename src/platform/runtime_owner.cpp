#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#include "platform/runtime_owner.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <sddl.h>
#include <cstdlib>
#include <vector>

namespace sc13::platform {
RuntimeOwner::RuntimeOwner() : RuntimeOwner(L"Global\\Simoder.Runtime.Owner") {}

RuntimeOwner::RuntimeOwner(const std::wstring& name) {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) { error_ = GetLastError(); return; }
    DWORD length{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    std::vector<unsigned char> storage(length);
    const BOOL queried = length != 0 && GetTokenInformation(token, TokenUser, storage.data(), length, &length);
    const DWORD queryError = queried ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!queried) { error_ = queryError ? queryError : ERROR_INVALID_DATA; return; }
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) {
        error_ = GetLastError(); return;
    }
    const std::wstring descriptor = L"D:P(A;;0x00100001;;;SY)(A;;0x00100001;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR security{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr)) {
        error_ = GetLastError(); return;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security, FALSE};
    SetLastError(ERROR_SUCCESS);
    HANDLE created = CreateMutexExW(&attributes, name.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    error_ = GetLastError();
    LocalFree(security);
    if (!created || error_ != ERROR_SUCCESS) {
        if (created) CloseHandle(created);
        if (!error_) error_ = ERROR_INVALID_HANDLE;
        return;
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        error_ = GetLastError(); CloseHandle(created); return;
    }
    RuntimeOwnerIdentity identity;
    identity.processId = GetCurrentProcessId();
    identity.processCreationTime = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    for (auto& value : identity.nonce) {
        unsigned int random{};
        if (rand_s(&random) != 0) { error_ = ERROR_GEN_FAILURE; CloseHandle(created); return; }
        value = random;
    }
    identity_ = identity;
    handle_ = created;
}

RuntimeOwner::~RuntimeOwner() { if (handle_) CloseHandle(handle_); }
bool RuntimeOwner::IsAcquired() const noexcept { return handle_ != nullptr; }
std::uint32_t RuntimeOwner::Error() const noexcept { return error_; }
const RuntimeOwnerIdentity& RuntimeOwner::Identity() const noexcept { return identity_; }
}
