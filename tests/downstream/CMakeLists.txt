cmake_minimum_required(VERSION 3.25)

project(RecoveryCoordinatorDownstream
    VERSION 1.0.0
    DESCRIPTION "Downstream consumer that proves the installed RecoveryCoordinator package is usable"
    LANGUAGES CXX)

# This project is deliberately outside the coordinator source tree. It sees the
# coordinator only through find_package, exactly as a real consumer does: it
# includes no private header, reads no source file, and links no target that was
# not exported by the installed package.
find_package(RecoveryCoordinator 1.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE RecoveryCoordinator::recovery_coordinator)
target_compile_features(downstream_consumer PRIVATE cxx_std_20)

if(MSVC)
    set_target_properties(downstream_consumer PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_definitions(downstream_consumer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
endif()

install(TARGETS downstream_consumer RUNTIME DESTINATION bin)
