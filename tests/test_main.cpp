#include "mod_fixture.hpp"
#include "binary_fixture.hpp"
#include "test_harness.hpp"
#include "config/loader_config.hpp"
#include "dev/mod_validation.hpp"
#include "dev/runtime_diagnostics.hpp"
#include "core/json.hpp"
#include "core/tgi.hpp"
#include "core/resource_filter.hpp"
#include "core/runtime_property_table.hpp"
#include "formats/dbpf/dbpf_reader.hpp"
#include "formats/dbpf/refpack.hpp"
#include "formats/prop/prop_document.hpp"
#include "formats/toon/toon_document.hpp"
#include "memory/signature.hpp"
#include "logging/log_event.hpp"
#include "mods/mod_definition_parser.hpp"
#include "mods/mod_manager.hpp"
#include "mods/resource_project.hpp"
#include "mods/mod_state_store.hpp"
#include "mods/patch_registry.hpp"
#include "reverse/pe_file.hpp"
#include "reverse/build_compatibility.hpp"
#include "runtime/runtime_resource_cache.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
using sc13::tests::CreateTemporaryDirectory;
using sc13::tests::WriteTextFile;
using sc13::tests::FixtureHeader;
using sc13::tests::FixturePatch;
using sc13::tests::WriteModFixture;

using sc13::tests::AppendBigU16;
using sc13::tests::AppendBigU32;
using sc13::tests::WriteLittleU16;
using sc13::tests::WriteLittleU32;



/** Provides a stable non-inlined destination for PE relative-call discovery. */
__declspec(noinline) int PeCallProbeTarget(int value) {
    return value * 7 + 3;
}

/** Emits a direct call whose result prevents tail-call optimization. */
__declspec(noinline) int PeCallProbeCaller(int value) {
    return PeCallProbeTarget(value) + 11;
}

/** @summary Checks disk identity and malformed input without retired launcher-choice behavior. */
void TestGameBuildAssessment() {
    wchar_t executable[32768]{};
    SC13_EXPECT(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0U);
    sc13::reverse::GameBuildAssessment assessment;
    std::string error;
    SC13_EXPECT(sc13::reverse::AssessGameBuild(executable, assessment, error));
    SC13_EXPECT(!assessment.verified);
    SC13_EXPECT(assessment.fileSha256.size() == 64U);
    assessment.fileVersion = sc13::reverse::kSupportedGameVersion;
    SC13_EXPECT(!assessment.verified);
    SC13_EXPECT(!sc13::reverse::AssessGameBuild(L"missing-simoder-regression-executable", assessment, error));
    SC13_EXPECT(!assessment.verified);
    SC13_EXPECT(!error.empty());
}

/** Validates PE32 parsing against the currently executing x86 test image. */
void TestPeFile() {
    wchar_t executablePath[32768]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
    SC13_EXPECT(length != 0);
    SC13_EXPECT(length < std::size(executablePath));

    sc13::reverse::PeFile pe;
    std::string error;
    SC13_EXPECT(pe.Open(executablePath, error));
    SC13_EXPECT(pe.machine() == 0x014CU);
    SC13_EXPECT(pe.imageBase() != 0);
    SC13_EXPECT(pe.entryPointRva() != 0);
    SC13_EXPECT(!pe.sections().empty());
    const std::array<std::string_view, 1> needles{"All SC13 tests passed."};
    const auto strings = pe.FindAsciiStrings(needles, 4, 4);
    SC13_EXPECT(!strings.empty());
    SC13_EXPECT(strings.front().rva.has_value());
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto targetAddress = reinterpret_cast<std::uintptr_t>(&PeCallProbeTarget);
    SC13_EXPECT(targetAddress >= moduleBase);
    const auto targetRva = static_cast<std::uint32_t>(targetAddress - moduleBase);
    const auto calls = pe.FindRelativeCallReferences(pe.imageBase() + targetRva, 16);
    SC13_EXPECT(!calls.empty());
    SC13_EXPECT(PeCallProbeCaller(2) == 28);
}

}  // namespace

/** Executes deterministic unit tests without a third-party framework. */
int main(int argc, char** argv) {
    /** @summary Validate an explicitly supplied private fixture with the production parser without running it in game. */
    if (argc == 3 && std::string_view(argv[1]) == "--validate-project") {
        sc13::mods::ModDefinition imported;
        std::string error;
        if (!sc13::mods::LoadModDefinition(std::filesystem::path(argv[2]), imported, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << imported.manifest.name << ": " << imported.resources.size()
                  << " verified resources\n" << sc13::mods::ResourceRuntimeDiagnostic(imported) << '\n';
        return 0;
    }
    sc13::tests::TestCorePrimitives();
    TestGameBuildAssessment();
    sc13::tests::TestRuntimeTable();
    sc13::tests::TestFormats();
    sc13::tests::TestModDefinitionParser();
    sc13::tests::TestPatchOwnership();
    sc13::tests::TestConfiguration();
    sc13::tests::TestModLifecycle();
    sc13::tests::TestResourceIntegration();
    TestPeFile();
    if (sc13::tests::FailureCount() == 0) {
        std::cout << "All SC13 tests passed.\n";
        return 0;
    }
    std::cerr << sc13::tests::FailureCount() << " test expectation(s) failed.\n";
    return 1;
}
