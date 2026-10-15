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

# @summary Pin the statically linked bytecode compiler and VM to one revision.
include(FetchContent)
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_STATIC_CRT ON CACHE BOOL "" FORCE)
FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG 1f6ff5d6f0072f2fab2adf35d1ffbaae7d848ad0
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
    src/config/loader_config.cpp
    src/core/resource_filter.cpp
    src/core/runtime_property_table.cpp
    src/core/sha256.cpp
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
    src/mods/mod_definition_parser.cpp
    src/mods/action_worker.cpp
    src/mods/mod_candidate.cpp
    src/mods/mod_manager.cpp
    src/mods/mod_state_store.cpp
    src/mods/mod_types.cpp
    src/mods/resource_project.cpp
    src/mods/patch_registry.cpp
    src/reverse/pe_file.cpp
    src/reverse/build_compatibility.cpp
    src/runtime/runtime_resource_cache.cpp
    src/scripting/script_runtime.cpp
    src/scripting/script_patch_reducer.cpp
    src/scripting/luau_compiler.cpp
    src/scripting/source_project.cpp
)
target_include_directories(sc13_core PUBLIC src)
target_compile_definitions(sc13_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(sc13_core PUBLIC bcrypt version Luau.VM Luau.Compiler)
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
        src/dev/session/native_owner_capture.cpp
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
