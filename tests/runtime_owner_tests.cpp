#include "platform/runtime_owner.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <sddl.h>
#include <iostream>
#include <stdexcept>
#include <string>

using sc13::platform::RuntimeOwnerTestAccess;
namespace {
/** @summary Fails an exact behavioral expectation. @param value Observed condition. @param message Failure diagnostic. */
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
/** @summary Allocates a process-specific unique TEST name; never opens the production owner object.
 * @return Bounded Global test namespace name. */
std::wstring Unique() {
    static unsigned int sequence{};
    return L"Global\\Simoder.TEST.U1." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++sequence);
}
/** @summary Runs a second process against only an owned TEST fixture.
 * @param name Existing TEST name. @return True when the child rejects existing ownership. */
bool DuplicateChild(const std::wstring& name) {
    wchar_t executable[32768]{};
    Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "test executable path unavailable");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --duplicate \"" + name + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    Require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &child) != FALSE, "duplicate child did not launch");
    CloseHandle(child.hThread);
    const auto waited = WaitForSingleObject(child.hProcess, 10000);
    DWORD result{1};
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &result);
    CloseHandle(child.hProcess);
    return waited == WAIT_OBJECT_0 && result == 0;
}
/** @summary Verifies fresh creation, strict duplicates, retained lifetime and recreated fresh identity. */
void Ownership() {
    const auto name = Unique();
    auto owner = RuntimeOwnerTestAccess::Acquire(name);
    Require(owner && owner->IsAcquired() && owner->Error() == ERROR_SUCCESS, "fresh restrictive TEST owner failed");
    const auto identity = owner->Identity();
    Require(identity.processId == GetCurrentProcessId() && identity.processCreationTime != 0 &&
            identity.nonce != decltype(identity.nonce){}, "fresh local identity missing");
    auto duplicate = RuntimeOwnerTestAccess::Acquire(name);
    Require(duplicate && !duplicate->IsAcquired() && duplicate->Error() != ERROR_SUCCESS, "same-process existing object accepted");
    Require(DuplicateChild(name), "second-process existing object accepted");
    auto retained = owner;
    owner.reset();
    Require(!RuntimeOwnerTestAccess::Acquire(name)->IsAcquired(), "retained owner released early");
    retained.reset();
    auto recreated = RuntimeOwnerTestAccess::Acquire(name);
    Require(recreated && recreated->IsAcquired() && recreated->Identity().nonce != identity.nonce, "fresh recreation reused identity");
    Require(!RuntimeOwnerTestAccess::Acquire(L"Global\\Simoder.Runtime.Owner"), "test factory accessed production name");
}
/** @summary Verifies wrong-type and explicitly denied TEST objects cannot become owner authority. */
void Denials() {
    const auto wrongName = Unique();
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, wrongName.c_str());
    Require(event != nullptr, "wrong-type fixture creation failed");
    auto wrong = RuntimeOwnerTestAccess::Acquire(wrongName);
    CloseHandle(event);
    Require(wrong && !wrong->IsAcquired(), "wrong-type object accepted");
    const auto deniedName = Unique();
    PSECURITY_DESCRIPTOR descriptor{};
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;GA;;;WD)", SDDL_REVISION_1,
            &descriptor, nullptr) != FALSE, "deny TEST descriptor failed");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    HANDLE denied = CreateMutexExW(&attributes, deniedName.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    const DWORD createdError = GetLastError();
    LocalFree(descriptor);
    Require(denied != nullptr && createdError != ERROR_ALREADY_EXISTS, "denied TEST fixture creation failed");
    auto blocked = RuntimeOwnerTestAccess::Acquire(deniedName);
    CloseHandle(denied);
    Require(blocked && !blocked->IsAcquired(), "denied owner object accepted");
}
}
/** @summary Executes unique TEST-only Win32 owner fixtures; never touches production object or game.
 * @param argc Argument count. @param argv Optional duplicate-child TEST name. @return Zero for verified rejection/pass. */
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--duplicate") {
            const auto duplicate = RuntimeOwnerTestAccess::Acquire(argv[2]);
            return duplicate && !duplicate->IsAcquired() ? 0 : 1;
        }
        Ownership(); Denials();
        std::cout << "Restrictive fresh-only Win32 TEST owner and lifetime passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
