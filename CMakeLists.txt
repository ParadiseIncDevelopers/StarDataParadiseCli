cmake_minimum_required(VERSION 4.2)
project(StarDataParadiseCLI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

include(CTest)
include(cmake/NativeSdk.cmake)
include(FetchContent)
set(SDP_JSON_ARCHIVE "" CACHE FILEPATH "nlohmann/json 3.12.0 release archive")
if(NOT SDP_JSON_ARCHIVE)
    set(SDP_JSON_ARCHIVE "https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz")
endif()
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    URL "${SDP_JSON_ARCHIVE}"
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(StarDataParadiseCLI
    StarDataParadiseCli/StarDataParadiseCli.cpp
    StarDataParadiseCli/terminal.cpp
    StarDataParadiseCli/connection.cpp)
target_compile_definitions(StarDataParadiseCLI PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(StarDataParadiseCLI PRIVATE
    StarDataParadise::StarDataParadiseDLLler nlohmann_json::nlohmann_json ws2_32)
add_custom_command(TARGET StarDataParadiseCLI POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:StarDataParadise::StarDataParadiseDLLler>" "$<TARGET_FILE_DIR:StarDataParadiseCLI>"
    VERBATIM)

if(BUILD_TESTING)
    add_test(NAME help COMMAND StarDataParadiseCLI --help)
    set_tests_properties(help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: StarDataParadiseCLI")
    add_test(NAME version COMMAND StarDataParadiseCLI --version)
    set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "^StarDataParadiseCLI 0[.]1[.]0")
    add_test(NAME invalid_option COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:StarDataParadiseCLI>" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_option.cmake")

    set(SDP_TEST_SERVER "" CACHE FILEPATH "Server EXE for CLI TLS integration")
    set(SDP_TEST_OPENSSL "" CACHE FILEPATH "OpenSSL tool for local test certificates")
    if(SDP_TEST_SERVER OR SDP_TEST_OPENSSL)
        if(NOT EXISTS "${SDP_TEST_SERVER}" OR NOT EXISTS "${SDP_TEST_OPENSSL}")
            message(FATAL_ERROR "CLI TLS test needs both SDP_TEST_SERVER and SDP_TEST_OPENSSL.")
        endif()
        find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
        add_test(NAME cli_tls COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.py"
            --cli "$<TARGET_FILE:StarDataParadiseCLI>" --dll "$<TARGET_FILE:StarDataParadise::StarDataParadiseDLLler>"
            --server "${SDP_TEST_SERVER}" --openssl "${SDP_TEST_OPENSSL}" --scratch "${CMAKE_CURRENT_BINARY_DIR}/instances")
        set_tests_properties(cli_tls PROPERTIES TIMEOUT 90)
    endif()
endif()
