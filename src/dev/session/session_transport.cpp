#include "dev/session/session_transport.hpp"
#include "core/json.hpp"
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>

namespace sc13::dev::session {
namespace {
/** @summary Retain one complete command until its correlated result is published. */
struct Request final {
    std::string nonce;
    std::string packet;
    std::int64_t expires{};
    bool running{};
    bool done{};
    ScriptReply reply;
};
std::mutex g_mutex;
std::deque<std::shared_ptr<Request>> g_requests;
std::function<ScriptReply(HWND, std::string_view)> g_executor;
std::function<bool()> g_permitted;
std::atomic<HWND> g_window{};
std::atomic_bool g_verified{};
std::atomic_flag g_pumping = ATOMIC_FLAG_INIT;
std::atomic<UINT> g_message{};

/** @summary Compare expiration against the same Unix-millisecond clock used by DevBridge. */
std::int64_t Now() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
/** @summary Decode an entire decimal field without accepting partial parses. */
template<class T> bool Number(std::string_view text, T& value) noexcept {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
/** @summary Validate optional canonical identifiers before serializing native query parameters. */
bool Identifier(std::string_view text) noexcept {
    if (text.empty()) return true;
    std::uint32_t value{};
    return Number(text, value) && value && text.front() != '0';
}
/** @summary Validate a bounded UTF-8 name without native query separators or control characters. */
bool Name(std::string_view text) noexcept {
    if (text.empty()) return true;
    if (text.size() > 512 || text.find_first_of("/\r\n<>") != std::string_view::npos) return false;
    for (const auto byte : text) if (static_cast<unsigned char>(byte) < 32) return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    return length > 0 && length <= 128;
}
/** @summary Parse only complete small requests; a wakeup never carries caller-owned pointers. */
std::shared_ptr<Request> ReadRequest(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return {};
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 2048) return {};
    std::ifstream input(path, std::ios::binary);
    std::array<std::string, 9> lines;
    bool complete = true;
    for (auto& line : lines) if (!std::getline(input, line)) { complete = false; break; }
    std::string extra;
    const bool additional = static_cast<bool>(std::getline(input, extra));
    input.close();
    std::filesystem::remove(path, error);
    if (!complete || additional || error || lines[0] != "SIMODERSESSION1" || lines[2].size() != 32) return {};
    for (const char byte : lines[2]) if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) return {};
    std::uint32_t processId{};
    std::int64_t expires{};
    if (!Number(lines[1], processId) || processId != GetCurrentProcessId() || !Number(lines[3], expires) ||
        !Identifier(lines[5]) || !Identifier(lines[6]) || !Identifier(lines[7]) || !Name(lines[8])) return {};
    const auto& operation = lines[4];
    if (operation != "observe" && operation != "enter-menu" && operation != "region-templates" && operation != "regions" &&
        operation != "region-details" && operation != "create-sandbox-region" && operation != "claim-city" && operation != "load-city" &&
        operation != "world-units" && operation != "world-unit" && operation != "skip-intro") return {};
    auto request = std::make_shared<Request>();
    request->nonce = lines[2]; request->expires = expires;
    request->packet = "{\"op\":" + core::JsonString(operation) + ",\"regionId\":" + core::JsonString(lines[5]) +
        ",\"cityId\":" + core::JsonString(lines[6]) + ",\"templateId\":" + core::JsonString(lines[7]) +
        ",\"regionName\":" + core::JsonString(lines[8]) + ",\"cityName\":" + core::JsonString(lines[8]) +
        ",\"argument\":" + core::JsonString(lines[8]) + "}";
    if (expires < Now() || expires > Now() + 30000) { request->done = true; request->reply.error = "Native session request expired or has an invalid deadline."; }
    return request;
}
/** @summary Publish only a completed, nonce-specific reply without following an existing linked file. */
bool Publish(const std::filesystem::path& directory, const Request& request) {
    const auto destination = directory / ("session-control-result-" + request.nonce + ".json");
    const auto attributes = GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return false;
    auto temporary = destination; temporary += L".tmp";
    const auto temporaryAttributes = GetFileAttributesW(temporary.c_str());
    if (temporaryAttributes != INVALID_FILE_ATTRIBUTES && (temporaryAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return false;
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << "{\"requestId\":" << core::JsonString(request.nonce) << ",\"pid\":" << GetCurrentProcessId()
        << ",\"success\":" << (request.reply.success ? "true" : "false") << ",\"error\":" << core::JsonString(request.reply.error)
        << ",\"retryable\":" << (request.reply.retryable ? "true" : "false")
        << ",\"threadId\":" << request.reply.threadId << ",\"viewIndex\":" << request.reply.viewIndex
        << ",\"completedAtUnixMs\":" << Now() << ",\"payload\":" << (request.reply.json.empty() ? "null" : request.reply.json) << "}";
    output.close();
    return output && MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
}

void ConfigureSessionTransport(std::function<ScriptReply(HWND, std::string_view)> executor, std::function<bool()> permitted) {
    /* @summary Register the wakeup outside DLL loader lock, during explicit runtime initialization. */
    g_message.store(RegisterWindowMessageW(L"Simoder.NativeSession.Dispatch.v1"));
    std::lock_guard lock(g_mutex); g_executor = std::move(executor); g_permitted = std::move(permitted); g_verified.store(false);
}
void BindSessionWindow(HWND window) noexcept { g_window.store(window); g_verified.store(false); }
bool DispatchSessionMessage(HWND window, UINT message) noexcept {
    const auto wakeup = g_message.load();
    if (!wakeup || message != wakeup) return false;
    if (window != g_window.load() || g_pumping.test_and_set()) return true;
    std::shared_ptr<Request> request;
    try {
        std::function<ScriptReply(HWND, std::string_view)> executor;
        std::function<bool()> permitted;
        {
            std::lock_guard lock(g_mutex);
            for (const auto& candidate : g_requests) if (!candidate->running && !candidate->done) { request = candidate; request->running = true; break; }
            executor = g_executor; permitted = g_permitted;
        }
        if (request) {
            ScriptReply reply;
            if (request->expires < Now()) reply.error = "Native session request expired before game-thread execution.";
            else if (!executor || !permitted || !permitted()) reply.error = "Developer service lease or native adapter is unavailable.";
            else reply = executor(window, request->packet);
            std::lock_guard lock(g_mutex);
            request->reply = std::move(reply); request->done = true;
            if (request->reply.success) g_verified.store(true);
        }
    } catch (...) {
        /* @summary Finish transport failures explicitly so a consumed request cannot remain running forever. */
        if (request) { std::lock_guard lock(g_mutex); request->done = true; request->reply.error = "Native session transport failed before completing its reply."; }
    }
    g_pumping.clear();
    return true;
}
void PollSessionTransport(const std::filesystem::path& directory, bool servicesActive) {
    const auto requestPath = directory / L"session-control.request";
    auto request = servicesActive ? ReadRequest(requestPath) : std::shared_ptr<Request>{};
    std::shared_ptr<Request> rejected;
    std::vector<std::shared_ptr<Request>> completed;
    {
        std::lock_guard lock(g_mutex);
        if (request) {
            if (g_requests.size() >= 8) { request->done = true; request->reply.error = "Native session queue is full."; rejected = std::move(request); }
            else {
                if (!g_executor) { request->done = true; request->reply.error = "Native session adapter unavailable."; }
                g_requests.push_back(std::move(request));
            }
        }
        for (auto iterator = g_requests.begin(); iterator != g_requests.end();) {
            auto& entry = **iterator;
            if (!entry.running && !entry.done && (!servicesActive || entry.expires < Now())) {
                entry.done = true; entry.reply.error = "Developer lease ended or native session deadline elapsed.";
            }
            if (entry.done) completed.push_back(*iterator);
            ++iterator;
        }
    }
    if (rejected) static_cast<void>(Publish(directory, *rejected));
    for (const auto& entry : completed) if (Publish(directory, *entry)) {
        std::lock_guard lock(g_mutex);
        const auto found = std::find(g_requests.begin(), g_requests.end(), entry);
        if (found != g_requests.end()) g_requests.erase(found);
    }
    const HWND window = g_window.load();
    const auto wakeup = g_message.load();
    if (window && wakeup) static_cast<void>(PostMessageW(window, wakeup, 0, 0));
}
std::string SessionTransportJson() {
    std::lock_guard lock(g_mutex);
    return "{\"admitted\":" + std::string(g_executor ? "true" : "false") + ",\"threadVerified\":" +
        (g_verified.load() ? "true" : "false") + ",\"windowReady\":" + (g_window.load() ? "true" : "false") + "}";
}
void ShutdownSessionTransport() noexcept {
    g_window.store(nullptr); g_verified.store(false);
    std::lock_guard lock(g_mutex); g_executor = {}; g_permitted = {};
    for (const auto& request : g_requests) if (!request->running) { request->done = true; request->reply.error = "Native session transport stopped."; }
}
}
