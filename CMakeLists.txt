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
# -O2: Optimization level 2 (good for production)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g")

# Include directories - tell compiler where to find header files
include_directories(${PROJECT_SOURCE_DIR}/include)

# Find required packages
# Threads are needed for handling multiple connections
find_package(Threads REQUIRED)

# Define the executable - webhook_server is the output binary name
# main.cpp will be our entry point
add_executable(webhook_server
    src/main.cpp
    src/http_server.cpp
    src/request_handler.cpp
)

# Link required libraries to our executable
# Threads::Threads adds pthread library on Linux
target_link_libraries(webhook_server
    Threads::Threads
)

# Set output directory for the binary
set_target_properties(webhook_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Optional: Add install rules for systemd deployment
install(TARGETS webhook_server DESTINATION /usr/local/bin)
