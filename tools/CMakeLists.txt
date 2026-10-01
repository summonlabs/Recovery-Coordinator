# Command line tools. They are thin: every decision they make is a call into the
# library, and they exist so that the durable state can be inspected and driven
# by a real process.

add_executable(recoveryctl recoveryctl.cpp)
target_link_libraries(recoveryctl PRIVATE recovery_coordinator)
target_compile_features(recoveryctl PRIVATE cxx_std_20)

add_executable(coordinator_child coordinator_child.cpp)

target_link_libraries(coordinator_child PRIVATE recovery_coordinator)
target_compile_features(coordinator_child PRIVATE cxx_std_20)

if(MSVC)
    foreach(target recoveryctl coordinator_child)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
        if(RC_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
        else()
            target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        endif()
    endforeach()
else()
    foreach(target recoveryctl coordinator_child)
        if(RC_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endforeach()
endif()

install(TARGETS recoveryctl coordinator_child RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})