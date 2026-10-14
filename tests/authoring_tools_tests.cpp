#include "dev/authoring_tools.hpp"
#include "scripting/source_project.hpp"
#include <iostream>
#include <stdexcept>

/** @summary Verify ownership conflicts, failed-registration isolation and lease revocation. */
int main() {
    try {
        using namespace sc13::dev;
        auto first = RegisterAuthoringTools("fixture.first", {{"compiler", L"C:\\fixture\\compiler.exe"}});
        if (sc13::scripting::ToolPath(L"simoder-build.exe") != L"C:\\fixture\\compiler.exe")
            throw std::runtime_error("Compiler resolution ignored the enabled owner");
        bool rejected{};
        try { auto duplicate = RegisterAuthoringTools("fixture.first", {{"compiler", L"C:\\other.exe"}}); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected || FindAuthoringTool("compiler") != L"C:\\fixture\\compiler.exe")
            throw std::runtime_error("Duplicate owner damaged the existing registration");
        rejected = false;
        try { auto conflict = RegisterAuthoringTools("fixture.second", {{"compiler", L"C:\\other.exe"}}); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected || FindAuthoringTool("compiler").empty()) throw std::runtime_error("Capability conflict was not isolated");
        auto second = RegisterAuthoringTools("fixture.second", {{"typescript-toolchain", L"C:\\fixture\\typescript"}});
        if (sc13::scripting::TypeScriptToolchainPath() != L"C:\\fixture\\typescript")
            throw std::runtime_error("TypeScript resolution ignored the enabled owner");
        first.reset();
        if (!FindAuthoringTool("compiler").empty() || FindAuthoringTool("typescript-toolchain").empty())
            throw std::runtime_error("Lease revocation crossed an ownership boundary");
        second.reset();
        if (!FindAuthoringTool("typescript-toolchain").empty()) throw std::runtime_error("Tool survived owner teardown");
        std::cout << "Authoring ownership and revocation passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
