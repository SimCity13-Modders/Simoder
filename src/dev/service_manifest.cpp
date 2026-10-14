#include "dev/service_manifest.hpp"
#include "mods/mod_types.hpp"
#include "mods/mod_definition_parser.hpp"
#include "scripting/source_project.hpp"
#include <functional>
#include <map>
#include <stdexcept>

namespace sc13::dev {
namespace {
/** @summary Require a nonempty bounded TOML string with a field-specific diagnostic. */
std::string Text(const toml::node_view<const toml::node>& node, std::string_view field) {
    const auto value = node.value<std::string>();
    if (!value || value->empty() || value->size() > 4096 || value->find('\0') != std::string::npos)
        throw std::runtime_error("Invalid service field: " + std::string(field));
    return *value;
}
}

std::shared_ptr<const ServiceModDefinition> ParseServiceManifest(const toml::table& document,
    const std::filesystem::path& directory) {
    if (document["serviceApiVersion"].value<std::int64_t>() != 1)
        throw std::runtime_error("Unsupported developer service API version");
    if (document.contains("scripts") || document.contains("resources") || document.contains("patches"))
        throw std::runtime_error("Developer service mods cannot contain gameplay scripts or resources");
    const auto services = document["services"].as_array();
    if (!services || services->empty() || services->size() > 16)
        throw std::runtime_error("Developer mod must declare 1..16 services");
    std::map<std::string, ServiceDefinition> candidates;
    for (const auto& node : *services) {
        const auto table = node.as_table();
        if (!table) throw std::runtime_error("services entries must be tables");
        for (const auto& [key, value] : *table) {
            static_cast<void>(value);
            if (key != "id" && key != "executable" && key != "arguments" && key != "dependencies")
                throw std::runtime_error("Unknown service field: " + std::string(key.str()));
        }
        ServiceDefinition service;
        service.id = Text((*table)["id"], "id");
        if (!mods::IsValidModId(service.id)) throw std::runtime_error("Invalid service ID");
        service.executable = scripting::SourcePath(directory, Text((*table)["executable"], "executable"));
        if (!std::filesystem::is_regular_file(service.executable) || service.executable.extension() != ".exe")
            throw std::runtime_error("Service executable must be a local .exe file");
        for (const auto* field : {"arguments", "dependencies"}) {
            if (!table->contains(field)) continue;
            const auto values = (*table)[field].as_array();
            if (!values || values->size() > 64) throw std::runtime_error("Invalid service argument/dependency array");
            for (const auto& value : *values) {
                const auto text = Text(toml::node_view<const toml::node>(&value), field);
                if (std::string_view(field) == "dependencies") service.dependencies.push_back(text);
                else {
                    if (text.starts_with("--parent-pid") || text.starts_with("--game-root"))
                        throw std::runtime_error("Service arguments cannot override runtime ownership");
                    service.arguments.push_back(std::filesystem::path(std::u8string(text.begin(), text.end())).native());
                }
            }
        }
        const auto id = service.id;
        if (!candidates.emplace(id, std::move(service)).second) throw std::runtime_error("Duplicate service ID");
    }
    auto result = std::make_shared<ServiceModDefinition>();
    std::map<std::string, unsigned> marks;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        const auto found = candidates.find(id);
        if (found == candidates.end()) throw std::runtime_error("Missing service dependency: " + id);
        auto& mark = marks[id];
        if (mark == 1) throw std::runtime_error("Cyclic service dependency: " + id);
        if (mark == 2) return;
        mark = 1;
        for (const auto& dependency : found->second.dependencies) visit(dependency);
        mark = 2;
        result->services.push_back(found->second);
    };
    for (const auto& [id, service] : candidates) { static_cast<void>(service); visit(id); }
    result->owner = Text(document["metadata"]["id"], "metadata.id");
    if (const auto tools = document["tools"].as_table()) {
        for (const auto& [name, node] : *tools) {
            const std::string capability(name.str());
            if (capability != "compiler" && capability != "resource-converter" && capability != "typescript-toolchain")
                throw std::runtime_error("Unknown authoring capability: " + capability);
            const auto relative = node.value<std::string>();
            if (!relative || relative->empty()) throw std::runtime_error("Invalid authoring tool path");
            const auto path = scripting::SourcePath(directory, *relative);
            if (capability == "typescript-toolchain") {
                const auto entry = scripting::SourcePath(directory, *relative + "/build.mjs");
                if (!std::filesystem::is_directory(path) || !std::filesystem::is_regular_file(entry))
                    throw std::runtime_error("TypeScript toolchain must contain build.mjs");
            } else if (!std::filesystem::is_regular_file(path) || path.extension() != ".exe")
                throw std::runtime_error("Authoring tool must be a local executable");
            result->tools.emplace(capability, path);
        }
    } else if (document.contains("tools")) throw std::runtime_error("tools must be a table");
    return result;
}
}
