#pragma once
#include "dev/authoring_tools.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace sc13::dev {
/** @summary Declare one explicitly enabled developer service within its owning mod. */
struct ServiceDefinition final {
    std::string id;
    std::filesystem::path executable;
    std::vector<std::wstring> arguments;
    std::vector<std::string> dependencies;
};
/** @summary Declare the versioned developer-service contract without granting script process APIs. */
struct ServiceModDefinition final {
    unsigned apiVersion{1};
    std::string owner;
    AuthoringToolPaths tools;
    std::vector<ServiceDefinition> services;
};
}
