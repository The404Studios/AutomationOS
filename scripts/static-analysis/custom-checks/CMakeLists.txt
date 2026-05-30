# CMakeLists.txt for AutomationOS Custom Clang-Tidy Checks
cmake_minimum_required(VERSION 3.13)
project(automationos-clang-tidy-checks)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Add LLVM definitions and include directories
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# C++14 required for Clang-Tidy
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build the custom checks library
add_library(automationos-checks MODULE
    AutomationOSChecks.cpp
)

# Link against clangTidy
target_link_libraries(automationos-checks
    clangTidy
    clangTidyUtils
)

# Installation
install(TARGETS automationos-checks
    LIBRARY DESTINATION lib)

# Tests
enable_testing()
add_subdirectory(tests)
