#include "scripting/base_host.hpp"
#include "mods/fixture_observation.hpp"
#include "scripting/semantic_bridge.hpp"
#include "scripting/source_project.hpp"
#include "scripting/luau_compiler.hpp"
#include "../../third_party/tomlplusplus/toml.hpp"
#include <atomic>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace sc13::scripting {
namespace {
std::atomic_bool occupied{};
constexpr std::size_t maximumSource = 4U * 1024U * 1024U;
/** @summary Verify a portable normalized ASCII package path without following the filesystem. */
void ValidateName(std::string_view name) {
    if (name.empty() || name.size() > 240 || name.front() == '/' || name.back() == '/')
        throw std::runtime_error("Invalid base source path");
    for (unsigned char c : name)
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '/' && c != '.' && c != '-' && c != '_')
            throw std::runtime_error("Noncanonical base source path");
    std::size_t begin{};
    while (begin < name.size()) {
        const auto end = name.find('/', begin);
        const auto part = name.substr(begin, end == name.npos ? name.size() - begin : end - begin);
        if (part.empty() || part == "." || part == ".." || part.back() == '.')
            throw std::runtime_error("Noncanonical base source component");
        if (end == name.npos) break;
        begin = end + 1;
    }
}
/** @summary Build a compiler generation only from captured hash-verified source bytes. */
ScriptDefinition Capture(const std::filesystem::path& root, const BasePackagePin& pin) {
    if (pin.version.empty() || pin.version.size() > 128 || pin.files.size() < 2 || pin.files.size() > 64 ||
        !pin.files.contains("mod.toml") || !pin.files.contains("scripts/init.luau"))
        throw std::runtime_error("Invalid host base pin");
    std::map<std::string, std::string> captured;
    std::size_t total{};
    for (const auto& [name, hash] : pin.files) {
        ValidateName(name);
        if (name != "mod.toml" && (!name.starts_with("scripts/") || !name.ends_with(".luau")))
            throw std::runtime_error("Unexpected pinned base code");
        if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }))
            throw std::runtime_error("Noncanonical base SHA-256");
        auto bytes = ReadBounded(SourcePath(root, name), maximumSource - total);
        total += bytes.size();
        if (HashText(bytes) != hash) throw std::runtime_error("Base source pin mismatch: " + name);
        captured.emplace(name, std::move(bytes));
    }
    // Inventory is bounded and non-following: only captured pinned bytes can ever reach the compiler.
    std::set<std::string> actual;
    std::size_t entries{};
    for (const auto& item : std::filesystem::recursive_directory_iterator(SourcePath(root, "scripts"))) {
        if (++entries > 128) throw std::runtime_error("Base script tree exceeds entry bound");
        const auto raw = item.path().lexically_relative(root).generic_u8string();
        const std::string name(reinterpret_cast<const char*>(raw.data()), raw.size());
        ValidateName(name);
        const auto checked = SourcePath(root, name);
        if (std::filesystem::is_regular_file(checked)) {
            if (!pin.files.contains(name)) throw std::runtime_error("Unpinned base script tree file");
            actual.insert(name);
        } else if (!std::filesystem::is_directory(checked)) throw std::runtime_error("Unsupported base tree entry");
    }
    if (actual.size() + 1 != captured.size()) throw std::runtime_error("Base source inventory changed");
    const auto& manifest = captured.at("mod.toml");
    const auto document = toml::parse(manifest);
    // Strictly reuse the existing authoring format without accepting dormant resources or roles.
    for (const auto& [key, value] : document) {
        static_cast<void>(value);
        if (key != "schemaVersion" && key != "format" && key != "kind" && key != "metadata" && key != "scripts")
            throw std::runtime_error("Unsupported base manifest field");
    }
    const auto projection = InspectSourceManifest(manifest);
    if (projection.metadata.id != "simoder.base" || projection.metadata.version != pin.version ||
        (document.contains("kind") && document["kind"].value<std::string>() != "gameplay") || projection.language != "luau" ||
        projection.entry != "scripts/init.luau" || projection.memoryBytes != 8U * 1024U * 1024U || projection.budgetMs != 2)
        throw std::runtime_error("Base manifest identity or configuration mismatch");
    ScriptDefinition result;
    result.entry = projection.entry; result.language = "luau"; result.revision = pin.files.at("mod.toml");
    result.memoryBytes = projection.memoryBytes; result.budgetMs = projection.budgetMs;
    std::size_t compiledBytes{};
    for (const auto& [name, bytes] : captured) {
        if (name == "mod.toml") continue;
        auto compiled = CompileLuauSource(bytes);
        compiledBytes += compiled.size();
        if (compiledBytes > 8U * 1024U * 1024U) throw std::runtime_error("Base bytecode exceeds aggregate limit");
        if (name == result.entry) result.bytecode = compiled;
        result.modules.emplace(name, std::move(compiled));
    }
    return result;
}
}

BaseHost::BaseHost(const std::filesystem::path& directory, BasePackagePin pin,
    std::function<void(ScriptPhase, std::uint64_t)> observer, std::shared_ptr<SemanticBridge> bridge) : pin_(pin) {
    bool expected{};
    if (!occupied.compare_exchange_strong(expected, true)) throw std::runtime_error("Base host already exists");
    ownsSlot_ = true;
    try {
        const auto definition = Capture(std::filesystem::absolute(directory).lexically_normal(), pin);
        bridge_=bridge ? std::move(bridge) : std::make_shared<SemanticBridge>();
        for (const auto& [name,code]:definition.modules)
            if (name.starts_with("scripts/lib/") && name != "scripts/lib/dag.luau")
                presentationModules_.emplace("scripts/.simoder-base/"+name.substr(12),code);
        ScriptHost host; host.lifecycle = std::move(observer); host.semantic=bridge_->Bind({});
        runtime_.reset(new ScriptRuntime(ScriptRuntime::BaseBootstrap{}, definition, std::move(host)));
        runtime_->Commit();
    } catch (...) { occupied.store(false); ownsSlot_ = false; throw; }
}
BaseHost::~BaseHost() {
#if defined(SIMODER_RUNTIME_TEST_AUTHORITY)
    mods::fixture::Destroying(this,mods::fixture::ObjectKind::Base);
#endif
    runtime_.reset();
    if (ownsSlot_) occupied.store(false);
}
ScriptStatus BaseHost::Status() const { return runtime_->Status(); }
std::vector<std::string> BaseHost::DrainLogs() { return runtime_->DrainLogs(); }
ScriptHost BaseHost::Bind(std::shared_ptr<runtime::RuntimeScope> scope) const {
    ScriptHost host; host.semantic=bridge_->Bind(std::move(scope)); host.semanticModules=presentationModules_; return host;
}
void BaseHost::Pump() { runtime_->Tick(0); }
bool BaseHost::Ready() const noexcept { return bridge_ && bridge_->ReadyGeneration()!=0; }
}
