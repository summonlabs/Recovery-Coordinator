# Test suites. Each file is an independent translation unit that registers
# named tests; the runner in test_main.cpp executes and reports them.

set(RC_TEST_SOURCES
    test_main.cpp
    test_process.cpp)

file(GLOB RC_SUITE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/suite_*.cpp")
list(APPEND RC_TEST_SOURCES ${RC_SUITE_SOURCES})

if(RC_TEST_SUPPORT_SOURCES)
    list(APPEND RC_TEST_SOURCES ${RC_TEST_SUPPORT_SOURCES})
endif()

add_executable(recovery_tests ${RC_TEST_SOURCES})

target_include_directories(recovery_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/support
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(recovery_tests PRIVATE recovery_coordinator)

target_compile_features(recovery_tests PRIVATE cxx_std_20)

if(MSVC)
    target_compile_definitions(recovery_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(RC_WARNINGS_AS_ERRORS)
        target_compile_options(recovery_tests PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(recovery_tests PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(recovery_tests PRIVATE Threads::Threads)
    if(RC_WARNINGS_AS_ERRORS)
        target_compile_options(recovery_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

if(RC_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(recovery_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(recovery_tests PRIVATE -fsanitize=address,undefined)
endif()

# Tests are registered with CTest without a TIMEOUT property: this suite is
# required to complete naturally.
add_test(NAME recovery_tests COMMAND recovery_tests)
