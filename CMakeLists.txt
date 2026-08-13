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

# Optional: Add install rules for systemd deployment
install(TARGETS webhook_server DESTINATION /usr/local/bin)
