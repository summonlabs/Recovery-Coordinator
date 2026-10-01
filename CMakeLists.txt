cmake_minimum_required(VERSION 3.25)

project(RecoveryCoordinator
    VERSION 1.0.0
    DESCRIPTION "DCCP boundary 52: evidence-bound cross-domain facility recovery orchestration"
    HOMEPAGE_URL "https://github.com/summonlabs/Recovery-Coordinator"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(RC_BUILD_TESTS "Build the Recovery Coordinator test suites" ON)
option(RC_BUILD_TOOLS "Build the Recovery Coordinator command line tools" ON)
option(RC_BUILD_BENCHMARKS "Build the Recovery Coordinator benchmarks" ON)
option(RC_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(RC_ENABLE_SANITIZERS "Enable AddressSanitizer/UBSan where the toolchain supports it" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CROSSCOMPILING)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(recovery_coordinator
    src/canonical.cpp
    src/digest.cpp
    src/epoch.cpp
    src/error.cpp
    src/id.cpp
    src/json.cpp
    src/sha256.cpp
    src/time.cpp
    src/observation.cpp
    src/plan.cpp
    src/authority.cpp
    src/fileio.cpp
    src/lockfile.cpp
    src/journal.cpp
    src/ports.cpp
    src/codec.cpp
    src/engine.cpp
    src/adapters/synthetic.cpp
    src/process.cpp)

add_library(RecoveryCoordinator::recovery_coordinator ALIAS recovery_coordinator)

target_include_directories(recovery_coordinator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(recovery_coordinator PUBLIC cxx_std_20)

if(MSVC)
    target_compile_definitions(recovery_coordinator PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(RC_WARNINGS_AS_ERRORS)
        target_compile_options(recovery_coordinator PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(recovery_coordinator PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(recovery_coordinator PRIVATE Threads::Threads)
    if(RC_WARNINGS_AS_ERRORS)
        target_compile_options(recovery_coordinator PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror)
    else()
        target_compile_options(recovery_coordinator PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
endif()

if(MSVC)
    target_compile_options(recovery_coordinator PRIVATE $<$<CONFIG:Release>:/O2>)
endif()

if(RC_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(recovery_coordinator PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(recovery_coordinator PUBLIC -fsanitize=address,undefined)
endif()

if(RC_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(RC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(RC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

install(TARGETS recovery_coordinator
    EXPORT RecoveryCoordinatorTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/recovery DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(RC_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/RecoveryCoordinator)

install(EXPORT RecoveryCoordinatorTargets
    FILE RecoveryCoordinatorTargets.cmake
    NAMESPACE RecoveryCoordinator::
    DESTINATION ${RC_CMAKE_INSTALL_DIR})

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RecoveryCoordinatorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/RecoveryCoordinatorConfig.cmake
    INSTALL_DESTINATION ${RC_CMAKE_INSTALL_DIR})

# No ARCH_INDEPENDENT here. This package installs a compiled C++ static
# library and C++ headers, so an installed package is only usable by a consumer
# built for the same architecture and pointer size. ARCH_INDEPENDENT removes
# exactly that check from the generated version file, which would let a
# mismatched consumer accept the package and then fail at link time with an
# error that names nothing the consumer did wrong.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/RecoveryCoordinatorConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/RecoveryCoordinatorConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/RecoveryCoordinatorConfigVersion.cmake
    DESTINATION ${RC_CMAKE_INSTALL_DIR})

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/RecoveryCoordinator)