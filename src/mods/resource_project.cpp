#include "mods/resource_project.hpp"
#include "mods/mod_definition_parser.hpp"
#include "core/sha256.hpp"
#include "../../third_party/tomlplusplus/toml.hpp"
#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>

namespace sc13::mods {
namespace {
/** @summary Require a nonempty string rather than silently defaulting invalid manifest fields. */
std::string Text(const toml::node_view<const toml::node>& node) {
    const auto value = node.value<std::string>();
    if (!value || value->empty()) throw std::runtime_error("Required nonempty TOML string missing");
    return *value;
}
/** @summary Reject links and junctions for each existing input component. */
void PlainPath(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Missing input or reparse point in resource project");
}
/** @summary Parse each fixed-width hexadecimal component without narrowing a 64-bit instance. */
void CheckIdentity(std::string_view value) {
    if (value.size() != 34 || value[8] != ':' || value[17] != ':')
        throw std::runtime_error("Resource ID requires TYPE:GROUP:64-bit-INSTANCE");
    for (const auto part : {value.substr(0,8),value.substr(9,8),value.substr(18,16)}) {
        std::uint64_t parsed{};
        const auto result = std::from_chars(part.data(),part.data()+part.size(),parsed,16);
        if (result.ec != std::errc{} || result.ptr != part.data()+part.size())
            throw std::runtime_error("Invalid hexadecimal resource ID");
    }
}
}

bool LoadResourceProject(const std::filesystem::path& directory, ModDefinition& definition,
    std::string& error) noexcept {
    try {
        definition = {};
        PlainPath(directory);
        const auto manifest = directory / L"project.toml";
        PlainPath(manifest);
        if (std::filesystem::file_size(manifest) > 8U*1024U*1024U)
            throw std::runtime_error("TOML manifest exceeds 8 MiB");
        std::ifstream input(manifest,std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        const auto document = toml::parse(bytes);
        if (document["schemaVersion"].value<std::int64_t>() != 3 ||
            document["contract"].value<std::string>() != "simoder.raw-resources")
            throw std::runtime_error("Unsupported TOML resource contract");
        const auto capabilities = document["requiredCapabilities"].as_array();
        if (!capabilities || capabilities->empty()) throw std::runtime_error("Missing resource capabilities");
        std::set<std::string> declared;
        for (const auto& node : *capabilities) {
            const auto capability = node.value<std::string>();
            if (!capability || (*capability != "resource.register" && *capability != "resource.replace"))
                throw std::runtime_error("Unknown required runtime capability");
            declared.insert(*capability);
        }
        ModDefinition parsed;
        parsed.directory = directory;
        parsed.manifest.id = Text(document["metadata"]["id"]);
        parsed.manifest.name = Text(document["metadata"]["name"]);
        parsed.manifest.version = Text(document["metadata"]["version"]);
        parsed.manifest.author = document["metadata"]["author"].value_or(std::string{});
        parsed.manifest.description = document["metadata"]["description"].value_or(std::string{});
        if (!IsValidModId(parsed.manifest.id)) throw std::runtime_error("Invalid mod ID");
        const auto resources = document["resources"].as_array();
        if (!resources || resources->empty() || resources->size()>65536)
            throw std::runtime_error("Resource array must contain 1..65536 entries");
        std::set<std::string> identities;
        for (const auto& node : *resources) {
            const auto table = node.as_table();
            if (!table) throw std::runtime_error("Resource must be a TOML table");
            BinaryResourceDefinition resource;
            resource.id = Text((*table)["id"]);
            CheckIdentity(resource.id);
            std::transform(resource.id.begin(),resource.id.end(),resource.id.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
            if (!identities.insert(resource.id).second) throw std::runtime_error("Duplicate resource identity");
            resource.operation = Text((*table)["operation"]);
            if (resource.operation != "add" && resource.operation != "replace")
                throw std::runtime_error("Unsupported resource operation");
            if (!declared.contains(resource.operation == "add" ? "resource.register" : "resource.replace"))
                throw std::runtime_error("Resource capability not declared");
            resource.sha256 = Text((*table)["sha256"]);
            if (resource.sha256.size()!=64 || !std::all_of(resource.sha256.begin(),resource.sha256.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))
                throw std::runtime_error("Expected lowercase SHA-256");
            resource.payload = Text((*table)["payload"]);
            if (resource.payload != "assets/"+resource.sha256+".bin")
                throw std::runtime_error("Payload must be its content-addressed assets/SHA256.bin path");
            const auto size = (*table)["size"].value<std::int64_t>();
            if (!size || *size<0 || *size>256LL*1024LL*1024LL) throw std::runtime_error("Invalid resource size");
            resource.size = static_cast<std::uint64_t>(*size);
            PlainPath(directory/L"assets");
            const auto path = directory/std::filesystem::path(resource.payload);
            PlainPath(path);
            if (std::filesystem::file_size(path)!=resource.size) throw std::runtime_error("Resource size mismatch");
            core::Sha256Digest digest;
            if (!core::Sha256File(path,digest,error)) throw std::runtime_error(error);
            auto actual = core::ToHex(digest);
            std::transform(actual.begin(),actual.end(),actual.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            if (actual!=resource.sha256) throw std::runtime_error("Resource SHA-256 mismatch");
            parsed.resources.push_back(std::move(resource));
        }
        definition = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        definition = {};
        error = "project.toml: " + std::string(exception.what());
        return false;
    } catch (...) {
        definition = {};
        error = "project.toml validation failed";
        return false;
    }
}

std::string ResourceRuntimeDiagnostic(const ModDefinition& definition) {
    if (definition.resources.empty()) return {};
    return "Validated TOML project ("+std::to_string(definition.resources.size())+
        " lossless resources). Activation blocked: resource.register/resource.replace runtime is not implemented; no resources applied.";
}
}
