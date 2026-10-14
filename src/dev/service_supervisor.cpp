#include "platform/windows_arguments.hpp"
#include "dev/service_supervisor.hpp"
#include <Windows.h>
#include <vector>
#include <stdexcept>

namespace sc13::dev {
namespace {
/** @summary Own a Win32 resource through all startup and rollback paths. */
struct Handle final {
    HANDLE value{};
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

}

/** @summary Keep process handles alive for exit diagnostics and release the job first on teardown. */
struct ServiceSupervisor::State final {
    HANDLE job{};
    std::vector<std::pair<std::string, HANDLE>> processes;
    std::unique_ptr<AuthoringToolLease> tools;
    ~State() {
        tools.reset();
        if (job) CloseHandle(job);
        for (const auto& [id, process] : processes) { static_cast<void>(id); CloseHandle(process); }
    }
};

ServiceSupervisor::ServiceSupervisor() = default;
ServiceSupervisor::~ServiceSupervisor() = default;

bool ServiceSupervisor::Start(const ServiceModDefinition& definition,
    const std::filesystem::path& gameRoot, std::string& error) noexcept {
    try {
        if (state_) { error = "Developer services are already started"; return false; }
        if (definition.apiVersion != 1 || definition.services.empty() || definition.services.size() > 16 || !gameRoot.is_absolute())
            throw std::runtime_error("Invalid developer-service contract");
        auto candidate = std::make_unique<State>();
        candidate->processes.reserve(definition.services.size());
        candidate->job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!candidate->job || !SetInformationJobObject(candidate->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("Cannot configure game-owned service job");
        for (const auto& service : definition.services) {
            if (!service.executable.is_absolute() || !std::filesystem::is_regular_file(service.executable) ||
                (GetFileAttributesW(service.executable.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("Developer executable is absent or not a plain file: " + service.id);
            std::wstring command = platform::QuoteWindowsArgument(service.executable.native());
            for (const auto& argument : service.arguments) command += L" " + platform::QuoteWindowsArgument(argument);
            command += L" --parent-pid " + std::to_wstring(GetCurrentProcessId()) + L" --game-root " + platform::QuoteWindowsArgument(gameRoot.native());
            if (command.size() >= 32767) throw std::runtime_error("Developer command line exceeds Windows limit");
            std::vector<wchar_t> buffer(command.begin(), command.end());
            buffer.push_back(0);
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(service.executable.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, service.executable.parent_path().c_str(), &startup, &process))
                throw std::runtime_error("Cannot start developer service: " + service.id);
            Handle processHandle(process.hProcess), threadHandle(process.hThread);
            if (!AssignProcessToJobObject(candidate->job, process.hProcess)) {
                TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
                throw std::runtime_error("Cannot assign developer service to game job: " + service.id);
            }
            if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
                throw std::runtime_error("Cannot resume developer service: " + service.id);
            candidate->processes.emplace_back(service.id, processHandle.value);
            processHandle.value = nullptr;
        }
        for (const auto& [id, process] : candidate->processes)
            if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
                throw std::runtime_error("Developer service exited during startup: " + id);
        if (!definition.tools.empty()) candidate->tools = RegisterAuthoringTools(definition.owner, definition.tools);
        state_ = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
    catch (...) { error = "Developer-service startup failed"; return false; }
}

void ServiceSupervisor::Stop() noexcept { state_.reset(); }

std::string ServiceSupervisor::Diagnostic() const {
    if (!state_) return {};
    for (const auto& [id, process] : state_->processes) {
        const auto status = WaitForSingleObject(process, 0);
        if (status == WAIT_TIMEOUT) continue;
        if (status != WAIT_OBJECT_0) {
            state_->tools.reset();
            return "Cannot observe developer service lifetime: " + id;
        }
        state_->tools.reset();
        DWORD code{};
        if (!GetExitCodeProcess(process, &code)) return "Cannot inspect developer service: " + id;
        return "Developer service exited: " + id + " (code " + std::to_string(code) + ")";
    }
    return {};
}
}
