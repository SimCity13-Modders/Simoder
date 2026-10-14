#include "dev/runtime_diagnostics.hpp"
#include "dev/runtime_control.hpp"
#include "dev/session/session_transport.hpp"
#include "core/json.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include <Windows.h>
#include <chrono>
#include <fstream>
#include <array>

namespace sc13::dev {
namespace {
/** @summary Observes filesystem events without polling asset contents or reacting to generated cache writes. */
class SourceWatch final {
public:
    /** @summary Arms one overlapped recursive watch after mod discovery creates the root. */
    explicit SourceWatch(const std::filesystem::path& root) {
        directory_ = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        overlapped_.hEvent = event_;
        Arm();
    }
    /** @summary Cancels and drains the outstanding operation before releasing its buffer. */
    ~SourceWatch() {
        if (directory_ != INVALID_HANDLE_VALUE) {
            if (pending_) {
                CancelIoEx(directory_, &overlapped_);
                DWORD ignored{}; GetOverlappedResult(directory_, &overlapped_, &ignored, TRUE);
            }
            CloseHandle(directory_);
        }
        if (event_) CloseHandle(event_);
    }
    /** @summary Consumes only completed events; a failed native watch uses a bounded five-second fallback. */
    bool Changed() {
        if (!pending_) {
            const auto now = std::chrono::steady_clock::now();
            if (now - fallback_ < std::chrono::seconds(5)) return false;
            fallback_ = now; return true;
        }
        if (WaitForSingleObject(event_, 0) != WAIT_OBJECT_0) return false;
        DWORD bytes{};
        bool changed = !GetOverlappedResult(directory_, &overlapped_, &bytes, FALSE) || bytes == 0;
        pending_ = false;
        std::size_t offset{};
        while (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= bytes) {
            const auto* item = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer_.data() + offset);
            if (item->FileNameLength % sizeof(wchar_t) || offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) + item->FileNameLength > bytes) { changed = true; break; }
            const std::wstring_view name(item->FileName, item->FileNameLength / sizeof(wchar_t));
            const auto separator = name.find(L'\\');
            const auto first = name.substr(0, separator);
            if (CompareStringOrdinal(first.data(), static_cast<int>(first.size()), L"cache", 5, TRUE) != CSTR_EQUAL) changed = true;
            if (!item->NextEntryOffset) break;
            if (item->NextEntryOffset < offsetof(FILE_NOTIFY_INFORMATION, FileName) || item->NextEntryOffset > bytes - offset) { changed = true; break; }
            offset += item->NextEntryOffset;
        }
        Arm(); return changed;
    }
private:
    /** @summary Reuses a fixed buffer and event, avoiding one allocation per changed file. */
    void Arm() {
        if (directory_ == INVALID_HANDLE_VALUE || !event_) return;
        ResetEvent(event_);
        pending_ = ReadDirectoryChangesW(directory_, buffer_.data(), static_cast<DWORD>(buffer_.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            nullptr, &overlapped_, nullptr) != FALSE;
    }
    HANDLE directory_{INVALID_HANDLE_VALUE};
    HANDLE event_{};
    OVERLAPPED overlapped_{};
    alignas(FILE_NOTIFY_INFORMATION) std::array<std::byte, 65536> buffer_{};
    bool pending_{};
    std::chrono::steady_clock::time_point fallback_{std::chrono::steady_clock::now()};
};
/** @summary Serializes resolved operations and optional pending definitions with bounded provenance. */
std::string PatchesJson(std::span<const mods::ResourcePatchDefinition> patches) {
    std::string json = "["; bool comma = false;
    for (const auto& patch : patches) {
        if (comma) json += ','; comma = true;
        json += "{\"tgi\":" + core::JsonString(core::ToString(patch.target)) + ",\"name\":" + core::JsonString(patch.name) +
            ",\"source\":" + core::JsonString(patch.source) + ",\"properties\":[";
        bool propertyComma = false;
        for (const auto& property : patch.properties) {
            if (propertyComma) json += ','; propertyComma = true;
            json += "{\"id\":" + std::to_string(property.propertyId) + ",\"name\":" + core::JsonString(property.name) +
                ",\"source\":" + core::JsonString(property.source) + ",\"operation\":" + core::JsonString(mods::PatchOperationName(property.operation)) +
                ",\"operand\":" + core::JsonFloat(property.value) + ",\"description\":" + core::JsonString(property.description) +
                ",\"unit\":" + core::JsonString(property.unit) + ",\"origin\":" + core::JsonString(property.origin) + "}";
        }
        json += "]}";
    }
    return json + ']';
}
}

std::string RuntimeDiagnosticsJson(const mods::ModManager& manager, std::uint32_t processId, bool controlAvailable) {
    const auto generation = manager.Generation();
    const auto mods = manager.Snapshot();
    const auto observations = manager.RuntimeObservations();
    const auto conflicts = manager.Conflicts();
    const auto generated = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string json = "{\"schemaVersion\":1,\"pid\":" + std::to_string(processId) + ",\"generatedAtUnixMs\":" + std::to_string(generated) +
        ",\"generation\":" + std::to_string(generation) + ",\"consistentGeneration\":" + (generation == manager.Generation() ? "true" : "false") +
        ",\"evidence\":\"historical committed resource values; copied consumers and gameplay effects unverified\",\"mods\":[";
    bool comma = false;
    for (const auto& mod : mods) {
        if (comma) json += ','; comma = true;
        json += "{\"id\":" + core::JsonString(mod.id) + ",\"name\":" + core::JsonString(mod.name) + ",\"state\":" + core::JsonString(mods::ModStateName(mod.state)) +
            ",\"diagnostic\":" + core::JsonString(mod.diagnostic) + ",\"canReload\":" + (mod.canReload ? "true" : "false") +
            ",\"pendingRevision\":" + core::JsonString(mod.pendingRevision) +
            ",\"script\":{\"language\":" + core::JsonString(mod.scriptLanguage) +
            ",\"revision\":" + core::JsonString(mod.scriptRevision) +
            ",\"pendingRevision\":" + core::JsonString(mod.pendingScriptRevision) +
            ",\"running\":" + (mod.scriptRunning ? "true" : "false") +
            ",\"memoryBytes\":" + std::to_string(mod.scriptMemoryBytes) +
            ",\"diagnostic\":" + core::JsonString(mod.scriptDiagnostic) + "}" +
            ",\"patches\":" + PatchesJson(mod.patches) + ",\"pendingPatches\":" + PatchesJson(mod.pendingPatches) + "}";
    }
    json += "],\"resources\":["; comma = false;
    for (const auto& observation : observations) {
        if (comma) json += ','; comma = true;
        json += "{\"tgi\":" + core::JsonString(core::ToString(observation.key.target)) + ",\"instanceToken\":" + std::to_string(observation.key.resourceAddress) +
            ",\"generation\":" + std::to_string(observation.generation) + ",\"status\":" + core::JsonString(observation.status) +
            ",\"lastCommittedGeneration\":" + std::to_string(observation.committedGeneration) +
            ",\"diagnostic\":" + core::JsonString(observation.diagnostic) + ",\"observedAtUnixMs\":" + std::to_string(observation.observedAtUnixMs) + ",\"values\":[";
        bool valueComma = false;
        std::set<std::uint32_t> selected;
        for (const auto& mod : mods) for (const auto& patch : mod.patches)
            if (patch.target == observation.key.target) for (const auto& property : patch.properties) selected.insert(property.propertyId);
        for (const auto id : selected) {
            if (valueComma) json += ','; valueComma = true;
            const auto vanilla = core::ReadRuntimeFloat(observation.vanilla, id);
            const auto applied = core::ReadRuntimeFloat(observation.applied, id);
            json += "{\"id\":" + std::to_string(id) + ",\"vanilla\":" + (vanilla ? core::JsonFloat(*vanilla) : "null") +
                ",\"lastCommitted\":" + (applied ? core::JsonFloat(*applied) : "null") + "}";
        }
        json += "]}";
    }
    json += "],\"controlAvailable\":" + std::string(controlAvailable ? "true" : "false") +
        ",\"nativeSession\":" + session::SessionTransportJson() + ",\"conflicts\":["; comma = false;
    for (const auto& conflict : conflicts) {
        if (comma) json += ','; comma = true;
        json += "{\"tgi\":" + core::JsonString(core::ToString(conflict.target)) + ",\"propertyId\":" + std::to_string(conflict.propertyId) +
            ",\"earlier\":" + core::JsonString(conflict.earlierOwner) + ",\"later\":" + core::JsonString(conflict.laterOwner) + "}";
    }
    return json + "]}\n";
}

RuntimeDiagnostics::RuntimeDiagnostics(mods::ModManager& manager, std::filesystem::path destination)
    : worker_([this, &manager, destination = std::move(destination)](std::stop_token stop) {
        SourceWatch watch(manager.ModsDirectory());
        bool first = true;
        while (!stop.stop_requested()) {
            try {
                std::string refreshError;
                if (first || watch.Changed()) static_cast<void>(manager.Refresh(refreshError));
                first = false;
                const bool controlAvailable = manager.DeveloperServicesActive();
                if (controlAvailable) ProcessRuntimeControl(manager, destination.parent_path());
                session::PollSessionTransport(destination.parent_path(), controlAvailable);
                const auto json = RuntimeDiagnosticsJson(manager, GetCurrentProcessId(), controlAvailable);
                if (json.size() <= 1024U * 1024U) {
                    auto temporary = destination; temporary += L".tmp";
                    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                    file << json; file.close();
                    if (file) static_cast<void>(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
                }
            } catch (...) { /* @summary Optional developer publishing cannot stop runtime patches. */ }
            std::unique_lock lock(waitMutex_);
            wake_.wait_for(lock, std::chrono::seconds(1), [&stop] { return stop.stop_requested(); });
        }
        std::error_code error;
        std::filesystem::remove(destination, error);
    }) {}

RuntimeDiagnostics::~RuntimeDiagnostics() {
    worker_.request_stop(); wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}
}
