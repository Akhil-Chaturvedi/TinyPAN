# TinyPAN CMake Build
#
# !! HOST-MACHINE UNIT TEST HARNESS ONLY !!
#
# This CMakeLists.txt is used to build and run TinyPAN's test suite on a
# standard Linux/macOS/Windows host. It fetches a standalone copy of lwIP
# purely to compile and test the protocol logic.
#
# DO NOT use this build file to integrate TinyPAN into a target RTOS project:
#
#  - ESP-IDF: Use `idf_component_register(...)`. The ESP-IDF framework already
#             links its own lwIP. Ensure `TINYPAN_ENABLE_LWIP=1` is set to
#             compile the netif adapter, but do not set `TINYPAN_FETCH_LWIP_TEST_HARNESS`.
#
#  - Zephyr: Use `zephyr_library_named(tinypan)` in a Zephyr module. 
#            Zephyr manages lwIP through its own Kconfig and CMake infrastructure.
#
cmake_minimum_required(VERSION 3.12)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
project(TinyPAN VERSION 0.1.0 LANGUAGES C)

# C Standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(TINYPAN_BUILD_TESTS "Build tests" ON)
option(TINYPAN_USE_MOCK_HAL "Use mock HAL for testing" ON)
option(TINYPAN_ENABLE_LWIP "Enable lwIP runtime integration" ON)
option(TINYPAN_FETCH_LWIP_TEST_HARNESS "Fetch standalone lwIP for host-machine tests" ON)

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

# Fetch lwIP if enabled for host verification
if(TINYPAN_FETCH_LWIP_TEST_HARNESS)
    include(FetchContent)
    message(STATUS "Downloading lwIP stack...")
    FetchContent_Declare(
        lwip
        URL https://github.com/lwip-tcpip/lwip/archive/refs/tags/STABLE-2_1_3_RELEASE.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP true
    )
    
    # Policy CMP0169: Suppress FetchContent_Populate deprecation warning
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()

    # Manually populate so we can use the sources WITHOUT adding lwIP's own
    # CMake project (which adds its own targets and checks for Doxygen).
    FetchContent_GetProperties(lwip)
    if(NOT lwip_POPULATED)
        FetchContent_Populate(lwip)
    endif()

    # Define minimal lwIP library
    # Load lwIP's official source lists to avoid brittle hardcoding
    set(LWIP_DIR ${lwip_SOURCE_DIR})
    include(${lwip_SOURCE_DIR}/src/Filelists.cmake)
    
    # Assemble required lwIP files
    set(LWIP_SOURCES
        ${lwipcore_SRCS}
        ${lwipcore4_SRCS}
        ${lwipnetif_SRCS}
    )
    add_library(lwip_lib STATIC ${LWIP_SOURCES})
    target_include_directories(lwip_lib PUBLIC
        ${lwip_SOURCE_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include # For lwipopts.h
    )
endif()

# TinyPAN Core Library
set(TINYPAN_SOURCES
    src/tinypan.c
    src/tinypan_bnep.c
    src/tinypan_transport.c
    src/tinypan_bnep_transport.c
    src/tinypan_slip_transport.c
    src/tinypan_supervisor.c
)

if(TINYPAN_ENABLE_LWIP)
    list(APPEND TINYPAN_SOURCES src/tinypan_lwip_netif.c)
else()
    list(APPEND TINYPAN_SOURCES src/tinypan_lwip_stub.c)
endif()

add_library(tinypan STATIC ${TINYPAN_SOURCES})

target_compile_definitions(tinypan PUBLIC
    TINYPAN_ENABLE_LWIP=$<BOOL:${TINYPAN_ENABLE_LWIP}>
)

target_include_directories(tinypan PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# HAL Selection
if(TINYPAN_USE_MOCK_HAL)
    add_library(tinypan_hal_mock STATIC hal/mock/tinypan_hal_mock.c)
    target_compile_definitions(tinypan_hal_mock PUBLIC
        TINYPAN_ENABLE_LWIP=$<BOOL:${TINYPAN_ENABLE_LWIP}>
    )
    target_include_directories(tinypan_hal_mock PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/hal/mock
    )
    if(TINYPAN_FETCH_LWIP_TEST_HARNESS)
        target_include_directories(tinypan_hal_mock PUBLIC
            ${lwip_SOURCE_DIR}/src/include
        )
    endif()
    # Link HAL before lwIP since it depends on it
    target_link_libraries(tinypan PUBLIC tinypan_hal_mock)
endif()

if(TINYPAN_FETCH_LWIP_TEST_HARNESS)
    target_include_directories(tinypan PUBLIC
        ${lwip_SOURCE_DIR}/src/include
    )
    target_link_libraries(tinypan PUBLIC lwip_lib)
endif()

# Tests
if(TINYPAN_BUILD_TESTS)
    enable_testing()

    find_program(CMAKE_SIZE_TOOL NAMES size llvm-size)
    
    # BNEP Unit Tests
    add_executable(test_bnep tests/test_bnep.c src/tinypan_bnep.c)
    target_include_directories(test_bnep PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    # Link mock HAL first, then lwIP (since mock HAL depends on lwIP)
    if(TINYPAN_USE_MOCK_HAL)
        target_link_libraries(test_bnep tinypan_hal_mock)
    endif()

    if(TINYPAN_FETCH_LWIP_TEST_HARNESS)
        target_include_directories(test_bnep PRIVATE
            ${lwip_SOURCE_DIR}/src/include
        )
        target_link_libraries(test_bnep lwip_lib)
    endif()
    
    add_test(NAME BNEPTests COMMAND test_bnep)

    # Supervisor State-Machine Tests
    add_executable(test_supervisor tests/test_supervisor.c)
    target_include_directories(test_supervisor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/hal/mock
    )
    # Link order: tinypan -> tinypan_hal_mock -> lwip_lib
    target_link_libraries(test_supervisor tinypan)

    add_test(NAME SupervisorTests COMMAND test_supervisor)

    # Integration Flow Test (simulated DHCP framing and state flow)
    add_executable(test_integration
        tests/test_integration.c
        tests/dhcp_sim.c
    )
    target_include_directories(test_integration PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/hal/mock
    )
    target_link_libraries(test_integration tinypan)

    add_test(NAME IntegrationFlowTests COMMAND test_integration)


endif()

# Print configuration summary
message(STATUS "TinyPAN Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build tests: ${TINYPAN_BUILD_TESTS}")
message(STATUS "  Use mock HAL: ${TINYPAN_USE_MOCK_HAL}")
message(STATUS "  Enable lwIP hooks: ${TINYPAN_ENABLE_LWIP}")
