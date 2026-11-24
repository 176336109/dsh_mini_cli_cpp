cmake_minimum_required(VERSION 3.22)
project(dsh_mini_cpp VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(dsh_deepseek_default ON)
else()
    set(dsh_deepseek_default OFF)
endif()
option(DSH_WITH_DEEPSEEK "Build the libcurl DeepSeek transport" "${dsh_deepseek_default}")

find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    set(json_include "${CMAKE_CURRENT_BINARY_DIR}/deps")
    set(json_header "${json_include}/nlohmann/json.hpp")
    set(json_url "https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp")
    file(DOWNLOAD "${json_url}" "${json_header}" TLS_VERIFY ON
         EXPECTED_HASH SHA256=AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63 TIMEOUT 180)
    add_library(dsh_json INTERFACE)
    target_include_directories(dsh_json SYSTEM INTERFACE "${json_include}")
    add_library(nlohmann_json::nlohmann_json ALIAS dsh_json)
endif()

add_library(dsh_core STATIC
    src/agent.cpp
    src/config.cpp
    src/deepseek.cpp
    src/mcp.cpp
    src/process.cpp
    src/quota.cpp
    src/session.cpp
    src/tools.cpp
    src/workspace.cpp
)
target_compile_features(dsh_core PUBLIC cxx_std_20)
target_include_directories(dsh_core PUBLIC src)
target_link_libraries(dsh_core PUBLIC nlohmann_json::nlohmann_json)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(dsh_core PRIVATE _GNU_SOURCE)
endif()
if(DSH_WITH_DEEPSEEK)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(DSH_CURL QUIET IMPORTED_TARGET libcurl)
        endif()
    endif()
    if(TARGET PkgConfig::DSH_CURL)
        target_link_libraries(dsh_core PRIVATE PkgConfig::DSH_CURL)
    else()
        find_package(CURL REQUIRED)
        target_link_libraries(dsh_core PRIVATE CURL::libcurl)
    endif()
    target_compile_definitions(dsh_core PRIVATE DSH_WITH_DEEPSEEK)
endif()

add_executable(dsh-mini src/main.cpp)
target_link_libraries(dsh-mini PRIVATE dsh_core)

function(dsh_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
dsh_warnings(dsh_core)
dsh_warnings(dsh-mini)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME offline_session
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/offline.py"
                     "$<TARGET_FILE:dsh-mini>")
    add_test(NAME file_tools
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/files.py"
                     "$<TARGET_FILE:dsh-mini>")
    add_executable(deepseek_protocol tests/deepseek.cpp)
    target_link_libraries(deepseek_protocol PRIVATE dsh_core)
    dsh_warnings(deepseek_protocol)
    add_test(NAME deepseek_protocol COMMAND deepseek_protocol)
    add_executable(history_budget tests/history.cpp)
    target_link_libraries(history_budget PRIVATE dsh_core)
    dsh_warnings(history_budget)
    add_test(NAME history_budget COMMAND history_budget)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME linux_tools
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux.py"
                         "$<TARGET_FILE:dsh-mini>")
    endif()
    if(DSH_WITH_DEEPSEEK)
        add_test(NAME deepseek_http
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/deepseek_http.py"
                         "$<TARGET_FILE:dsh-mini>")
    endif()
endif()
