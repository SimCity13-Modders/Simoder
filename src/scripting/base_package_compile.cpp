/** @file @summary Actual BaseHost verification/compiler factored for original captured bytes; no filesystem or activation. */
#include "scripting/base_package_compile.hpp"
#include "scripting/base_host.hpp"
#include "scripting/source_project.hpp"
#include "scripting/luau_compiler.hpp"
#include "../../third_party/tomlplusplus/toml.hpp"
#include <algorithm>
#include <stdexcept>
namespace sc13::scripting {
namespace detail {
/** @summary Verify a portable normalized ASCII package path without following the filesystem. */
void ValidateBaseSourceName(std::string_view name) {
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
}
ScriptDefinition CompileCapturedBasePackage(const std::map<std::string,std::string>& captured,const BasePackagePin& pin){
    if(pin.version.empty()||pin.version.size()>128||pin.files.size()<2||pin.files.size()>64||
        !pin.files.contains("mod.toml")||!pin.files.contains("scripts/init.luau")||captured.size()!=pin.files.size())
        throw std::runtime_error("Invalid host base pin or captured membership");
    // @summary Canonical names are lowercase ASCII; exact slash-boundary ancestry is the Windows case policy after validation.
    // Check ALL pairs: lexicographic neighbours can be separated by a similar punctuation prefix.
    for(const auto& [name,hash]:pin.files){static_cast<void>(hash);detail::ValidateBaseSourceName(name);
        for(const auto& [other,otherHash]:pin.files){static_cast<void>(otherHash);
            if(other.size()>name.size()&&other.starts_with(name)&&other[name.size()]=='/')
                throw std::runtime_error("Base executable file is ancestor of another file");
        }
    }
    std::size_t total{};
    for(const auto& [name,hash]:pin.files){detail::ValidateBaseSourceName(name);
        if(name!="mod.toml"&&(!name.starts_with("scripts/")||!name.ends_with(".luau")))throw std::runtime_error("Unexpected pinned base code");
        if(hash.size()!=64||!std::all_of(hash.begin(),hash.end(),[](char c){return(c>='0'&&c<='9')||(c>='A'&&c<='F');}))throw std::runtime_error("Noncanonical base SHA-256");
        const auto found=captured.find(name);if(found==captured.end()||found->second.size()>4U*1024U*1024U-total)throw std::runtime_error("Base source membership or aggregate bound mismatch");
        total+=found->second.size();if(HashText(found->second)!=hash)throw std::runtime_error("Base source pin mismatch: "+name);
    }
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
