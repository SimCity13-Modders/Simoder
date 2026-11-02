#include "scripting/base_host.hpp"
#include "bootstrap/selection_data_session.hpp"
#include "scripting/base_package_compile.hpp"
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
/** @summary Build a compiler generation only from captured hash-verified source bytes. */
ScriptDefinition Capture(const std::filesystem::path& root, const BasePackagePin& pin) {
    if (pin.version.empty() || pin.version.size() > 128 || pin.files.size() < 2 || pin.files.size() > 64 ||
        !pin.files.contains("mod.toml") || !pin.files.contains("scripts/init.luau"))
        throw std::runtime_error("Invalid host base pin");
    std::map<std::string, std::string> captured;
    std::size_t total{};
    for (const auto& [name, hash] : pin.files) {
        detail::ValidateBaseSourceName(name);
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
        detail::ValidateBaseSourceName(name);
        const auto checked = SourcePath(root, name);
        if (std::filesystem::is_regular_file(checked)) {
            if (!pin.files.contains(name)) throw std::runtime_error("Unpinned base script tree file");
            actual.insert(name);
        } else if (!std::filesystem::is_directory(checked)) throw std::runtime_error("Unsupported base tree entry");
    }
    if (actual.size() + 1 != captured.size()) throw std::runtime_error("Base source inventory changed");
    return CompileCapturedBasePackage(captured,pin);
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
BaseHost::BaseHost(std::shared_ptr<bootstrap::SelectionDataSession> session,BasePackagePin pin)
    :capturedSession_(std::move(session)),pin_(std::move(pin)){
    bool expected{};if(!occupied.compare_exchange_strong(expected,true))throw std::runtime_error("Base host already exists");ownsSlot_=true;
    try{
        if(!capturedSession_)throw std::runtime_error("Original captured Base session missing");
        const auto definition=capturedSession_->Compile(pin_);
        bridge_=std::make_shared<SemanticBridge>();ScriptHost host;host.semantic=bridge_->Bind({});
        runtime_.reset(new ScriptRuntime(ScriptRuntime::BaseBootstrap{},definition,std::move(host)));
        runtime_->pausedCloseManaged_=true;
        // @summary Deliberately NO Commit, scheduler registration, manager publication or public host access.
    }catch(...){occupied.store(false);ownsSlot_=false;throw;}
}
runtime::ScopeResult BaseHost::DisposePaused()noexcept{
    if(!capturedSession_||!runtime_)return runtime::ScopeResult::Invalid;
    try{std::unique_lock control(runtime_->controlMutex_,std::try_to_lock);
        if(!control.owns_lock())return runtime::ScopeResult::Busy;return runtime_->ContinuePausedCloseLocked();}
    catch(...){return runtime::ScopeResult::Indeterminate;}
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
