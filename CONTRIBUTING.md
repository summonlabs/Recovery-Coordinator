# Contributing to Recovery Coordinator

## What this project is

Recovery Coordinator is the DCCP boundary 52 implementation: it owns
evidence-bound cross-domain facility recovery orchestration and nothing else. It
does not own incident lifecycle, power switching, cooling actuation, capacity
truth, hardware lifecycle state, rack evacuation execution, workload recovery,
network recovery, or facility policy definition.

Before changing behaviour, read `docs/ARCHITECTURE.md`. It states the
invariants the tests exist to defend.

## Build and test

```powershell
# Configure and build (MSVC 2022 Build Tools + Ninja)
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/release'

# Run the whole suite
build/release/tests/recovery_tests.exe

# Run one test by name fragment
build/release/tests/recovery_tests.exe --filter <fragment>
```

The build treats first-party warnings as errors. A change that introduces a
warning is not ready.

On a toolchain that supports it, configure with `-DRC_ENABLE_SANITIZERS=ON` to
build the suites under AddressSanitizer and UndefinedBehaviorSanitizer.

## Registering a test

Add a file named `tests/suite_<topic>.cpp`. The CMake glob picks it up after a
reconfigure. Use the framework in `tests/test.hpp`:

```cpp
#include "test.hpp"

RC_TEST(a_readiness_gate_blocks_dispatch_without_evidence) {
    RC_REQUIRE_EQ(some_observation.state, StepState::Pending);
}
```

A test must assert an invariant and must print the values that broke it. A test
that cannot fail is not a test.

## The durable format is frozen

The journal framing, the chain construction, the checksum fixed point, and the
canonical encodings are a compatibility surface. Changing any of them requires:

1. a format version bump in `include/recovery/journal.hpp`;
2. a written migration statement in `docs/ARCHITECTURE.md`;
3. a test that opens a journal written by the previous version.

## Style

* C++20, no exceptions in the public API: every fallible operation returns
  `Result<T>` and every error carries a stable machine-readable code.
* No undefined behaviour: no unchecked narrowing, no signed overflow, no
  uninitialised reads, no reliance on iterator invalidation.
* Deterministic output: canonical state must never depend on hash iteration
  order, the wall clock, thread timing, or a random device.
* Comments explain why. A comment that restates the code is noise.
* One lock order per process. If you add a second mutex, document the order in
  `docs/ARCHITECTURE.md` and prove there is no inversion.

## Review expectations

A change is ready when:

* the full suite passes from a clean build directory;
* a new or changed invariant has a test that fails without the change;
* the durable format is unchanged, or the migration rules above are satisfied;
* `README.md` and `docs/` still describe reality.
