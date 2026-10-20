#include "mods/native/native_module_loader.hpp"
#include "mods/native/native_participant.hpp"
#include "core/sha256.hpp"
#include "platform/runtime_owner.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace sc13::mods::native {
namespace {
/** @summary Every successfully opened verification handle closes through ordinary RAII until retained. */
struct File final {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit File(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
/** @summary Verified file bytes, original identity and ancestor leases prevent replacement during loading. */
struct PinnedFile final {
    std::vector<std::shared_ptr<File>> leases;
    std::vector<unsigned char> bytes;
    BY_HANDLE_FILE_INFORMATION identity{};
};
/** @summary Import thunk slot and trusted resolved address for loaded-image substitution detection. */
struct Import final { DWORD rva{}; FARPROC address{}; };
/** @summary Immutable validated PE facts required before any module initializer executes. */
struct PeFacts final { DWORD imageSize{}; std::array<DWORD, 5> exports{}; std::vector<Import> imports; };
/** @summary Loaded images are deliberately never released, including failed module initialization. */
struct Image final {
    HMODULE module{};
    std::array<unsigned char, 32> digest{};
    PeFacts facts;
    PinnedFile file;
    std::wstring path;
    SdModuleQueryV2 query{};
    SdModulePrepareV2 prepare{};
    SdModuleAdmitV2 admit{};
    SdModuleQuiesceV2 quiesce{};
    SdModuleDrainV2 drain{};
};
/** @summary Registry state is intentionally process-retained to protect foreign callbacks and borrowed tables. */
struct Registry final {
    std::mutex mutex;
    bool loading{};
    std::vector<std::shared_ptr<Image>> images;
    std::vector<std::shared_ptr<NativeModule::State>> states;
    std::size_t mappedBytes{};
    std::size_t stagingAttempts{};
    bool poisoned{};
};
/** @summary Avoids static destruction while native threads may still retain host pointers. @return Permanent bounded registry. */
Registry& Global() { static auto* registry = new Registry; return *registry; }
constexpr std::size_t MaxImages =
#if defined(SIMODER_NATIVE_TEST_HOST) && defined(SIMODER_NATIVE_TEST_IMAGE_LIMIT)
    SIMODER_NATIVE_TEST_IMAGE_LIMIT;
#else
    8;
#endif
constexpr std::size_t MaxMappedBytes = 64 * 1024 * 1024;
constexpr std::size_t MaxFileBytes = 16 * 1024 * 1024;
constexpr std::size_t MaxGenerations = 128;
constexpr std::size_t MaxSubscriptions = 64;
constexpr const char* ExportNames[] = {"SimoderModule_QueryV2", "SimoderModule_PrepareV2", "SimoderModule_AdmitV2", "SimoderModule_QuiesceV2", "SimoderModule_DrainV2"};

/** @summary Validates a normal drive-absolute path without alternate streams, dot segments or device aliases. */
bool PlainPath(const std::filesystem::path& input) {
    const auto path = input.native();
    if (path.size() < 4 || path.size() > 220 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/') ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))) return false;
    if (path.find_first_of(L"*?\"<>|") != std::wstring::npos || path.find(L':', 2) != std::wstring::npos) return false;
    for (const auto& part : input.relative_path()) {
        auto text = part.native();
        if (text.empty() || text == L"." || text == L".." || text.back() == L'.' || text.back() == L' ' || text.find(L'\0') != std::wstring::npos) return false;
    }
    return true;
}
/** @summary Pins every ancestor against rename/delete and rejects all reparse points. */
bool PinAncestors(const std::filesystem::path& path, PinnedFile& output) {
    auto current = path.root_path();
    auto lease = [&](const std::filesystem::path& directory) {
        auto file = std::make_shared<File>(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        FILE_ATTRIBUTE_TAG_INFO info{};
        if (file->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(file->value, FileAttributeTagInfo, &info, sizeof(info)) ||
            !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        output.leases.push_back(std::move(file));
        return true;
    };
    if (!lease(current)) return false;
    for (const auto& component : path.parent_path().relative_path()) { current /= component; if (!lease(current)) return false; }
    return true;
}
/** @summary Pins an ordinary single-link file for read-only content verification without write/delete sharing. */
bool PinFile(const std::filesystem::path& path, PinnedFile& output) {
    if (!PlainPath(path) || !PinAncestors(path, output)) return false;
    auto file = std::make_shared<File>(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file->value, &output.identity) ||
        (output.identity.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        output.identity.nNumberOfLinks != 1 || output.identity.nFileSizeHigh || output.identity.nFileSizeLow > MaxFileBytes || !output.identity.nFileSizeLow) return false;
    output.bytes.resize(output.identity.nFileSizeLow);
    DWORD read{};
    if (!ReadFile(file->value, output.bytes.data(), static_cast<DWORD>(output.bytes.size()), &read, nullptr) || read != output.bytes.size()) return false;
    output.leases.push_back(std::move(file));
    return true;
}
/** @summary Computes exact SHA256 with the Windows provider, without external executables. */
bool Digest(const std::vector<unsigned char>& bytes, std::array<unsigned char, 32>& output) {
    core::Sha256Digest digest{};
    std::string error;
    if (!core::Sha256(std::as_bytes(std::span(bytes)), digest, error)) return false;
    std::transform(digest.begin(), digest.end(), output.begin(), [](std::byte value) { return std::to_integer<unsigned char>(value); });
    return true;
}
/** @summary Bounds-checks a copied structure without potentially unaligned mapped dereferences. */
template<class T> bool Read(const std::vector<unsigned char>& bytes, std::size_t offset, T& output) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&output, bytes.data() + offset, sizeof(T)); return true;
}
/** @summary Returns trusted System32 directory plus delimiter with no environment lookup. */
std::wstring SystemDirectory() {
    wchar_t path[MAX_PATH]{}; const auto size = GetSystemDirectoryW(path, MAX_PATH);
    if (!size || size >= MAX_PATH) return {}; return std::wstring(path, size) + L"\\";
}
/** @summary Verifies a resolved forwarder target resides directly in the trusted Windows system directory. */
bool SystemAddress(FARPROC address) {
    MEMORY_BASIC_INFORMATION info{}; wchar_t path[MAX_PATH]{};
    if (!address || !VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) return false;
    const auto count = GetModuleFileNameW(static_cast<HMODULE>(info.AllocationBase), path, MAX_PATH);
    auto root = SystemDirectory();
    if (!count || count >= MAX_PATH || root.empty()) return false;
    const auto parent = std::filesystem::path(path).parent_path().native() + L"\\";
    return _wcsicmp(parent.c_str(), root.c_str()) == 0;
}
/** @summary Parses bounded PE32 structures and rejects imports, redirections and exports outside the frozen policy. */
bool ValidatePe(const std::vector<unsigned char>& bytes, PeFacts& facts) {
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS32 nt{};
    if (!Read(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
        !Read(bytes, static_cast<std::size_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER32) ||
        !(nt.FileHeader.Characteristics & IMAGE_FILE_DLL) || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > 32 ||
        nt.OptionalHeader.NumberOfRvaAndSizes != IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > MaxMappedBytes ||
        nt.OptionalHeader.SizeOfHeaders > bytes.size() || nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.AddressOfEntryPoint) return false;
    const auto fileAlignment = nt.OptionalHeader.FileAlignment;
    const auto sectionAlignment = nt.OptionalHeader.SectionAlignment;
    if (fileAlignment < 512 || fileAlignment > 65536 || (fileAlignment & (fileAlignment - 1)) ||
        sectionAlignment < 4096 || sectionAlignment > 65536 || (sectionAlignment & (sectionAlignment - 1)) ||
        fileAlignment > sectionAlignment || nt.OptionalHeader.SizeOfImage % sectionAlignment || nt.OptionalHeader.SizeOfHeaders % fileAlignment) return false;
    for (std::size_t i = 0; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; ++i) {
        const auto& directory = nt.OptionalHeader.DataDirectory[i];
        const auto extent = i == IMAGE_DIRECTORY_ENTRY_SECURITY ? bytes.size() : nt.OptionalHeader.SizeOfImage;
        if (directory.VirtualAddress > extent || directory.Size > extent - directory.VirtualAddress) return false;
    }
    for (auto directory : {IMAGE_DIRECTORY_ENTRY_RESOURCE, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT, IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR, IMAGE_DIRECTORY_ENTRY_TLS})
        if (nt.OptionalHeader.DataDirectory[directory].VirtualAddress || nt.OptionalHeader.DataDirectory[directory].Size) return false;
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    const auto sectionOffset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(nt);
    for (std::size_t i = 0; i < sections.size(); ++i) {
        auto& section = sections[i]; if (!Read(bytes, sectionOffset + i * sizeof(section), section)) return false;
        if (section.VirtualAddress < nt.OptionalHeader.SizeOfHeaders || section.VirtualAddress >= nt.OptionalHeader.SizeOfImage ||
            std::max(section.Misc.VirtualSize, section.SizeOfRawData) > nt.OptionalHeader.SizeOfImage - section.VirtualAddress ||
            section.PointerToRawData > bytes.size() || section.SizeOfRawData > bytes.size() - section.PointerToRawData) return false;
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && (section.Characteristics & IMAGE_SCN_MEM_WRITE)) return false;
        for (std::size_t j = 0; j < i; ++j) {
            const auto& other = sections[j];
            const auto extent = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
            const auto previousExtent = std::max(other.Misc.VirtualSize, other.SizeOfRawData);
            if (static_cast<std::uint64_t>(section.VirtualAddress) < static_cast<std::uint64_t>(other.VirtualAddress) + previousExtent &&
                static_cast<std::uint64_t>(other.VirtualAddress) < static_cast<std::uint64_t>(section.VirtualAddress) + extent) return false;
        }
    }
    auto offset = [&](DWORD rva, std::size_t length, std::size_t& result) {
        if (rva >= nt.OptionalHeader.SizeOfImage || length > nt.OptionalHeader.SizeOfImage - rva) return false;
        for (const auto& section : sections) {
            if (rva < section.VirtualAddress) continue;
            const auto delta = rva - section.VirtualAddress;
            if (delta <= section.SizeOfRawData && length <= section.SizeOfRawData - delta) { result = section.PointerToRawData + delta; return true; }
        }
        return false;
    };
    auto text = [&](DWORD rva, std::string& value) {
        value.clear(); std::size_t at{};
        for (DWORD i = 0; i < 256; ++i) {
            if (rva > MAXDWORD - i || !offset(rva + i, 1, at)) return false;
            const auto ch = bytes[at]; if (!ch) return !value.empty();
            if (ch < 32 || ch > 126) return false; value.push_back(static_cast<char>(ch));
        } return false;
    };
    auto mapped = [&](std::uint64_t rva, auto& value) { std::size_t at{}; return rva <= MAXDWORD && offset(static_cast<DWORD>(rva), sizeof(value), at) && Read(bytes, at, value); };
    auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress) {
        if (directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) || directory.Size > 4096) return false;
        bool ended{};
        for (DWORD i = 0; i < directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR); ++i) {
            IMAGE_IMPORT_DESCRIPTOR descriptor{};
            if (!mapped(static_cast<std::uint64_t>(directory.VirtualAddress) + i * sizeof(descriptor), descriptor)) return false;
            if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk && !descriptor.TimeDateStamp && !descriptor.ForwarderChain) { ended = true; break; }
            std::string name; if (!text(descriptor.Name, name) || _stricmp(name.c_str(), "kernel32.dll") || descriptor.TimeDateStamp || descriptor.ForwarderChain || !descriptor.OriginalFirstThunk || !descriptor.FirstThunk) return false;
            const auto kernel = GetModuleHandleW(L"kernel32.dll");
            wchar_t kernelPath[MAX_PATH]{};
            if (!kernel || !GetModuleFileNameW(kernel, kernelPath, MAX_PATH) ||
                _wcsicmp(std::filesystem::path(kernelPath).parent_path().native().c_str(), std::filesystem::path(SystemDirectory()).parent_path().native().c_str())) return false;
            bool thunkEnd{};
            for (DWORD t = 0; t < 4096; ++t) {
                DWORD thunk{}; if (!mapped(static_cast<std::uint64_t>(descriptor.OriginalFirstThunk) + t * sizeof(DWORD), thunk)) return false;
                if (!thunk) { thunkEnd = true; break; }
                if (IMAGE_SNAP_BY_ORDINAL32(thunk)) return false;
                std::string symbol; if (thunk > MAXDWORD - 2 || !text(thunk + 2, symbol)) return false;
                auto address = GetProcAddress(kernel, symbol.c_str());
                DWORD initial{};
                if (!SystemAddress(address) || !mapped(static_cast<std::uint64_t>(descriptor.FirstThunk) + t * sizeof(DWORD), initial)) return false;
                facts.imports.push_back({descriptor.FirstThunk + t * sizeof(DWORD), address});
                if (facts.imports.size() > 4096) return false;
            }
            if (!thunkEnd) return false;
        }
        if (!ended) return false;
    } else if (directory.Size) return false;
    directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY exports{};
    if (!directory.VirtualAddress || !directory.Size || !mapped(directory.VirtualAddress, exports) ||
        exports.NumberOfNames > 128 || exports.NumberOfFunctions > 128 || exports.NumberOfNames < 5) return false;
    std::array<bool, 5> found{};
    for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
        DWORD nameRva{}, function{}; WORD ordinal{}; std::string name;
        if (!mapped(static_cast<std::uint64_t>(exports.AddressOfNames) + i * 4, nameRva) || !text(nameRva, name) || !mapped(static_cast<std::uint64_t>(exports.AddressOfNameOrdinals) + i * 2, ordinal) ||
            ordinal >= exports.NumberOfFunctions || !mapped(static_cast<std::uint64_t>(exports.AddressOfFunctions) + ordinal * 4, function)) return false;
        for (std::size_t e = 0; e < found.size(); ++e) if (name == ExportNames[e]) {
            if (found[e] || function >= nt.OptionalHeader.SizeOfImage ||
                (function >= directory.VirtualAddress && static_cast<std::uint64_t>(function) < static_cast<std::uint64_t>(directory.VirtualAddress) + directory.Size)) return false;
            bool executable{};
            for (const auto& section : sections) if (function >= section.VirtualAddress && function - section.VirtualAddress < section.Misc.VirtualSize && (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) executable = true;
            if (!executable) return false; facts.exports[e] = function; found[e] = true;
        }
    }
    if (!std::all_of(found.begin(), found.end(), [](bool present) { return present; })) return false;
    facts.imageSize = nt.OptionalHeader.SizeOfImage; return true;
}
/** @summary Compares immutable file identities without trusting path text alone. */
bool SameFile(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}
/** @summary Copies reviewed bytes to a unique non-overwriting immutable path, then reopens and revalidates it. */
bool Stage(const std::filesystem::path& root, const PinnedFile& source, const std::array<unsigned char, 32>& digest, Image& image) {
    if (!PlainPath(root)) return false;
    PinnedFile rootLease;
    if (!PinAncestors(root / L"placeholder.dll", rootLease)) return false;
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    const wchar_t hex[] = L"0123456789abcdef"; std::wstring name = L"native-";
    for (auto value : random) { name += hex[value >> 4]; name += hex[value & 15]; }
    const auto directory = root / name;
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return false;
    const auto path = directory / L"module.dll";
    BY_HANDLE_FILE_INFORMATION original{};
    {
        File target(CreateFileW(path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_READONLY, nullptr));
        DWORD wrote{};
        if (target.value == INVALID_HANDLE_VALUE || !WriteFile(target.value, source.bytes.data(), static_cast<DWORD>(source.bytes.size()), &wrote, nullptr) ||
            wrote != source.bytes.size() || !FlushFileBuffers(target.value) || !GetFileInformationByHandle(target.value, &original)) return false;
    }
    PinnedFile staged;
    std::array<unsigned char, 32> actual{};
    if (!PinFile(path, staged) || !SameFile(original, staged.identity) || !Digest(staged.bytes, actual) || actual != digest) return false;
    image.path = path.native(); image.file = std::move(staged);
    return true;
}
/** @summary Converts contract identity hex, with no permissive parsing or native-mod input. */
std::array<unsigned char, 32> ContractIdentity() {
    std::array<unsigned char, 32> result{}; constexpr char text[] = SD_CONTRACT_SHA256_HEX;
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto digit = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
        result[i] = static_cast<unsigned char>((digit(text[i * 2]) << 4) | digit(text[i * 2 + 1]));
    } return result;
}
}

/** @summary A retained owner, factual publisher and immutable tables outlive every foreign callback. */
struct NativeModule::State final {
    struct Subscription final { runtime::RuntimeLifecycle::Subscription id{}; std::uint32_t generation{1}; bool used{}; };
    std::mutex mutex;
    /** @summary Sole coherent local phase/retirement source; retirement never clears. */
    std::atomic<std::uint32_t> lifetime{static_cast<std::uint32_t>(ModulePhase::Prepared)};
    /** @summary Published only after successful foreign Prepare and final original-owner checks. */
    std::atomic_bool prepared{};
    /** @summary Original scope generation staged once before foreign Prepare, never refreshed by a fence. */
    std::uint64_t scopeGeneration{};
    std::atomic_bool calling{};
    std::atomic<std::size_t> subscribing{};
    std::atomic_bool quiesced{};
    std::shared_ptr<Image> image;
    std::string moduleId;
    std::shared_ptr<runtime::RuntimeScope> scope;
    std::shared_ptr<runtime::RuntimeCommitDomain> domain;
    std::shared_ptr<runtime::RuntimeLifecycle> lifecycle;
    SdHostApiV2 host{};
    std::array<Subscription, MaxSubscriptions> subscriptions{};
    static constexpr std::uint32_t Retired = 0x100;
    static constexpr std::uint32_t PhaseMask = 0xff;
    /** @summary Reads the phase from the sole local word. @return Current cooperative phase. */
    ModulePhase Phase() const noexcept { return static_cast<ModulePhase>(lifetime.load() & PhaseMask); }
    /** @summary Closes the local lifetime before any loader-owned stop request or foreign cleanup. */
    void Retire() noexcept { lifetime.fetch_or(Retired); }
    /** @summary Publishes a valid phase while preserving sticky retirement on every CAS.
     * @param next Actual cooperative lifecycle phase. @return False for invalid or terminal reversal. */
    bool PublishPhase(ModulePhase next) noexcept {
        if (next < ModulePhase::Prepared || next > ModulePhase::Faulted) return false;
        auto original = lifetime.load();
        do {
            const auto before = static_cast<ModulePhase>(original & PhaseMask);
            const bool valid = before == next ||
                (before == ModulePhase::Prepared && (next == ModulePhase::Active || next == ModulePhase::Stopping || next == ModulePhase::Faulted)) ||
                (before == ModulePhase::Active && (next == ModulePhase::Stopping || next == ModulePhase::Faulted)) ||
                (before == ModulePhase::Faulted && next == ModulePhase::Stopping) ||
                (before == ModulePhase::Stopping && next == ModulePhase::Drained);
            if (!valid) return false;
            auto desired = (original & Retired) | static_cast<std::uint32_t>(next);
            if (next == ModulePhase::Stopping || next == ModulePhase::Drained || next == ModulePhase::Faulted) desired |= Retired;
            if (lifetime.compare_exchange_weak(original, desired)) return true;
        } while (true);
    }
};

namespace {
/** @summary Resolves exact public slot/generation without truncating U1's internal generation. */
std::shared_ptr<NativeModule::State> Find(SdHandle owner) {
    auto& registry = Global(); const std::lock_guard lock(registry.mutex);
    if (!owner.slot || owner.slot > registry.states.size() || owner.generation != 1) return {};
    return registry.states[owner.slot - 1];
}
/** @summary Produces sanitized factual output; native caller must provide valid storage. */
SdState Snapshot(const NativeModule::State& state, const runtime::LifecycleSnapshot& lifecycle) {
    const auto scope = state.scope->Current();
    SdState output{{sizeof(SdState), 1}, scope.epoch, lifecycle.sequence, scope.revision, SD_STATE_COLD, 0};
    if (lifecycle.runtimePrepared) output.readiness_flags |= SD_READY_RUNTIME;
    if (lifecycle.menuObserved) output.readiness_flags |= SD_READY_MENU;
    if (lifecycle.cityObserved) output.readiness_flags |= SD_READY_CITY;
    output.lifecycle = lifecycle.stopping ? SD_STATE_STOPPING : lifecycle.runtimePrepared ? SD_STATE_READY : SD_STATE_STARTING;
    return output;
}
/** @summary Copies lifecycle facts only while the issuing generation has not stopped. */
SdResult SD_CALL LifecycleSnapshot(SdHandle owner, SdState* output) try {
    if (!output) return SD_INVALID;
    const auto header = output->header; *output = {}; output->header = header;
    if (header.size != sizeof(SdState) || header.version != 1) return SD_VERSION;
    auto state = Find(owner); if (!state || state->scope->Current().stopping || state->domain->IsStopping() || state->lifecycle->Current().stopping) return SD_STALE;
    *output = Snapshot(*state, state->lifecycle->Current()); return SD_OK;
} catch (...) { if (output) { const auto header = output->header; *output = {}; output->header = header; } return SD_OPERATION_FAILED; }
/** @summary Registers factual callbacks; callback code must obey cooperative nonblocking and no-Lua-entry rules. */
SdResult SD_CALL Subscribe(SdHandle owner, SdStateCallback callback, void* user, SdHandle* output) try {
    if (output) *output = {}; if (!output || !callback) return SD_INVALID;
    auto state = Find(owner); if (!state) return SD_STALE;
    state->subscribing.fetch_add(1);
    struct Registration final { std::atomic<std::size_t>& count; ~Registration() { count.fetch_sub(1); } } registration{state->subscribing};
    std::size_t slot{};
    {
        const std::lock_guard lock(state->mutex);
        if (state->scope->Current().stopping || state->domain->IsStopping() || state->lifecycle->Current().stopping) return SD_STALE;
        for (; slot < state->subscriptions.size(); ++slot) if (!state->subscriptions[slot].used && state->subscriptions[slot].generation) break;
        if (slot == state->subscriptions.size()) return SD_RESOURCE_LIMIT;
        state->subscriptions[slot].used = true;
    }
    runtime::RuntimeLifecycle::Subscription id{};
    try {
        id = state->lifecycle->SubscribeCurrent([state, callback, user](const runtime::LifecycleSnapshot& observed) {
            if (state->scope->Current().stopping || state->domain->IsStopping() || state->lifecycle->Current().stopping) return;
            const auto snapshot = Snapshot(*state, observed);
            try { callback(user, &snapshot); } catch (...) { state->Retire(); state->scope->RequestStop(runtime::StopReason::ActivationFailed); }
        });
    } catch (...) {
        const std::lock_guard lock(state->mutex); state->subscriptions[slot].used = false;
        return SD_OPERATION_FAILED;
    }
    {
        const std::lock_guard lock(state->mutex);
        auto& record = state->subscriptions[slot];
        record.id = id;
        if (!id) { record.used = false; return SD_RESOURCE_LIMIT; }
        *output = {owner.slot * static_cast<std::uint32_t>(MaxSubscriptions) + static_cast<std::uint32_t>(slot), record.generation};
    }
    if (state->scope->Current().stopping || state->domain->IsStopping() || state->lifecycle->Current().stopping) {
        state->lifecycle->Unsubscribe(id); *output = {}; return SD_STALE;
    }
    return SD_OK;
} catch (...) { if (output) *output = {}; return SD_OPERATION_FAILED; }
/** @summary Closes callback admission and preserves in-flight callback state until a later successful cleanup. */
SdResult SD_CALL Unsubscribe(SdHandle handle) try {
    const auto ownerSlot = handle.slot / MaxSubscriptions; const auto slot = handle.slot % MaxSubscriptions;
    auto state = Find({static_cast<std::uint32_t>(ownerSlot), 1}); if (!state || !handle.generation) return SD_STALE;
    runtime::RuntimeLifecycle::Subscription id{};
    {
        const std::lock_guard lock(state->mutex); const auto& entry = state->subscriptions[slot];
        if (!entry.used || entry.generation != handle.generation || !entry.id) return SD_STALE; id = entry.id;
    }
    if (!state->lifecycle->Unsubscribe(id)) return SD_BUSY;
    {
        const std::lock_guard lock(state->mutex); auto& entry = state->subscriptions[slot];
        if (entry.generation != handle.generation || !entry.used) return SD_STALE;
        entry.used = false; entry.id = 0;
        entry.generation = entry.generation == MAXDWORD ? 0 : entry.generation + 1;
    } return SD_OK;
} catch (...) { return SD_OPERATION_FAILED; }
/** @summary Immutable sole lifecycle implementation; no authority/semantic facets exist in U2b. */
const SdLifecycleApi LifecycleApi{{sizeof(SdLifecycleApi), 1}, LifecycleSnapshot, Subscribe, Unsubscribe};
/** @summary Reserves one foreign lifecycle call without holding a host lock across native code. */
struct Call final {
    std::atomic_bool& flag; bool acquired;
    explicit Call(std::atomic_bool& value) : flag(value), acquired(!flag.exchange(true)) {}
    ~Call() { if (acquired) flag.store(false); }
};
/** @summary Calls trusted cooperative code, containing ordinary C++ exceptions but not claiming native fault isolation. */
template<class Function, class... Args> SdResult Invoke(Function function, Args... args) {
    try { const auto result = function(args...); return static_cast<int>(result) >= 0 && static_cast<int>(result) <= 16 ? result : SD_OPERATION_FAILED; }
    catch (...) { return SD_OPERATION_FAILED; }
}
}

NativeModule::NativeModule(std::shared_ptr<State> state) : state_(std::move(state)) {}
const SdHostApiV2& NativeModule::Host() const noexcept { return state_->host; }
ModulePhase NativeModule::Phase() const noexcept { return state_->Phase(); }
NativeParticipant::NativeParticipant(std::shared_ptr<NativeModule::State> state) noexcept : state_(std::move(state)) {}
NativeParticipantFence NativeParticipant::ObserveLocalFence() const noexcept {
    const auto word = state_->lifetime.load();
    return {{state_->host.owner, state_->image->digest, state_->scopeGeneration, state_->host.owner.slot},
        static_cast<ModulePhase>(word & NativeModule::State::PhaseMask),
        (word & NativeModule::State::Retired) != 0, state_->domain->IsStopping()};
}
bool NativeParticipant::SameIdentity(const NativeParticipant& other) const noexcept {
    return state_ == other.state_;
}
bool NativeParticipant::StageLifetime(std::shared_ptr<runtime::RuntimeScope>& scope,
    std::shared_ptr<runtime::RuntimeLifecycle>& lifecycle) const {
    scope.reset();lifecycle.reset();
    if(!state_->prepared.load() || !ObserveLocalFence().LocallyLive())return false;
    auto originalScope=state_->scope;auto originalLifecycle=state_->lifecycle;
    const auto observed=originalScope->Current();
    if(observed.stopping || observed.generation!=state_->scopeGeneration || originalLifecycle->Current().stopping ||
        !ObserveLocalFence().LocallyLive())return false;
    scope=std::move(originalScope);lifecycle=std::move(originalLifecycle);return true;
}
SdResult NativeModule::CaptureParticipant(std::optional<NativeParticipant>& output) const {
    output.reset();
    if (!state_->prepared.load()) return SD_NOT_READY;
    // @summary These off-guard observations deny known external stop without claiming an atomic whole-runtime fence.
    if (state_->scope->Current().stopping || state_->domain->IsStopping() || state_->lifecycle->Current().stopping ||
        (state_->lifetime.load() & State::Retired)) return SD_STALE;
    output = NativeParticipant(state_);
    return SD_OK;
}
SdResult NativeModule::Admit() {
    return (state_->lifetime.load() & State::Retired) || state_->scope->Current().stopping || state_->domain->IsStopping() || state_->lifecycle->Current().stopping ? SD_STALE : SD_NOT_READY;
}
#if defined(SIMODER_NATIVE_TEST_HOST)
void NativeParticipantTestAccess::StopOriginalScope(NativeModule& module) noexcept {
    module.state_->scope->RequestStop();
}
SdResult NativeModule::AdmitSynthetic() {
    SdResult result{};
    {
        Call call(state_->calling); if (!call.acquired) return SD_BUSY;
        if ((state_->lifetime.load() & State::Retired) || state_->Phase() != ModulePhase::Prepared || state_->scope->Current().stopping || state_->domain->IsStopping() || state_->lifecycle->Current().stopping) { state_->Retire(); return SD_STALE; }
        result = state_->scope->CommitActivation() ? Invoke(state_->image->admit, state_->host.owner) : SD_NOT_READY;
        if (result == SD_OK && (state_->scope->Current().stopping || state_->domain->IsStopping() || state_->lifecycle->Current().stopping)) result = SD_INDETERMINATE;
        state_->PublishPhase(result == SD_OK ? ModulePhase::Active : ModulePhase::Faulted);
        if (result != SD_OK) state_->scope->RequestStop(runtime::StopReason::ActivationFailed);
    }
    if (result != SD_OK) { Quiesce(); Drain(); }
    return result;
}
#endif
SdResult NativeModule::Quiesce() {
    state_->Retire();
    state_->scope->RequestStop();
    Call call(state_->calling); if (!call.acquired) return SD_BUSY;
    if (state_->Phase() == ModulePhase::Drained) return SD_OK;
    state_->PublishPhase(ModulePhase::Stopping);
    const auto result = Invoke(state_->image->quiesce, state_->host.owner);
    if (result == SD_OK) state_->quiesced = true;
    return result;
}
SdResult NativeModule::Drain() {
    if (!state_->scope->Current().stopping) return SD_NOT_READY;
    Call call(state_->calling); if (!call.acquired) return SD_BUSY;
    if (state_->Phase() == ModulePhase::Drained) return SD_OK;
    if (!state_->quiesced || state_->subscribing.load()) return SD_BUSY;
    for (std::size_t i = 0; i < state_->subscriptions.size(); ++i) {
        SdHandle handle{};
        { const std::lock_guard lock(state_->mutex); const auto& entry = state_->subscriptions[i];
          if (entry.used && entry.id) handle = {state_->host.owner.slot * static_cast<std::uint32_t>(MaxSubscriptions) + static_cast<std::uint32_t>(i), entry.generation}; }
        if (handle.slot) { const auto retired = Unsubscribe(handle); if (retired != SD_OK && retired != SD_STALE) return retired; }
    }
    const auto result = Invoke(state_->image->drain, state_->host.owner);
    if (result != SD_OK) return result;
    if (!state_->scope->DrainFor(std::chrono::milliseconds(0))) return SD_BUSY;
    state_->PublishPhase(ModulePhase::Drained); return SD_OK;
}
NativeModuleLoader::NativeModuleLoader(std::filesystem::path root, std::shared_ptr<platform::RuntimeOwner> owner,
    std::shared_ptr<runtime::RuntimeCommitDomain> domain, std::shared_ptr<runtime::RuntimeLifecycle> lifecycle)
    : stagingRoot_(std::move(root)), owner_(std::move(owner)), domain_(std::move(domain)), lifecycle_(std::move(lifecycle)) {}

SdResult NativeModuleLoader::Prepare(const ReviewedModule& registration, std::shared_ptr<NativeModule>& output) {
    output.reset(); error_.clear();
    auto fail = [&](SdResult result, const char* message) { error_ = message; return result; };
    if (!registration.nativeConsentApproved) return fail(SD_DENIED, "Native execution consent is absent");
    if (registration.moduleId.empty() || registration.moduleId.size() > 128 || !std::all_of(registration.moduleId.begin(), registration.moduleId.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
    })) return fail(SD_INVALID, "Core module identity is invalid");
    if (!owner_ || !owner_->IsAcquired() || !domain_ || domain_->IsStopping() || !lifecycle_ || lifecycle_->Current().stopping) return fail(SD_NOT_READY, "Qualified runtime ownership is absent");
    PinnedFile source; std::array<unsigned char, 32> digest{}; PeFacts facts;
    if (!PinFile(registration.absolutePath, source) || !Digest(source.bytes, digest) || digest != registration.sha256) return fail(SD_DENIED, "Path, file lease or reviewed content pin rejected");
    if (!ValidatePe(source.bytes, facts)) return fail(SD_DENIED, "Native PE/import/export policy rejected");
    auto& registry = Global(); std::shared_ptr<Image> image;
    {
        const std::lock_guard lock(registry.mutex);
        if (registry.loading) return fail(SD_BUSY, "Another native image is being verified/loaded");
        if (registry.poisoned) return fail(SD_RESTART_REQUIRED, "A failed native initialization requires process restart");
        if (registry.states.size() >= MaxGenerations) return fail(SD_RESTART_REQUIRED, "Native owner retention budget exhausted");
        for (const auto& state : registry.states) if (state->moduleId == registration.moduleId && state->Phase() != ModulePhase::Drained) return fail(SD_CONFLICT, "A logical module generation has not drained");
        for (const auto& candidate : registry.images) if (candidate->digest == digest) { image = candidate; break; }
        if (!image && (registry.images.size() >= MaxImages || facts.imageSize > MaxMappedBytes - registry.mappedBytes)) return fail(SD_RESTART_REQUIRED, "Native image retention budget exhausted");
        if (!image && registry.stagingAttempts >= MaxGenerations) return fail(SD_RESTART_REQUIRED, "Native staging attempt budget exhausted");
        if (!image) ++registry.stagingAttempts;
        registry.loading = true;
    }
    struct LoadingGuard final { Registry& registry; ~LoadingGuard() { const std::lock_guard lock(registry.mutex); registry.loading = false; } } guard{registry};
    if (!image) {
        image = std::make_shared<Image>(); image->digest = digest; image->facts = std::move(facts);
        if (!Stage(stagingRoot_, source, digest, *image)) return fail(SD_DENIED, "Immutable native staging failed");
        {
            const std::lock_guard lock(registry.mutex);
            registry.images.push_back(image); registry.mappedBytes += image->facts.imageSize;
        }
        image->module = LoadLibraryExW(image->path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!image->module) {
            const std::lock_guard lock(registry.mutex); registry.poisoned = true;
            return fail(SD_RESTART_REQUIRED, "Restricted native image load failed; process restart required");
        }
        wchar_t loadedPath[MAX_PATH]{}; PinnedFile loaded;
        if (!GetModuleFileNameW(image->module, loadedPath, MAX_PATH) || !PinFile(loadedPath, loaded) || !SameFile(image->file.identity, loaded.identity)) return fail(SD_DENIED, "Loaded image file identity mismatch; image retained");
        auto base = reinterpret_cast<unsigned char*>(image->module);
        for (const auto& entry : image->facts.imports) {
            FARPROC resolved{}; std::memcpy(&resolved, base + entry.rva, sizeof(resolved));
            if (resolved != entry.address) return fail(SD_DENIED, "Loaded native dependency substitution detected; image retained");
        }
        std::array<FARPROC, 5> functions{};
        for (std::size_t i = 0; i < functions.size(); ++i) {
            functions[i] = GetProcAddress(image->module, ExportNames[i]);
            if (reinterpret_cast<unsigned char*>(functions[i]) != base + image->facts.exports[i]) return fail(SD_DENIED, "Loaded native export identity mismatch; image retained");
        }
        image->query = reinterpret_cast<SdModuleQueryV2>(functions[0]); image->prepare = reinterpret_cast<SdModulePrepareV2>(functions[1]);
        image->admit = reinterpret_cast<SdModuleAdmitV2>(functions[2]); image->quiesce = reinterpret_cast<SdModuleQuiesceV2>(functions[3]); image->drain = reinterpret_cast<SdModuleDrainV2>(functions[4]);
    }
    if (!image->query || !image->prepare) return fail(SD_DENIED, "Previously rejected retained image cannot activate");
    SdModuleDescriptorV2 descriptor{}; descriptor.header = {sizeof(descriptor), SD_U2A_HOST_VERSION};
    auto result = Invoke(image->query, SD_U2A_HOST_VERSION, &descriptor);
    if (result != SD_OK || descriptor.header.size != sizeof(descriptor) || descriptor.header.version != SD_U2A_HOST_VERSION ||
        descriptor.host_version != SD_U2A_HOST_VERSION || descriptor.base_contract_version != SD_BASE_CONTRACT_VERSION || descriptor.reserved0 || descriptor.reserved1 ||
        std::memcmp(descriptor.contract_sha256, ContractIdentity().data(), 32)) return fail(SD_VERSION, "Native contract negotiation rejected; image retained");
    auto state = std::make_shared<NativeModule::State>(); state->image = image; state->lifecycle = lifecycle_; state->moduleId = registration.moduleId;
    state->domain = domain_;
    state->scope = std::make_shared<runtime::RuntimeScope>(owner_, domain_);
    state->scopeGeneration = state->scope->Current().generation;
    {
        const std::lock_guard lock(registry.mutex);
        state->host = {{sizeof(SdHostApiV2), SD_U2A_HOST_VERSION}, {static_cast<std::uint32_t>(registry.states.size() + 1), 1}, &LifecycleApi, nullptr, nullptr, nullptr};
        registry.states.push_back(state);
    }
    result = Invoke(image->prepare, &state->host);
    auto module = std::shared_ptr<NativeModule>(new NativeModule(state));
    if (result == SD_OK && (state->scope->Current().stopping || domain_->IsStopping() || lifecycle_->Current().stopping || !owner_->IsAcquired())) result = SD_CANCELLED;
    if (result != SD_OK) {
        state->PublishPhase(ModulePhase::Faulted); module->Quiesce(); module->Drain();
        return fail(result, "Native preparation rejected; generation and image retained");
    }
    state->prepared.store(true);
    output = std::move(module); return SD_OK;
}
ImageUsage NativeModuleLoader::Usage() {
    auto& registry = Global(); const std::lock_guard lock(registry.mutex);
    return {registry.images.size(), registry.mappedBytes, registry.states.size()};
}
}
