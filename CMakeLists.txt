# Minimum CMake version required - 3.10 is standard on Debian 13
cmake_minimum_required(VERSION 3.10)

# Project name and version
project(webhook_server VERSION 0.0.1)

# Set C++ standard to C++17 (good balance of modern features and compatibility)
set(CMAKE_CXX_STANDARD 17)
# Require the standard (don't fall back to older versions)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add compiler flags for debugging and warnings
# -Wall: Enable all common warnings
# -Wextra: Enable extra warning flags
# -g: Generate debug information
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g")

# ASan + UBSan build (docs/security.md standing rule 6). Off by default;
# turn on for test/fuzz runs:
#   cmake -S . -B build-asan -DENABLE_SANITIZERS=ON && cmake --build build-asan
#   cd build-asan && ctest --output-on-failure
# -fno-sanitize-recover=all makes UBSan abort rather than log-and-continue,
# so CTest actually fails on a finding instead of passing with warnings.
option(ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
    set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer")

    # The runtime libraries ship separately from the compiler on most
    # distros. Probe before anything else so a missing package reports
    # itself, instead of surfacing as an unrelated find_package failure
    # further down (CMAKE_REQUIRED_FLAGS reaches the link line too).
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "${SANITIZER_FLAGS}")
    check_cxx_source_compiles("int main() { return 0; }" SANITIZER_RUNTIME_LINKS)
    unset(CMAKE_REQUIRED_FLAGS)

    if(NOT SANITIZER_RUNTIME_LINKS)
        message(FATAL_ERROR
            "ENABLE_SANITIZERS=ON but the sanitizer runtime does not link.\n"
            "Install it, then reconfigure:\n"
            "  Fedora/RHEL:    sudo dnf install libasan libubsan\n"
            "  Debian/Ubuntu:  sudo apt install libasan8 libubsan1\n"
            "  (clang users:   the runtime ships with compiler-rt)")
    endif()

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
    message(STATUS "Sanitizers enabled: address, undefined")
endif()

# Coverage-guided fuzzing via libFuzzer (clang only). The fuzz target
# still builds and runs without it — see the standalone driver in
# tests/fuzz_parse_request.cpp:
#   CXX=clang++ cmake -S . -B build-fuzz -DENABLE_LIBFUZZER=ON -DENABLE_SANITIZERS=ON
#   ./build-fuzz/tests/fuzz_parse_request -max_total_time=60
option(ENABLE_LIBFUZZER "Build fuzz targets against libFuzzer (requires clang)" OFF)
if(ENABLE_LIBFUZZER AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ENABLE_LIBFUZZER requires clang; got ${CMAKE_CXX_COMPILER_ID}")
endif()

# Include directories - tell compiler where to find header files
include_directories(${PROJECT_SOURCE_DIR}/include)

# Find required packages
# Threads are needed for handling multiple connections
find_package(Threads REQUIRED)

# Core library shared by the server binary and the tests
add_library(webhook_core STATIC
    src/http_server.cpp
    src/request_handler.cpp
)
target_link_libraries(webhook_core Threads::Threads)

# Define the executable - webhook_server is the output binary name
add_executable(webhook_server src/main.cpp)
target_link_libraries(webhook_server webhook_core)

# Set output directory for the binary
set_target_properties(webhook_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Tests: plain-assert executables wired into CTest (no framework)
enable_testing()
foreach(test_name test_parser test_routes test_server)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} webhook_core)
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Fuzz harness for the HTTP intake path. Asserts stay live regardless of
# build type: the harness checks parser invariants, not just crashes.
add_executable(fuzz_parse_request tests/fuzz_parse_request.cpp)
target_link_libraries(fuzz_parse_request webhook_core)
target_compile_options(fuzz_parse_request PRIVATE -UNDEBUG)
set_target_properties(fuzz_parse_request PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

if(ENABLE_LIBFUZZER)
    target_compile_definitions(fuzz_parse_request PRIVATE FUZZING_LIBFUZZER)
    target_compile_options(fuzz_parse_request PRIVATE -fsanitize=fuzzer)
    target_link_libraries(fuzz_parse_request -fsanitize=fuzzer)
    # libFuzzer runs until stopped; not a CTest case.
else()
    # Short bounded run so every `ctest` gets some fuzzing. Fixed seed,
    # so a failure here reproduces exactly. Longer campaigns:
    #   ./tests/fuzz_parse_request 2000000 <seed>
    add_test(NAME fuzz_parse_request_smoke
             COMMAND fuzz_parse_request 20000)
endif()

# Optional: Add install rules for systemd deployment
install(TARGETS webhook_server DESTINATION /usr/local/bin)
