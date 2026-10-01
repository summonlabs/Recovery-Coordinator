# Benchmarks measure completed useful operations, not submission latency.
# Every measurement reports its workload scale and its REAL / SYNTHETIC
# provenance.

add_executable(recovery_benchmarks benchmark_main.cpp)
target_link_libraries(recovery_benchmarks PRIVATE recovery_coordinator)
target_compile_features(recovery_benchmarks PRIVATE cxx_std_20)

# The ready-set benchmark measures the exact readiness function the engine
# dispatches from, and that function is declared next to the engine state it
# folds over. The benchmarks are part of this repository and are never
# installed, so they may see the private header; the downstream consumer in
# tests/downstream deliberately may not.
target_include_directories(recovery_benchmarks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

if(MSVC)
    target_compile_definitions(recovery_benchmarks PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(RC_WARNINGS_AS_ERRORS)
        target_compile_options(recovery_benchmarks PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(recovery_benchmarks PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    endif()
else()
    if(RC_WARNINGS_AS_ERRORS)
        target_compile_options(recovery_benchmarks PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()
