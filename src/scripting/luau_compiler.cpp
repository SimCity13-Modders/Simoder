#include "scripting/luau_compiler.hpp"
#include "core/json.hpp"
#include <Luau/Compiler.h>
#include <Luau/Bytecode.h>
#include <Windows.h>
#include <atomic>
#include <fstream>
#include <stdexcept>

namespace sc13::scripting {
namespace {
/** @summary Supply identical compilation options to execution and content interpretation.
 * @return Pinned integrated compiler options.
 */
Luau::CompileOptions Options() {
    Luau::CompileOptions options;
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    static constexpr const char* mutableGlobals[]{"require", nullptr};
    options.mutableGlobals = mutableGlobals;
    return options;
}
}
std::string LuauCompilationProfile() {
    const auto options = Options();
    return std::string("luau/") + SC13_LUAU_REVISION + "/opt=" + std::to_string(options.optimizationLevel) +
        "/debug=" + std::to_string(options.debugLevel) + "/mutable=require/bytecode=" + std::to_string(LBC_VERSION_TARGET) +
        "/windows/pointer=" + std::to_string(sizeof(void*) * 8);
}
/** @summary Compile bounded source for the pinned VM while preserving module-local require. */
std::string CompileLuauSource(std::string_view source) {
    if (source.size() > 8U * 1024U * 1024U) throw std::runtime_error("Luau source exceeds 8 MiB");
    const auto options = Options();
    auto result = Luau::compile(std::string(source), options);
    if (result.empty() || result.front() == '\0')
        throw std::runtime_error(result.empty() ? "Empty Luau compiler output" : result.substr(1));
    if (result.size() > 8U * 1024U * 1024U) throw std::runtime_error("Bytecode exceeds 8 MiB");
    return result;
}
namespace {
/** @summary Reject partial output before publishing a generation. */
void Write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(NativePath(path), std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) throw std::runtime_error("Cannot write compiled generation");
}
/** @summary Remove only owned unpublished output during success or exception unwinding. */
struct Staging final {
    std::filesystem::path path;
    /** @summary Best-effort cleanup cannot replace the original compiler exception. */
    ~Staging() {
        try { std::error_code error; std::filesystem::remove_all(NativePath(path), error); }
        catch (...) { /* Cleanup must not replace the original compiler failure. */ }
    }
};
}

void CompileLuauGeneration(const SourceProject& source, const std::filesystem::path& generation) {
    if (source.language != "luau" || source.hasResources)
        throw std::runtime_error("Embedded compiler accepts script-only Luau projects");
    static std::atomic_uint64_t sequence{};
    Staging staging{generation.parent_path() / ("staging-" + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)))};
    if (!std::filesystem::create_directory(NativePath(staging.path))) throw std::runtime_error("Cannot create compiler staging");
    const auto bytecode = CompileLuauSource(ReadBounded(SourcePath(source.directory, source.entry), 8U * 1024U * 1024U));
    Write(staging.path / "script.luau.bin", bytecode);
    std::string descriptor = "schemaVersion = 1\nsourceFingerprint = " + core::JsonString(source.fingerprint) +
        "\nbytecodeSha256 = " + core::JsonString(HashText(bytecode)) + "\n";
    std::filesystem::create_directory(NativePath(staging.path / "modules"));
    std::size_t total = bytecode.size();
    for (const auto& file : std::filesystem::recursive_directory_iterator(source.directory / "scripts")) {
        if (!file.is_regular_file() || file.path().extension() != ".luau") continue;
        const auto relative = file.path().lexically_relative(source.directory).generic_u8string();
        const std::string name(reinterpret_cast<const char*>(relative.data()), relative.size());
        const auto compiled = CompileLuauSource(ReadBounded(SourcePath(source.directory, name), 8U * 1024U * 1024U));
        total += compiled.size();
        if (total > 8U * 1024U * 1024U) throw std::runtime_error("Compiled scripts exceed 8 MiB");
        const auto payload = "modules/" + HashText(name) + ".bin";
        Write(staging.path / payload, compiled);
        descriptor += "\n[[modules]]\nname = " + core::JsonString(name) + "\npayload = " +
            core::JsonString(payload) + "\nsha256 = " + core::JsonString(HashText(compiled)) + "\n";
    }
    if (InspectSourceProject(source.directory).fingerprint != source.fingerprint)
        throw std::runtime_error("Source changed during compilation; active generation retained");
    Write(staging.path / "generation.toml", descriptor);
    if (!PublishGeneration(staging.path, generation)) {
        mods::ModDefinition validated; std::string error;
        if (!LoadSourceProject(source.directory, validated, error))
            throw std::runtime_error("Existing compiled generation rejected: " + error);
    }
}
}
