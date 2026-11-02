cmake_minimum_required(VERSION 3.24)

project(Simoder VERSION 0.1.0 LANGUAGES C CXX)


if(NOT WIN32)
    message(FATAL_ERROR "SC13 Mod Loader currently targets Windows only.")
endif()

option(SC13_BUILD_RUNTIME "Build the integrated D3D9 runtime DLL." ON)
option(SC13_BUILD_TESTS "Build unit tests." ON)
option(SC13_CORE_ONLY "Expose only the shared Simoder core library." OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include(cmake/VerifyNativeSdk.cmake)

# @summary Pin the statically linked bytecode compiler and VM to one revision.
include(FetchContent)
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_STATIC_CRT ON CACHE BOOL "" FORCE)
# @summary Native ownership guards require Luau's C++ exception path; longjmp would bypass retained-object cleanup.
set(LUAU_EXTERN_C OFF CACHE BOOL "" FORCE)
set(SC13_LUAU_REVISION 1f6ff5d6f0072f2fab2adf35d1ffbaae7d848ad0)
FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG ${SC13_LUAU_REVISION}
)
FetchContent_MakeAvailable(luau)
set_property(DIRECTORY "${luau_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
if(MSVC)
    # @summary The embedded parser/compiler uses C++ exception handlers for malformed input.
    target_compile_options(Luau.Ast PRIVATE /EHsc)
    target_compile_options(Luau.Compiler PRIVATE /EHsc)
    target_compile_options(Luau.VM PRIVATE /EHsc)
endif()

function(sc13_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(sc13_core STATIC
    # @summary Share original bootstrap composition with the isolated acceptance host.
    src/bootstrap/runtime_context.cpp
    src/config/loader_config.cpp
    src/core/resource_filter.cpp
    src/core/runtime_property_table.cpp
    src/core/sha256.cpp
    src/platform/component_store/metadata.cpp
    src/platform/component_store/windows_store.cpp
    src/platform/component_store/windows_store_io.cpp
    src/platform/component_store/streaming_digest.cpp
    src/platform/base_artifact_reader.cpp
    src/platform/component_store/coordinator.cpp
    src/core/tgi.cpp
    src/dev/mod_validation.cpp
    src/dev/authoring_tools.cpp
    src/dev/authoring_tool_manifest.cpp
    src/dev/service_supervisor.cpp
    src/dev/service_manifest.cpp
    src/dev/runtime_diagnostics.cpp
    src/dev/runtime_control.cpp
    src/dev/session/session_transport.cpp
    src/formats/dbpf/dbpf_reader.cpp
    src/formats/dbpf/refpack.cpp
    src/formats/prop/prop_document.cpp
    src/formats/toon/toon_document.cpp
    src/formats/toml/value_conversion.cpp
    src/logging/log_event.cpp
    src/memory/signature.cpp
    src/platform/windows_arguments.cpp
    src/platform/runtime_owner.cpp
    src/platform/game_window_dispatch.cpp
    src/mods/mod_definition_parser.cpp
    src/mods/captured_candidate.cpp
    src/mods/action_worker.cpp
    src/mods/mod_candidate.cpp
    src/mods/mod_manager.cpp
    src/mods/manager_preparation.cpp
    src/mods/manager_publication.cpp
    src/mods/manager_cache.cpp
    src/mods/manager_commands.cpp
    src/mods/manager_close.cpp
    src/mods/mod_state_store.cpp
src/mods/state_path_writer.cpp
    src/mods/mod_types.cpp
    src/mods/native/native_module_loader.cpp
    src/mods/native/native_provider.cpp
    src/mods/resource_project.cpp
    src/mods/patch_registry.cpp
    src/reverse/pe_file.cpp
    src/reverse/build_compatibility.cpp
    src/runtime/runtime_resource_cache.cpp
    src/runtime/cache_settlement.cpp
    src/runtime/runtime_scope.cpp
    src/runtime/runtime_lifecycle.cpp
    src/runtime/copied_value.cpp
    src/runtime/semantic_tasks.cpp
    src/scripting/script_runtime.cpp
    src/scripting/base_host.cpp
    src/scripting/semantic_bridge.cpp
    src/scripting/semantic_participant.cpp
    src/mods/owned_publication.cpp
    src/scripting/semantic_lua.cpp
    src/scripting/script_patch_reducer.cpp
    src/scripting/luau_compiler.cpp
    src/scripting/source_project.cpp
)
target_include_directories(sc13_core PUBLIC src)
target_compile_definitions(sc13_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(sc13_core PRIVATE SC13_LUAU_REVISION="${SC13_LUAU_REVISION}")
# @summary Identify diagnostic records by reviewed source/toolchain inputs; binary hashes remain separate release evidence.
file(GLOB_RECURSE sc13_identity_sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
list(APPEND sc13_identity_sources CMakeLists.txt cmake/simoder-native-sdk.lock.cmake cmake/VerifyNativeSdk.cmake)
list(SORT sc13_identity_sources)
set(sc13_identity_material "${PROJECT_VERSION}|${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_SIZEOF_VOID_P}|1f6ff5d6f0072f2fab2adf35d1ffbaae7d848ad0\n")
foreach(sc13_identity_source IN LISTS sc13_identity_sources)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${sc13_identity_source}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${sc13_identity_source}" sc13_identity_hash)
    string(APPEND sc13_identity_material "${sc13_identity_source}:${sc13_identity_hash}\n")
endforeach()
string(SHA256 SC13_SEMANTIC_BUILD_ID "${sc13_identity_material}")
target_compile_definitions(sc13_core PRIVATE SC13_SEMANTIC_BUILD_ID="${SC13_SEMANTIC_BUILD_ID}" SC13_SEMANTIC_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(sc13_core PUBLIC bcrypt version advapi32 user32 Luau.VM Luau.Compiler)
sc13_enable_warnings(sc13_core)

# @summary Stable source SDK consumed by the separately versioned DevTools repository.
set(SC13_CORE_API_VERSION 1 CACHE INTERNAL "Simoder source SDK API version")
add_library(Simoder::Core ALIAS sc13_core)
if(SC13_CORE_ONLY)
    return()
endif()
if(SC13_BUILD_RUNTIME)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "The SimCity 10.3.6.0 process is x86. Configure with -A Win32.")
    endif()

    include(FetchContent)
    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG v1.3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(minhook)
    set_property(DIRECTORY "${minhook_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.9
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)
    add_library(sc13_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    )
    target_include_directories(
        sc13_imgui
        PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends"
    )
    target_compile_definitions(sc13_imgui PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(sc13_imgui PROPERTIES FOLDER "Dependencies")

    add_library(simoder-runtime SHARED
        src/bootstrap/proxy/d3d9_proxy.cpp
        src/bootstrap/loader.cpp
        src/hooks/runtime_hooks.cpp
        src/hooks/game_log_capture.cpp
        src/hooks/resource_trace.cpp
        src/logging/async_logger.cpp
        src/logging/mod_logger.cpp
        src/reverse/game_build.cpp
        src/reverse/resource_symbols.cpp
        src/runtime/patch_signal.cpp
        src/ui/simoder_overlay.cpp
        src/ui/window_message_hook.cpp
        src/ui/mod_panel.cpp
        src/dev/session/native_script_bridge.cpp
    )
    # @summary Embed the fixed semantic session router without adding a production sidecar.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/dev/session/session_router.js" SC13_SESSION_ROUTER_SOURCE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/dev/session/session_router.js)
    configure_file(src/dev/session/session_router.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/session_router.hpp" @ONLY)
    target_link_libraries(simoder-runtime PRIVATE sc13_core sc13_imgui minhook version d3d9)
    target_include_directories(simoder-runtime PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(simoder-runtime PRIVATE SC13_RUNTIME_BUILD)
    set_target_properties(simoder-runtime PROPERTIES OUTPUT_NAME "d3d9")
    target_compile_options(simoder-runtime PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/EHsc;/utf-8;/Zc:__cplusplus>")

    # @summary Build an independent x86 proxy with the genuine D3D9 export names and ordinals.
    if(NOT MSVC)
        message(FATAL_ERROR "The D3D9 proxy currently requires MSVC and x86 MASM.")
    endif()
    enable_language(ASM_MASM)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(sc13_proxy_exports "${CMAKE_CURRENT_BINARY_DIR}/proxy-exports")
    add_custom_command(
        OUTPUT "${sc13_proxy_exports}/d3d9.def" "${sc13_proxy_exports}/d3d9.asm"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/proxy/generate_exports.py" "${sc13_proxy_exports}"
        DEPENDS src/bootstrap/proxy/generate_exports.py
        VERBATIM)
    target_sources(simoder-runtime PRIVATE
        "${sc13_proxy_exports}/d3d9.def" "${sc13_proxy_exports}/d3d9.asm")
    # @summary Install the only production binary beside the game's executable.
    install(TARGETS simoder-runtime RUNTIME DESTINATION . LIBRARY DESTINATION .)
    install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION licenses)
    install(FILES "${luau_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME luau-LICENSE.txt)
    install(FILES "${minhook_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME minhook-LICENSE.txt)
    install(FILES "${imgui_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME imgui-LICENSE.txt)
    install(FILES third_party/tomlplusplus/LICENSE DESTINATION licenses RENAME tomlplusplus-LICENSE.txt)

endif()

if(SC13_BUILD_TESTS)
    enable_testing()
    set(SC13_BASE_SOURCE_DIR "" CACHE PATH "Optional separately checked-out Base source for integration tests")
    # @summary Compile the identical Core sources with fixture issuers only into a separate non-installed acceptance host.
    get_target_property(sc13_semantic_test_sources sc13_core SOURCES)
    add_library(sc13-semantic-test-support STATIC ${sc13_semantic_test_sources})
    # @summary Observe genuine native lifetime/admission only in the non-installed fixture host.
    target_sources(sc13-semantic-test-support PRIVATE src/mods/fixture_observation.cpp)
    # @summary Qualify original cache/reset witnesses and bounded native eviction obligations.
    add_executable(simoder-cache-settlement-tests tests/cache_settlement_tests.cpp)
    target_link_libraries(simoder-cache-settlement-tests PRIVATE sc13-semantic-test-support)
    sc13_enable_warnings(simoder-cache-settlement-tests)
    add_test(NAME simoder-cache-settlement-tests COMMAND simoder-cache-settlement-tests)
    set_tests_properties(simoder-cache-settlement-tests PROPERTIES TIMEOUT 90)
      # @summary Qualify actual canonical manager membership and borrowed-resource lifetime without production cutover.
      add_executable(simoder-manager-lifetime-tests tests/manager_lifetime_tests.cpp tests/mod_fixture.cpp tests/test_harness.cpp)
      target_link_libraries(simoder-manager-lifetime-tests PRIVATE sc13-semantic-test-support)
      sc13_enable_warnings(simoder-manager-lifetime-tests)
      add_test(NAME simoder-manager-lifetime-tests COMMAND simoder-manager-lifetime-tests)
      set_tests_properties(simoder-manager-lifetime-tests PROPERTIES TIMEOUT 45)
    # @summary Qualify the existing scheduler's isolated native reservation and retirement route.
    add_executable(simoder-scheduler-publication-tests tests/scheduler_publication_tests.cpp)
    target_link_libraries(simoder-scheduler-publication-tests PRIVATE sc13-semantic-test-support)
    sc13_enable_warnings(simoder-scheduler-publication-tests)
    add_test(NAME simoder-scheduler-publication-tests COMMAND simoder-scheduler-publication-tests)
    set_tests_properties(simoder-scheduler-publication-tests PROPERTIES TIMEOUT 45)
    target_include_directories(sc13-semantic-test-support PUBLIC src)
    target_compile_definitions(sc13-semantic-test-support PUBLIC SIMODER_RUNTIME_TEST_AUTHORITY UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_definitions(sc13-semantic-test-support PRIVATE SC13_LUAU_REVISION="${SC13_LUAU_REVISION}")
    # @summary Qualify actual lane rejection only in the fixture that links the real script runtime.
    target_compile_definitions(sc13-semantic-test-support PRIVATE SIMODER_SCRIPT_LANE_TEST_WITNESS)
    # @summary Exercise immutable selected-input capture in the non-installed fixture host.
    add_executable(simoder-captured-candidate-tests tests/captured_candidate_tests.cpp)
    target_link_libraries(simoder-captured-candidate-tests PRIVATE sc13-semantic-test-support)
    sc13_enable_warnings(simoder-captured-candidate-tests)
    add_test(NAME simoder-captured-candidate-tests COMMAND simoder-captured-candidate-tests)
    set_tests_properties(simoder-captured-candidate-tests PROPERTIES TIMEOUT 30)
    target_compile_definitions(sc13-semantic-test-support PRIVATE SC13_SEMANTIC_BUILD_ID="${SC13_SEMANTIC_BUILD_ID}" SC13_SEMANTIC_BUILD_CONFIG="$<CONFIG>")
    target_link_libraries(sc13-semantic-test-support PUBLIC bcrypt version advapi32 user32 Luau.VM Luau.Compiler)
    sc13_enable_warnings(sc13-semantic-test-support)
    # @summary Exercise the production composition and retained close with original owners and callbacks.
    add_executable(bootstrap-runtime-context-tests tests/bootstrap_runtime_context_tests.cpp
        tests/mod_fixture.cpp tests/test_harness.cpp)
    target_link_libraries(bootstrap-runtime-context-tests PRIVATE sc13-semantic-test-support)
    sc13_enable_warnings(bootstrap-runtime-context-tests)
    foreach(context_case prepare root-failures reentry foreign-writer delivery capacity allocation capture manager-lease)
        add_test(NAME simoder-bootstrap-runtime-context-${context_case}
            COMMAND bootstrap-runtime-context-tests ${context_case} "${CMAKE_CURRENT_SOURCE_DIR}/.analysis")
        set_tests_properties(simoder-bootstrap-runtime-context-${context_case} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(simoder-semantic-bridge-tests tests/semantic_bridge_tests.cpp)
    target_link_libraries(simoder-semantic-bridge-tests PRIVATE sc13-semantic-test-support)
    sc13_enable_warnings(simoder-semantic-bridge-tests)
    # @summary Qualify genuine script participant identity without native image execution.
    add_executable(simoder-semantic-participant-tests tests/semantic_participant_tests.cpp)
    target_link_libraries(simoder-semantic-participant-tests PRIVATE sc13-semantic-test-support)
    sc13_enable_warnings(simoder-semantic-participant-tests)
    if(SC13_BASE_SOURCE_DIR)
        # @summary Qualify the actual closed native adapter through the existing Base/scope fixture path.
        add_executable(simoder-native-provider-tests tests/native_provider_tests.cpp)
        target_link_libraries(simoder-native-provider-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-native-provider-tests)
        add_test(NAME simoder-native-provider-tests COMMAND simoder-native-provider-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-native-provider-tests PROPERTIES TIMEOUT 90)
        # @summary Qualify original task delivery and the shared native reference ceiling in the private host.
        add_executable(simoder-native-provider-poll-tests tests/native_provider_poll_tests.cpp)
        target_link_libraries(simoder-native-provider-poll-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-native-provider-poll-tests)
        add_test(NAME simoder-native-provider-poll-tests COMMAND simoder-native-provider-poll-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-native-provider-poll-tests PROPERTIES TIMEOUT 90)
        # @summary Exhaust the checked process-wide handle range only in its own fresh fixture process.
        add_test(NAME simoder-native-provider-range-exhaustion-tests COMMAND simoder-native-provider-poll-tests
            "${SC13_BASE_SOURCE_DIR}" --block-exhaustion)
        set_tests_properties(simoder-native-provider-range-exhaustion-tests PROPERTIES TIMEOUT 90)
        # @summary Qualify original task publication sequence and revision with the actual isolated Base/writer path.
        add_executable(simoder-task-publication-metadata-tests tests/task_publication_metadata_tests.cpp)
        target_link_libraries(simoder-task-publication-metadata-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-task-publication-metadata-tests)
        add_test(NAME simoder-task-publication-metadata-tests COMMAND simoder-task-publication-metadata-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-task-publication-metadata-tests PROPERTIES TIMEOUT 90)
        # @summary Exercise actual captured preparation without operational or production activation.
        add_executable(simoder-manager-preparation-tests tests/manager_preparation_tests.cpp tests/mod_fixture.cpp tests/test_harness.cpp)
        target_link_libraries(simoder-manager-preparation-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-manager-preparation-tests)
        add_test(NAME simoder-manager-preparation-tests COMMAND simoder-manager-preparation-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-manager-preparation-tests PROPERTIES TIMEOUT 60)
        # @summary Actual canonical joint transaction uses the separate native fixture authority library only.
        add_executable(simoder-manager-publication-tests tests/manager_publication_tests.cpp tests/mod_fixture.cpp tests/test_harness.cpp)
        target_link_libraries(simoder-manager-publication-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-manager-publication-tests)
        add_test(NAME simoder-manager-publication-tests COMMAND simoder-manager-publication-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-manager-publication-tests PROPERTIES TIMEOUT 60)
        # @summary Real scheduler outputs exercise the isolated exact native consumer without production ingress.
        add_executable(simoder-manager-commands-tests tests/manager_commands_tests.cpp tests/mod_fixture.cpp tests/test_harness.cpp)
        target_link_libraries(simoder-manager-commands-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-manager-commands-tests)
        add_test(NAME simoder-manager-commands-tests COMMAND simoder-manager-commands-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-manager-commands-tests PROPERTIES TIMEOUT 90)
        add_executable(simoder-manager-close-tests tests/manager_close_tests.cpp tests/mod_fixture.cpp tests/test_harness.cpp)
        target_link_libraries(simoder-manager-close-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-manager-close-tests)
        add_test(NAME simoder-manager-close-tests COMMAND simoder-manager-close-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-manager-close-tests PROPERTIES TIMEOUT 90)
        # @summary Qualify exact native scope retirement without production authority or game effects.
        add_executable(simoder-scope-retirement-tests tests/scope_retirement_tests.cpp tests/mod_fixture.cpp tests/test_harness.cpp)
        target_link_libraries(simoder-scope-retirement-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-scope-retirement-tests)
        add_test(NAME simoder-scope-retirement-tests COMMAND simoder-scope-retirement-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-scope-retirement-tests PROPERTIES TIMEOUT 90)
        # @summary Exercise original-host VM enrollment and actual control/close races without production issuance.
        add_executable(simoder-manager-vm-enrollment-tests tests/manager_vm_enrollment_tests.cpp tests/mod_fixture.cpp tests/test_harness.cpp)
        target_link_libraries(simoder-manager-vm-enrollment-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-manager-vm-enrollment-tests)
        add_test(NAME simoder-manager-vm-enrollment-tests COMMAND simoder-manager-vm-enrollment-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-manager-vm-enrollment-tests PROPERTIES TIMEOUT 90)
        # @summary Qualify explicit off-lane original-record VM retirement and actual close-pin lifetime races.
        add_executable(simoder-manager-vm-retirement-tests tests/manager_vm_retirement_tests.cpp tests/mod_fixture.cpp tests/test_harness.cpp)
        target_link_libraries(simoder-manager-vm-retirement-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-manager-vm-retirement-tests)
        add_test(NAME simoder-manager-vm-retirement-tests COMMAND simoder-manager-vm-retirement-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-manager-vm-retirement-tests PROPERTIES TIMEOUT 90)
        # @summary Qualify original whole-Plan VM retirement and exact lifecycle reservation ownership.
        # @summary Qualify the isolated bounded native state writer without production issuance.
        add_executable(simoder-state-path-writer-tests tests/state_path_writer_tests.cpp)
        target_link_libraries(simoder-state-path-writer-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-state-path-writer-tests)
        add_test(NAME simoder-state-path-writer-tests COMMAND simoder-state-path-writer-tests "${CMAKE_CURRENT_SOURCE_DIR}/.analysis")
        set_tests_properties(simoder-state-path-writer-tests PROPERTIES TIMEOUT 90)
        add_executable(simoder-prepared-retirement-tests tests/prepared_retirement_tests.cpp tests/mod_fixture.cpp tests/test_harness.cpp)
        target_link_libraries(simoder-prepared-retirement-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-prepared-retirement-tests)
        add_test(NAME simoder-prepared-retirement-tests COMMAND simoder-prepared-retirement-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-prepared-retirement-tests PROPERTIES TIMEOUT 90)
        # @summary Qualify isolated scheduler/registry/context/scope publication through one original marker.
        add_executable(simoder-joint-publication-tests tests/joint_publication_tests.cpp)
        target_link_libraries(simoder-joint-publication-tests PRIVATE sc13-semantic-test-support)
        sc13_enable_warnings(simoder-joint-publication-tests)
        add_test(NAME simoder-joint-publication-tests COMMAND simoder-joint-publication-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-joint-publication-tests PROPERTIES TIMEOUT 60)
        add_test(NAME simoder-semantic-bridge-tests COMMAND simoder-semantic-bridge-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-semantic-bridge-tests PROPERTIES TIMEOUT 60)
        add_test(NAME simoder-semantic-participant-tests COMMAND simoder-semantic-participant-tests script "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-semantic-participant-tests PROPERTIES TIMEOUT 90)
    endif()
    # @summary Exercise verified private base hosting without production service readiness.
    add_executable(simoder-base-host-tests tests/base_host_tests.cpp)
    target_link_libraries(simoder-base-host-tests PRIVATE sc13_core)
    sc13_enable_warnings(simoder-base-host-tests)
    add_test(NAME simoder-base-host-tests COMMAND simoder-base-host-tests)
    set_tests_properties(simoder-base-host-tests PROPERTIES TIMEOUT 30)
    if(SC13_BASE_SOURCE_DIR)
        add_test(NAME simoder-base-package-tests COMMAND simoder-base-host-tests "${SC13_BASE_SOURCE_DIR}")
        set_tests_properties(simoder-base-package-tests PROPERTIES TIMEOUT 30)
    endif()
    # @summary Exercise the private strict copied-value arena without enabling semantic providers.
    add_executable(simoder-copied-value-tests tests/copied_value_tests.cpp)
    target_link_libraries(simoder-copied-value-tests PRIVATE sc13_core)
    sc13_enable_warnings(simoder-copied-value-tests)
    add_test(NAME simoder-copied-value-tests COMMAND simoder-copied-value-tests)
    set_tests_properties(simoder-copied-value-tests PROPERTIES TIMEOUT 30)
    # @summary Compile synthetic authority construction only into an independent, non-installed test library.
    add_library(sc13-runtime-test-support STATIC src/runtime/runtime_scope.cpp src/runtime/runtime_lifecycle.cpp
        src/runtime/copied_value.cpp src/runtime/semantic_tasks.cpp
        # @summary Link the real native publication primitive into isolated authority fixtures.
        src/mods/patch_registry.cpp src/core/runtime_property_table.cpp
        src/platform/runtime_owner.cpp)
    target_include_directories(sc13-runtime-test-support PUBLIC src)
    target_compile_definitions(sc13-runtime-test-support PUBLIC SIMODER_RUNTIME_TEST_AUTHORITY
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(sc13-runtime-test-support PUBLIC bcrypt advapi32)
    sc13_enable_warnings(sc13-runtime-test-support)
    foreach(suite runtime_scope runtime_lifecycle runtime_owner semantic_task)
        string(REPLACE "_" "-" suite_name "${suite}")
        add_executable(simoder-${suite_name}-tests tests/${suite}_tests.cpp)
        target_link_libraries(simoder-${suite_name}-tests PRIVATE sc13-runtime-test-support)
        sc13_enable_warnings(simoder-${suite_name}-tests)
        add_test(NAME simoder-${suite_name}-tests COMMAND simoder-${suite_name}-tests)
        set_tests_properties(simoder-${suite_name}-tests PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(simoder-game-window-dispatch-tests tests/game_window_dispatch_tests.cpp)
    target_link_libraries(simoder-game-window-dispatch-tests PRIVATE sc13_core user32)
    sc13_enable_warnings(simoder-game-window-dispatch-tests)
    add_test(NAME simoder-game-window-dispatch-tests COMMAND simoder-game-window-dispatch-tests)
    set_tests_properties(simoder-game-window-dispatch-tests PROPERTIES TIMEOUT 30)
    # @summary Verify actual world-unit unavailable-status serialization after complete owner-hook retirement.
    add_executable(simoder-native-bindings-status-tests tests/native_bindings_status_tests.cpp)
    target_include_directories(simoder-native-bindings-status-tests PRIVATE src)
    sc13_enable_warnings(simoder-native-bindings-status-tests)
    add_test(NAME simoder-native-bindings-status-tests COMMAND simoder-native-bindings-status-tests)
    set_tests_properties(simoder-native-bindings-status-tests PROPERTIES TIMEOUT 30)
    add_executable(simoder-native-session-transport-tests tests/native_session_transport_tests.cpp)
    target_link_libraries(simoder-native-session-transport-tests PRIVATE sc13_core user32)
    sc13_enable_warnings(simoder-native-session-transport-tests)
    add_test(NAME simoder-native-session-transport-tests COMMAND simoder-native-session-transport-tests)
    add_executable(simoder-window-message-hook-tests tests/window_message_hook_tests.cpp src/ui/window_message_hook.cpp)
    target_include_directories(simoder-window-message-hook-tests PRIVATE src)
    target_link_libraries(simoder-window-message-hook-tests PRIVATE user32)
    sc13_enable_warnings(simoder-window-message-hook-tests)
    add_test(NAME simoder-window-message-hook-tests COMMAND simoder-window-message-hook-tests)
    find_program(SC13_SESSION_TEST_NODE NAMES node)
    if(SC13_SESSION_TEST_NODE)
        # @summary Node is only a test host; the production DLL uses the game's native JavaScript context.
        add_test(NAME simoder-native-session-router-tests COMMAND "${SC13_SESSION_TEST_NODE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_session_router_tests.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/src/dev/session/session_router.js")
    endif()
    add_executable(simoder-action-worker-tests tests/action_worker_tests.cpp)
    target_link_libraries(simoder-action-worker-tests PRIVATE sc13_core)
    sc13_enable_warnings(simoder-action-worker-tests)
    add_test(NAME simoder-action-worker-tests COMMAND simoder-action-worker-tests)
    set_tests_properties(simoder-action-worker-tests PROPERTIES TIMEOUT 20)
    add_executable(simoder-authoring-tools-tests tests/authoring_tools_tests.cpp)
    target_link_libraries(simoder-authoring-tools-tests PRIVATE sc13_core)
    sc13_enable_warnings(simoder-authoring-tools-tests)
    add_test(NAME simoder-authoring-tools-tests COMMAND simoder-authoring-tools-tests)
    add_executable(sc13-tests tests/test_main.cpp tests/test_harness.cpp tests/core_primitive_tests.cpp
        tests/runtime_table_tests.cpp tests/binary_fixture.cpp tests/format_tests.cpp
        tests/mod_fixture.cpp tests/mod_parser_tests.cpp tests/patch_ownership_tests.cpp
        tests/configuration_tests.cpp tests/mod_lifecycle_tests.cpp tests/resource_integration_tests.cpp)
    target_link_libraries(sc13-tests PRIVATE sc13_core)
    sc13_enable_warnings(sc13-tests)
    add_test(NAME sc13-tests COMMAND sc13-tests)
    target_compile_definitions(sc13-tests PRIVATE SC13_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(simoder-script-tests tests/script_tests.cpp)
    target_link_libraries(simoder-script-tests PRIVATE sc13_core)
    sc13_enable_warnings(simoder-script-tests)
    add_test(NAME simoder-script-tests COMMAND simoder-script-tests)
    add_executable(simoder-script-scheduler-tests tests/script_scheduler_tests.cpp)
    target_link_libraries(simoder-script-scheduler-tests PRIVATE sc13_core)
    sc13_enable_warnings(simoder-script-scheduler-tests)
    add_test(NAME simoder-script-scheduler-tests COMMAND simoder-script-scheduler-tests)
    add_executable(simoder-script-lifecycle-transaction-tests tests/script_lifecycle_transaction_tests.cpp)
    target_link_libraries(simoder-script-lifecycle-transaction-tests PRIVATE sc13_core)
    sc13_enable_warnings(simoder-script-lifecycle-transaction-tests)
    add_test(NAME simoder-script-lifecycle-transaction-tests COMMAND simoder-script-lifecycle-transaction-tests)
    set_tests_properties(simoder-script-scheduler-tests simoder-script-lifecycle-transaction-tests PROPERTIES TIMEOUT 20)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME simoder-embedded-script-smoke COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/embedded_script_smoke.py" "$<TARGET_FILE:simoder-script-tests>")
    set_tests_properties(simoder-embedded-script-smoke PROPERTIES TIMEOUT 100)
    add_executable(simoder-service-tests tests/service_mod_tests.cpp)
    target_link_libraries(simoder-service-tests PRIVATE sc13_core)
    sc13_enable_warnings(simoder-service-tests)
    add_test(NAME simoder-service-tests COMMAND simoder-service-tests)
    add_executable(simoder-windows-arguments-tests tests/windows_arguments_tests.cpp)
    target_link_libraries(simoder-windows-arguments-tests PRIVATE sc13_core shell32)
    sc13_enable_warnings(simoder-windows-arguments-tests)
    add_test(NAME simoder-windows-arguments-tests COMMAND simoder-windows-arguments-tests)
    add_executable(simoder-patch-tests tests/declarative_patch_tests.cpp)
    target_link_libraries(simoder-patch-tests PRIVATE sc13_core)
    sc13_enable_warnings(simoder-patch-tests)
    add_test(NAME simoder-patch-tests COMMAND simoder-patch-tests)

    if(SC13_BUILD_RUNTIME)
        add_executable(simoder-proxy-probe WIN32 tests/proxy_probe.cpp tests/bootstrap_probe.rc)
        target_compile_definitions(simoder-proxy-probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(simoder-proxy-probe PRIVATE shell32)
        sc13_enable_warnings(simoder-proxy-probe)
        add_test(NAME simoder-integrated-bootstrap COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/single_file_bootstrap.py"
            "$<TARGET_FILE:simoder-proxy-probe>" "$<TARGET_FILE:simoder-runtime>")
    endif()


endif()

foreach(target sc13_core simoder-runtime sc13-tests simoder-script-tests)
    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        )
    endif()
endforeach()

# @summary Optionally qualify explicit canonical Base/SDK/Examples bytes in a non-installed native host.
# @details These profile roots are separate from the strict clean SIMODER_NATIVE_SDK_SOURCE_DIR dependency.
set(SC13_BASE_PROFILE_SDK_DIR "" CACHE PATH "Optional explicit canonical SDK profile root for Base conformance")
set(SC13_BASE_PROFILE_EXAMPLES_DIR "" CACHE PATH "Optional explicit canonical Examples profile root for Base conformance")
if(("${SC13_BASE_PROFILE_SDK_DIR}" STREQUAL "" AND NOT "${SC13_BASE_PROFILE_EXAMPLES_DIR}" STREQUAL "") OR
   (NOT "${SC13_BASE_PROFILE_SDK_DIR}" STREQUAL "" AND "${SC13_BASE_PROFILE_EXAMPLES_DIR}" STREQUAL ""))
    message(FATAL_ERROR "Base profile conformance requires both SC13_BASE_PROFILE_SDK_DIR and SC13_BASE_PROFILE_EXAMPLES_DIR")
endif()
if(NOT "${SC13_BASE_PROFILE_SDK_DIR}" STREQUAL "")
    if(NOT SC13_BUILD_TESTS)
        message(FATAL_ERROR "Requested Base profile conformance requires SC13_BUILD_TESTS")
    endif()
    if("${SC13_BASE_SOURCE_DIR}" STREQUAL "" OR NOT IS_DIRECTORY "${SC13_BASE_SOURCE_DIR}")
        message(FATAL_ERROR "Requested Base profile conformance requires an existing explicit SC13_BASE_SOURCE_DIR")
    endif()
    if(NOT IS_ABSOLUTE "${SC13_BASE_PROFILE_SDK_DIR}" OR NOT IS_DIRECTORY "${SC13_BASE_PROFILE_SDK_DIR}" OR
       NOT IS_ABSOLUTE "${SC13_BASE_PROFILE_EXAMPLES_DIR}" OR NOT IS_DIRECTORY "${SC13_BASE_PROFILE_EXAMPLES_DIR}")
        message(FATAL_ERROR "Requested SDK and Examples profile roots must be existing absolute directories")
    endif()
    if(NOT TARGET sc13-semantic-test-support OR NOT TARGET Luau.Ast)
        message(FATAL_ERROR "Base profile conformance requires the existing private semantic host and Luau.Ast")
    endif()
    # @summary Exercise actual cold Base and original canonical scripts with unchanged execution limits.
    add_executable(simoder-base-sdk-conformance-tests tests/base_sdk_conformance_tests.cpp)
    target_link_libraries(simoder-base-sdk-conformance-tests PRIVATE sc13-semantic-test-support Luau.Ast)
    sc13_enable_warnings(simoder-base-sdk-conformance-tests)
    add_test(NAME simoder-base-sdk-conformance-tests COMMAND simoder-base-sdk-conformance-tests
        "${SC13_BASE_SOURCE_DIR}" "${SC13_BASE_PROFILE_SDK_DIR}" "${SC13_BASE_PROFILE_EXAMPLES_DIR}")
    set_tests_properties(simoder-base-sdk-conformance-tests PROPERTIES TIMEOUT 60)
endif()

# @summary Qualify only explicitly retained native retirement owners, without a global close certificate.
if(SC13_BUILD_TESTS AND SC13_BASE_SOURCE_DIR)
    if(NOT TARGET sc13-semantic-test-support)
        message(FATAL_ERROR "Known-owner retirement qualification requires the existing private semantic fixture host")
    endif()
    add_executable(simoder-manager-retirement-inventory-tests tests/manager_retirement_inventory_tests.cpp
        tests/mod_fixture.cpp tests/test_harness.cpp)
    target_link_libraries(simoder-manager-retirement-inventory-tests PRIVATE sc13-semantic-test-support)
    sc13_enable_warnings(simoder-manager-retirement-inventory-tests)
    add_test(NAME simoder-manager-retirement-inventory-tests COMMAND simoder-manager-retirement-inventory-tests
        "${SC13_BASE_SOURCE_DIR}")
    set_tests_properties(simoder-manager-retirement-inventory-tests PROPERTIES TIMEOUT 90)
endif()

# @summary Qualify private same-State native file/memory coordination without production authority.
if(SC13_BUILD_TESTS AND SC13_BASE_SOURCE_DIR)
    if(NOT TARGET sc13-semantic-test-support)
        message(FATAL_ERROR "Canonical persistence qualification requires the existing private semantic fixture host")
    endif()
    add_executable(simoder-manager-persistence-tests tests/manager_persistence_tests.cpp
        tests/mod_fixture.cpp tests/test_harness.cpp)
    target_link_libraries(simoder-manager-persistence-tests PRIVATE sc13-semantic-test-support)
    sc13_enable_warnings(simoder-manager-persistence-tests)
    add_test(NAME simoder-manager-persistence-tests COMMAND simoder-manager-persistence-tests
        "${SC13_BASE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/.analysis")
    set_tests_properties(simoder-manager-persistence-tests PROPERTIES TIMEOUT 90)
endif()

# @summary Qualify the finite same-original-manager runtime/file/cache retirement cohort only in the private native fixture host.
if(SC13_BUILD_TESTS AND SC13_BASE_SOURCE_DIR)
    if(NOT TARGET sc13-semantic-test-support)
        message(FATAL_ERROR "Combined original-owner qualification requires the existing private semantic fixture host")
    endif()
    add_executable(simoder-manager-combined-owners-tests tests/manager_combined_owners_tests.cpp
        tests/mod_fixture.cpp tests/test_harness.cpp)
    target_link_libraries(simoder-manager-combined-owners-tests PRIVATE sc13-semantic-test-support)
    sc13_enable_warnings(simoder-manager-combined-owners-tests)
    add_test(NAME simoder-manager-combined-owners-tests COMMAND simoder-manager-combined-owners-tests
        "${SC13_BASE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/.analysis")
    set_tests_properties(simoder-manager-combined-owners-tests PROPERTIES TIMEOUT 90)
endif()
