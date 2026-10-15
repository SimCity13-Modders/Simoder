#include "platform/windows_arguments.hpp"
#include "dev/authoring_tools.hpp"
#include "scripting/source_project.hpp"
#include "scripting/luau_compiler.hpp"
#include "dev/service_manifest.hpp"
#include "scripting/script_runtime.hpp"
#include "mods/mod_definition_parser.hpp"
#include "mods/resource_project.hpp"
#include "../../third_party/tomlplusplus/toml.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <stdexcept>
#include <cstring>

namespace sc13::scripting {
namespace {
/** @summary Rejects absent paths and NTFS reparse points before using inputs or cache files. */
void Plain(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(NativePath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Missing path or forbidden link/junction: " + path.string());
}
/** @summary Reads mandatory metadata without silent defaults for malformed values. */
std::string Text(const toml::node_view<const toml::node>& value) {
    const auto result = value.value<std::string>();
    if (!result || result->empty() || result->size() > 1024) throw std::runtime_error("Missing or invalid TOML string");
    return *result;
}

/** @summary Releases native process resources on every error and timeout path. */
struct Handle final {
    HANDLE value{};
    /** @summary Closes one owned native handle. */
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
/** @summary Owns the explicit inherited-handle list so unrelated game handles never reach a compiler. */
struct ProcessAttributes final {
    std::vector<std::byte> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list{};
    /** @summary Builds an attribute list containing exactly the compiler's standard input and output handles. */
    explicit ProcessAttributes(const std::array<HANDLE, 2>& handles) {
        SIZE_T size{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage.resize(size);
        auto candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(candidate, 1, 0, &size)) throw std::runtime_error("Cannot initialize compiler handle isolation");
        list = candidate;
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(handles.data()), sizeof(handles), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(list); list = nullptr;
            throw std::runtime_error("Cannot isolate compiler inherited handles");
        }
    }
    /** @summary Releases the initialized process attribute list before its storage. */
    ~ProcessAttributes() { if (list) DeleteProcThreadAttributeList(list); }
};
/** @summary Retains compiler error tails without reading an unbounded log into memory. */
std::string LogTail(const std::filesystem::path& path) {
    std::ifstream input(NativePath(path), std::ios::binary);
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) return {};
    input.seekg(size > 4096 ? size - std::streamoff(4096) : std::streamoff(0));
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

std::filesystem::path ToolPath(const wchar_t* filename) {
    const std::wstring_view name(filename);
    const auto capability = name == L"simoder-build.exe" ? "compiler" :
        name == L"sc-converter-cli.exe" ? "resource-converter" : "";
    if (*capability) {
        const auto registered = dev::FindAuthoringTool(capability);
        if (!registered.empty()) return registered;
    }
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ToolPath), &module)) throw std::runtime_error("Cannot locate loader module");
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("Cannot locate compiler installation");
    if (module != GetModuleHandleW(nullptr))
        throw std::runtime_error("Authoring tool unavailable; install and enable a developer service mod providing it");
    return std::filesystem::path(path.data()).parent_path() / filename;
}

std::filesystem::path NativePath(const std::filesystem::path& path) {
    auto value = std::filesystem::absolute(path).lexically_normal().native();
    if (value.starts_with(L"\\\\?\\")) return value;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}

bool PublishGeneration(const std::filesystem::path& staging, const std::filesystem::path& generation) {
    const auto source = NativePath(staging), destination = NativePath(generation);
    const auto parent = source.parent_path();
    if (_wcsicmp(parent.c_str(), destination.parent_path().c_str()) != 0 ||
        _wcsicmp(source.c_str(), destination.c_str()) == 0)
        throw std::runtime_error("Generation publication requires distinct siblings");
    /** @summary Read a plain directory's stable identity from an opened, non-following handle.
     * @param handle Open directory handle.
     * @return Volume and file index identity; reparse points and non-directories are rejected.
     */
    const auto identity = [](HANDLE handle) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Publication requires an accessible plain directory");
        return std::array<DWORD, 3>{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
    };
    Handle anchor{CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    const auto sourceIdentity = identity(anchor.value);
    Handle parentAnchor{CreateFileW(parent.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    const auto parentIdentity = identity(parentAnchor.value);
    const auto started = GetTickCount64();
    DWORD failure{}; unsigned attempts{};
    for (;;) {
        ++attempts;
        Handle currentParent{CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        Handle currentSource{CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (identity(currentParent.value) != parentIdentity || identity(currentSource.value) != sourceIdentity)
            throw std::runtime_error("Publication directory identity changed");
        const auto attributes = GetFileAttributesW(destination.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) return false;
        failure = GetLastError();
        if (failure != ERROR_FILE_NOT_FOUND && failure != ERROR_PATH_NOT_FOUND)
            throw std::runtime_error("Cannot inspect publication destination (Win32 error " + std::to_string(failure) + ")");
        {
            Handle rename{CreateFileW(source.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            if (rename.value == INVALID_HANDLE_VALUE) failure = GetLastError();
            else {
                if (identity(rename.value) != sourceIdentity) throw std::runtime_error("Publication staging identity changed");
                const auto bytes = destination.native().size() * sizeof(wchar_t);
                std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t));
                auto info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
                info->ReplaceIfExists = FALSE; info->RootDirectory = nullptr; info->FileNameLength = static_cast<DWORD>(bytes);
                std::memcpy(info->FileName, destination.c_str(), bytes);
                if (SetFileInformationByHandle(rename.value, FileRenameInfo, info, static_cast<DWORD>(storage.size()))) return true;
                failure = GetLastError();
            }
        }
        if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
        const auto elapsed = GetTickCount64() - started;
        if ((failure != ERROR_ACCESS_DENIED && failure != ERROR_SHARING_VIOLATION) || attempts >= 11 || elapsed >= 500) break;
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(50, 500 - elapsed)));
        if (GetTickCount64() - started >= 500) break;
    }
    throw std::runtime_error("Cannot publish compiled generation (Win32 error " + std::to_string(failure) +
        "; attempts=" + std::to_string(attempts) + "; staging=" + staging.string() + "; destination=" + generation.string() + ")");
}

std::filesystem::path TypeScriptToolchainPath() {
    const auto registered = dev::FindAuthoringTool("typescript-toolchain");
    if (!registered.empty()) return registered;
    auto root = ToolPath(L"toolchains") / L"typescript";
    /** @summary Allow authors to use an external toolchain with the lightweight runtime distribution. */
    std::array<wchar_t, 32768> external{};
    const auto length = GetEnvironmentVariableW(L"SIMODER_TYPESCRIPT_TOOLCHAIN", external.data(), static_cast<DWORD>(external.size()));
    if (!std::filesystem::exists(root / L"build.mjs") && length && length < external.size()) root = external.data();
    if (!std::filesystem::exists(root / L"build.mjs")) throw std::runtime_error("TypeScript authoring toolchain is not installed; set SIMODER_TYPESCRIPT_TOOLCHAIN to the installed toolchain directory");
    return root;
}

std::filesystem::path SourcePath(const std::filesystem::path& root, std::string_view relative) {
    Plain(root);
    if (relative.empty() || relative.back() == '/' || relative.find('\\') != std::string_view::npos ||
        relative.find(':') != std::string_view::npos) throw std::runtime_error("Expected portable relative source path");
    auto path = root;
    std::size_t begin{};
    while (begin < relative.size()) {
        const auto end = relative.find('/', begin);
        const auto part = relative.substr(begin, end == relative.npos ? relative.size() - begin : end - begin);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' ||
            std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32 || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|'; }))
            throw std::runtime_error("Unsafe source path component");
        path /= std::filesystem::path(std::u8string(part.begin(), part.end())); Plain(path);
        if (end == relative.npos) return path;
        begin = end + 1;
    }
    throw std::runtime_error("Invalid source path");
}

std::string ReadBounded(const std::filesystem::path& path, std::size_t maximum) {
    Plain(path);
    if (!std::filesystem::is_regular_file(NativePath(path)) || std::filesystem::file_size(NativePath(path)) > maximum)
        throw std::runtime_error("Input file exceeds its limit: " + path.string());
    std::ifstream input(NativePath(path), std::ios::binary);
    std::string result{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || result.size() > maximum) throw std::runtime_error("Failed or oversized source read");
    return result;
}

std::string HashText(std::string_view bytes) {
    core::Sha256Digest hash{}; std::string error;
    if (!core::Sha256(std::as_bytes(std::span(bytes.data(), bytes.size())), hash, error)) throw std::runtime_error(error);
    return core::ToHex(hash);
}

SourceProject InspectSourceProject(const std::filesystem::path& directory) {
    SourceProject result;
    result.directory = std::filesystem::absolute(directory).lexically_normal();
    const auto manifest = SourcePath(result.directory, "mod.toml");
    const auto manifestBytes = ReadBounded(manifest, 8U * 1024U * 1024U);
    const auto document = toml::parse(manifestBytes);
    if (document["schemaVersion"].value<std::int64_t>() != 2 ||
        document["format"].value<std::string>() != "simoder.authoring")
        throw std::runtime_error("Unsupported mod.toml authoring schema");
    result.metadata.id = Text(document["metadata"]["id"]);
    result.metadata.name = Text(document["metadata"]["name"]);
    result.metadata.version = Text(document["metadata"]["version"]);
    result.metadata.author = document["metadata"]["author"].value_or(std::string{});
    result.metadata.description = document["metadata"]["description"].value_or(std::string{});
    if (!mods::IsValidModId(result.metadata.id)) throw std::runtime_error("Invalid source mod ID");
    std::string patchError;
    if (!mods::LoadAuthoringPatches(result.directory, result.patches, patchError)) throw std::runtime_error(patchError);
    std::set<std::filesystem::path> inputs{manifest};
    const auto kind = document["kind"].value_or(std::string("gameplay"));
    if (kind == "developer-services") {
        result.services = dev::ParseServiceManifest(document, result.directory);
        for (const auto& service : result.services->services) inputs.insert(service.executable);
        std::size_t count{};
        for (const auto& item : std::filesystem::recursive_directory_iterator(result.directory)) {
            if (++count > 65536) throw std::runtime_error("Too many developer-service package entries");
            const auto relative = item.path().lexically_relative(result.directory).generic_u8string();
            const auto checked = SourcePath(result.directory,
                std::string_view(reinterpret_cast<const char*>(relative.data()), relative.size()));
            if (item.is_regular_file()) inputs.insert(checked);
        }
    } else if (kind != "gameplay") throw std::runtime_error("Unsupported mod kind");
    if (const auto resources = document["resources"].as_table()) {
        result.hasResources = !resources->empty();
        if (resources->size() > 65536) throw std::runtime_error("Too many resources");
        for (const auto& [alias, node] : *resources) {
            static_cast<void>(alias);
            const auto table = node.as_table();
            if (!table) throw std::runtime_error("Resource definition must be a table");
            inputs.insert(SourcePath(result.directory, Text((*table)["source"])));
        }
    } else if (document.contains("resources")) throw std::runtime_error("resources must be a table");
    if (document.contains("scripts")) {
        if (!document["scripts"].is_table()) throw std::runtime_error("scripts must be a table");
        for (const auto& [key, value] : *document["scripts"].as_table()) {
            static_cast<void>(value);
            if (key != "language" && key != "entry" && key != "memoryMiB" && key != "budgetMs")
                throw std::runtime_error("Unknown script configuration field: " + std::string(key.str()));
        }
        result.language = Text(document["scripts"]["language"]);
        result.entry = Text(document["scripts"]["entry"]);
        if (result.language != "luau" && result.language != "typescript") throw std::runtime_error("Unsupported script language");
        if (!result.entry.starts_with("scripts/") ||
            !result.entry.ends_with(result.language == "luau" ? ".luau" : ".ts"))
            throw std::runtime_error("Script entry must be under scripts/ with the language extension");
        inputs.insert(SourcePath(result.directory, result.entry));
        const auto memory = document["scripts"]["memoryMiB"].value_or(std::int64_t(8));
        const auto budget = document["scripts"]["budgetMs"].value_or(std::int64_t(2));
        if ((document["scripts"]["memoryMiB"] && !document["scripts"]["memoryMiB"].is_integer()) ||
            (document["scripts"]["budgetMs"] && !document["scripts"]["budgetMs"].is_integer()))
            throw std::runtime_error("Script budgets must be integers");
        if (memory < 2 || memory > 64 || budget < 1 || budget > 20) throw std::runtime_error("Script limits: memoryMiB 2..64, budgetMs 1..20");
        result.memoryBytes = static_cast<std::size_t>(memory) * 1024U * 1024U;
        result.budgetMs = static_cast<unsigned>(budget);
    }
    if (!result.hasResources && result.language.empty() && !result.services && result.patches.empty()) throw std::runtime_error("Mod contains no resources, script, patches or services");
    for (const auto* subtree : {".simoder", "scripts"}) {
        const auto root = result.directory / subtree;
        if (!std::filesystem::exists(root)) continue;
        Plain(root);
        std::size_t count{};
        std::uint64_t scriptBytes{};
        for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
            if (++count > 65536) throw std::runtime_error("Too many source entries");
            if (std::string_view(subtree) == "scripts" && count > 256) throw std::runtime_error("At most 256 script entries supported");
            Plain(item.path());
            if (item.is_regular_file()) {
                if (std::string_view(subtree) == "scripts") {
                    scriptBytes += item.file_size();
                    if (scriptBytes > 8U * 1024U * 1024U) throw std::runtime_error("Script sources exceed 8 MiB");
                }
                const auto relative = item.path().lexically_relative(result.directory).generic_u8string();
                inputs.insert(SourcePath(result.directory,
                    std::string_view(reinterpret_cast<const char*>(relative.data()), relative.size())));
            }
        }
    }
    std::string hashes = "simoder-build-v1/luau-0.695/tstl-1.37.1/windows-x86\n";
    std::uint64_t total{};
    for (const auto& file : inputs) {
        const auto size = std::filesystem::file_size(file);
        total += size;
        if (size > 256U * 1024U * 1024U || total > 512ULL * 1024ULL * 1024ULL) throw std::runtime_error("Source project exceeds resource limits");
        core::Sha256Digest hash{}; std::string error;
        if (!core::Sha256File(file, hash, error)) throw std::runtime_error(error);
        const auto relative = file.lexically_relative(result.directory).generic_u8string();
        hashes += std::string(reinterpret_cast<const char*>(relative.data()), relative.size()) + "\n" + std::to_string(size) + "\n" + core::ToHex(hash) + "\n";
    }
    for (const auto* tool : {L"simoder-build.exe", L"sc-converter-cli.exe"}) {
        if (result.services) continue;
        if (result.language.empty() && !result.hasResources) continue;
        if (result.language == "luau" && !result.hasResources) continue;
        if (tool[0] == L's' && tool[1] == L'c' && !result.hasResources) continue;
        const auto path = ToolPath(tool);
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error("Required trusted compiler missing: " + path.string());
        }
        core::Sha256Digest hash{}; std::string error;
        if (!core::Sha256File(path, hash, error)) throw std::runtime_error(error);
        hashes += core::ToHex(hash) + "\n";
    }
    if (result.language == "typescript") {
        const auto toolchain = TypeScriptToolchainPath();
        for (const auto* name : {"build.mjs", "simoder.d.ts", "package-lock.json"})
            hashes += HashText(ReadBounded(SourcePath(toolchain, name), 1024U * 1024U)) + "\n";
    }
    result.fingerprint = HashText(hashes);
    const auto canonical = std::filesystem::canonical(result.directory).generic_u8string();
    result.projectKey = HashText(std::string(reinterpret_cast<const char*>(canonical.data()), canonical.size()) + "\n" + result.metadata.id);
    return result;
}

void RunTool(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
    const std::filesystem::path& log, unsigned timeoutMs) {
    Plain(executable);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output{CreateFileW(NativePath(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Handle nullInput{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr)};
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (output.value == INVALID_HANDLE_VALUE || nullInput.value == INVALID_HANDLE_VALUE || !job.value)
        throw std::runtime_error("Cannot create compiler process resources");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error("Cannot configure compiler process cleanup");
    std::wstring command = platform::QuoteWindowsArgument(executable.native());
    for (const auto& argument : arguments) command += L" " + platform::QuoteWindowsArgument(argument);
    const std::array<HANDLE, 2> inherited{output.value, nullInput.value};
    ProcessAttributes attributes(inherited);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = output.value; startup.StartupInfo.hStdError = output.value; startup.StartupInfo.hStdInput = nullInput.value;
    startup.lpAttributeList = attributes.list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process))
        throw std::runtime_error("Cannot start trusted compiler: Windows error " + std::to_string(GetLastError()));
    Handle processHandle{process.hProcess}; Handle threadHandle{process.hThread};
    if (!AssignProcessToJobObject(job.value, process.hProcess)) {
        TerminateProcess(process.hProcess, 1); throw std::runtime_error("Cannot contain compiler process");
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) throw std::runtime_error("Cannot resume compiler process");
    if (WaitForSingleObject(process.hProcess, timeoutMs) != WAIT_OBJECT_0) {
        TerminateJobObject(job.value, 1);
        WaitForSingleObject(process.hProcess, 5000);
        throw std::runtime_error("Compiler timed out; candidate generation discarded");
    }
    DWORD code{};
    if (!GetExitCodeProcess(process.hProcess, &code) || code != 0)
        throw std::runtime_error("Compiler failed: " + LogTail(log));
}

bool LoadSourceProject(const std::filesystem::path& directory, mods::ModDefinition& definition, std::string& error) noexcept {
    try {
        const auto source = InspectSourceProject(directory);
        if (!source.services && !source.hasResources && source.language.empty()) {
            mods::ModDefinition parsed;
            parsed.directory = source.directory;
            parsed.manifest = source.metadata;
            parsed.patches = source.patches;
            if (InspectSourceProject(source.directory).fingerprint != source.fingerprint)
                throw std::runtime_error("Patch inputs changed during validation");
            definition = std::move(parsed); error.clear(); return true;
        }
        if (source.services) {
            mods::ModDefinition parsed;
            parsed.directory = source.directory;
            parsed.manifest = source.metadata;
            parsed.services = source.services;
            if (InspectSourceProject(source.directory).fingerprint != source.fingerprint)
                throw std::runtime_error("Service inputs changed during validation");
            definition = std::move(parsed);
            error.clear();
            return true;
        }
        const auto cache = source.directory.parent_path() / L"cache";
        std::filesystem::create_directories(NativePath(cache)); Plain(cache);
        const auto projectCache = cache / source.projectKey;
        std::filesystem::create_directories(NativePath(projectCache)); Plain(projectCache);
        const auto generation = projectCache / source.fingerprint;
        if (!std::filesystem::exists(NativePath(generation))) {
            const auto log = projectCache / ("compiler-" + std::to_string(GetCurrentProcessId()) + ".log");
            if (source.language == "luau" && !source.hasResources)
                CompileLuauGeneration(source, generation);
            else
                RunTool(ToolPath(L"simoder-build.exe"), {source.directory.native(), cache.native()}, log);
        }
        const auto descriptor = toml::parse(ReadBounded(SourcePath(generation, "generation.toml"), 65536));
        if (descriptor["schemaVersion"].value<std::int64_t>() != 1 ||
            descriptor["sourceFingerprint"].value<std::string>() != source.fingerprint)
            throw std::runtime_error("Compiled generation is incompatible or stale");
        mods::ModDefinition parsed;
        if (source.hasResources) {
            if (!mods::LoadResourceProject(SourcePath(generation, "runtime"), parsed, error)) throw std::runtime_error(error);
            if (parsed.manifest.id != source.metadata.id) throw std::runtime_error("Compiled native mod identity mismatch");
        }
        parsed.directory = source.directory;
        parsed.manifest = source.metadata;
        parsed.patches = source.patches;
        if (!source.language.empty()) {
            auto script = std::make_shared<ScriptDefinition>();
            script->bytecode = ReadBounded(SourcePath(generation, "script.luau.bin"), 8U * 1024U * 1024U);
            if (HashText(script->bytecode) != descriptor["bytecodeSha256"].value<std::string>())
                throw std::runtime_error("Compiled script checksum mismatch; remove damaged cache generation and rebuild");
            script->entry = source.entry;
            script->language = source.language;
            script->revision = source.fingerprint;
            script->budgetMs = source.budgetMs;
            script->memoryBytes = source.memoryBytes;
            if (!mods::LoadScriptSymbols(source.directory, *script, error)) throw std::runtime_error(error);
            if (const auto modules = descriptor["modules"].as_array()) {
                if (modules->size() > 256) throw std::runtime_error("Too many compiled modules");
                std::size_t totalBytes = script->bytecode.size();
                for (const auto& node : *modules) {
                    const auto table = node.as_table();
                    if (!table) throw std::runtime_error("Invalid compiled module descriptor");
                    const auto name = Text((*table)["name"]);
                    static_cast<void>(SourcePath(source.directory, name));
                    const auto payload = Text((*table)["payload"]);
                    if (!payload.starts_with("modules/")) throw std::runtime_error("Invalid module cache payload");
                    auto bytes = ReadBounded(SourcePath(generation, payload), 8U * 1024U * 1024U);
                    totalBytes += bytes.size();
                    if (totalBytes > 8U * 1024U * 1024U || HashText(bytes) != Text((*table)["sha256"]))
                        throw std::runtime_error("Module checksum mismatch or generation too large");
                    if (!script->modules.emplace(name, std::move(bytes)).second) throw std::runtime_error("Duplicate compiled module");
                }
            }
            parsed.script = std::move(script);
        }
        if (InspectSourceProject(source.directory).fingerprint != source.fingerprint)
            throw std::runtime_error("Source changed during compilation; active generation retained");
        definition = std::move(parsed); error.clear(); return true;
    } catch (const std::exception& exception) { definition = {}; error = "mod.toml: " + std::string(exception.what()); return false; }
    catch (...) { definition = {}; error = "Source project preparation failed"; return false; }
}
}
