# @summary Verify canonical vendored native declarations without downloads or sibling discovery.
include("${CMAKE_CURRENT_LIST_DIR}/simoder-native-sdk.lock.cmake")
set(SIMODER_NATIVE_SDK_SOURCE_DIR "" CACHE PATH "Optional explicit clean pinned native SDK checkout")

# @summary Reject missing or changed canonical bytes before compilation.
# @param file_path Explicit declaration path. @param expected Reviewed SHA256.
function(simoder_verify_native_header file_path expected)
    if(NOT EXISTS "${file_path}" OR IS_DIRECTORY "${file_path}" OR IS_SYMLINK "${file_path}")
        message(FATAL_ERROR "Missing or aliased native SDK declaration: ${file_path}")
    endif()
    file(SHA256 "${file_path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Native SDK declaration drift: ${file_path}; refresh from the pinned canonical SDK")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file_path}")
endfunction()

simoder_verify_native_header("${CMAKE_CURRENT_LIST_DIR}/../src/mods/native/contract.h" "${SIMODER_NATIVE_HEADER_SHA256}")
simoder_verify_native_header("${CMAKE_CURRENT_LIST_DIR}/../src/mods/native/abi_v1.h" "${SIMODER_NATIVE_BASELINE_SHA256}")
if(SIMODER_NATIVE_SDK_SOURCE_DIR)
    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SIMODER_NATIVE_SDK_SOURCE_DIR}" rev-parse HEAD
        RESULT_VARIABLE revision_result OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SIMODER_NATIVE_SDK_SOURCE_DIR}" status --porcelain --untracked-files=normal
        RESULT_VARIABLE status_result OUTPUT_VARIABLE status OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT revision_result EQUAL 0 OR NOT status_result EQUAL 0 OR NOT revision STREQUAL SIMODER_NATIVE_SDK_REVISION OR NOT status STREQUAL "")
        message(FATAL_ERROR "Native SDK requires exact clean revision ${SIMODER_NATIVE_SDK_REVISION}; no fetch or fallback")
    endif()
    simoder_verify_native_header("${SIMODER_NATIVE_SDK_SOURCE_DIR}/Native/native.h" "${SIMODER_NATIVE_HEADER_SHA256}")
    simoder_verify_native_header("${SIMODER_NATIVE_SDK_SOURCE_DIR}/Native/abi_v1.h" "${SIMODER_NATIVE_BASELINE_SHA256}")
endif()
