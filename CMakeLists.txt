# TinyPAN CMake Build
cmake_minimum_required(VERSION 3.12)
project(TinyPAN VERSION 0.1.0 LANGUAGES C)

# C Standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(TINYPAN_BUILD_TESTS "Build tests" ON)
option(TINYPAN_USE_MOCK_HAL "Use mock HAL for testing" ON)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# TinyPAN Core Library
set(TINYPAN_SOURCES
    src/tinypan.c
    src/tinypan_bnep.c
    src/tinypan_supervisor.c
)

add_library(tinypan STATIC ${TINYPAN_SOURCES})

target_include_directories(tinypan PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# HAL Selection
if(TINYPAN_USE_MOCK_HAL)
    add_library(tinypan_hal_mock STATIC hal/mock/tinypan_hal_mock.c)
    target_include_directories(tinypan_hal_mock PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/hal/mock
    )
    target_link_libraries(tinypan PUBLIC tinypan_hal_mock)
endif()

# Tests
if(TINYPAN_BUILD_TESTS)
    enable_testing()
    
    # BNEP Unit Tests
    add_executable(test_bnep tests/test_bnep.c src/tinypan_bnep.c)
    target_include_directories(test_bnep PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    # Link mock HAL for the BNEP tests (needed for hal_bt_l2cap_send calls)
    if(TINYPAN_USE_MOCK_HAL)
        target_link_libraries(test_bnep tinypan_hal_mock)
    endif()
    
    add_test(NAME BNEPTests COMMAND test_bnep)
endif()

# Print configuration summary
message(STATUS "TinyPAN Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build tests: ${TINYPAN_BUILD_TESTS}")
message(STATUS "  Use mock HAL: ${TINYPAN_USE_MOCK_HAL}")
