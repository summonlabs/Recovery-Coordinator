# Recovery Coordinator

```
# DCCP-Boundary:        52
# Boundary-Name:        Facility Recovery Coordination Boundary
# Boundary-Slug:        recovery-coordinator
# Version:              1.0.0
# License:              Apache-2.0
# Language:             C++20
# Build-System:         CMake >= 3.25 (Ninja, MSVC 2022 / GCC 12+ / Clang 15+)
# Repository:           https://github.com/summonlabs/Recovery-Coordinator
# Upstream-Release:     v1.0.0
# Release-Tag:          v1.0.0
# Release-Date:         2026-01-01
# Scope:                cross-domain facility recovery sequencing across assets,
#                       power, cooling, capacity, ASI, DFI and dependent service
#                       obligations, over the plan DAG and its durable record
# Authority:            owns recovery-plan identity, dependency-aware sequencing,
#                       readiness gates, bounded requests to adjacent authorities,
#                       durable attempt identity, verified-effect tracking,
#                       compensation, point-of-no-return semantics, replan and
#                       stale-authority fencing, completion proof and audit state
# Excludes:             incident lifecycle, power switching, cooling actuation,
#                       capacity truth, hardware lifecycle state, rack evacuation
#                       execution, ASI workload recovery, DFI network recovery,
#                       facility policy definition
# Determinism:          no clock, random device, host name, process id or hash
#                       iteration order influences any decision or any canonical byte
# Stability:            durable record format and canonical encodings are frozen at
#                       version 1; changing them requires a version bump and a
#                       migration test
```

## What this is

Recovery Coordinator is the DCCP boundary 52 implementation. It answers one
question, and refuses to answer any other:

> Given a facility incident and the current authoritative evidence, what ordered
> recovery plan is valid *now*, which adjacent authorities must act, what
> dependencies gate each step, and when must the plan pause, replan, compensate,
> or be fenced stale?

It is a coordinator, not a controller. It decides **what to ask for, in what
order, and whether what happened matched what was asked**. It never decides how.

## The four sentences the whole design defends

1. **A plan is a proposal, not authority.** Publishing a plan changes nothing
   outside this process.
2. **A dispatched request is not an effect.** The attempt is recorded before the
   request leaves, and it stays unproven until evidence says otherwise.
3. **An acknowledgement is not completion.** `Acknowledged` and `Verified` are
   different states with different causes.
4. **A verified effect must satisfy the exact expectation bound to that attempt,
   under authority that is still current.** Anything else is `Unverifiable`, and
   `Unverifiable` is never silently promoted.

## Ownership boundary

| The coordinator owns | The coordinator does not own |
| --- | --- |
| plan identity, revisions, goals and scopes | incident lifecycle |
| dependency-aware sequencing over a plan DAG | power switching |
| readiness gates over accepted evidence | cooling actuation |
| bounded requests to adjacent authorities | capacity truth |
| durable attempt identity and idempotency keys | hardware lifecycle state |
| acknowledgement / observation / verified-effect tracking | rack evacuation execution |
| compensation and rollback semantics | ASI workload recovery |
| point-of-no-return semantics | DFI network recovery |
| replan and stale-authority fencing | facility policy definition |
| completion proof and durable audit state | |

If a decision is not in the left column, this boundary does not make it. It
escalates and records that it escalated.

## Public API

Everything lives behind `RecoveryCoordinator::recovery_coordinator`. The surface
is deliberately small: 27 methods, all of them `[[nodiscard]]`, none of them
throwing.

**Open and close**

```cpp
Result<RecoveryReport> open(Epoch requestedEpoch, std::string incarnation);
void                   shutdown();
Epoch                  epoch() const noexcept;
std::string            incarnation() const;
std::uint64_t          sequence() const noexcept;
```

**Authority and evidence**

```cpp
Result<AuthorityRef>          accept_authority(Domain domain, const AuthorityRef& reference);
Result<AuthorityRef>          retire_authority(Domain domain, std::string_view reason);
std::optional<AuthorityRef>   current_authority(Domain domain) const;
Result<EvidenceId>            observe(const AdapterObservation& observation);
std::vector<Observation>      observations(Domain domain) const;
```

**Plans**

```cpp
Result<PlanId>          propose_plan(const PlanDefinition& definition);
Result<PlanId>          publish_plan(const PlanId& plan);
Result<bool>            supersede_plan(const PlanId& plan, std::string_view reason);
std::optional<Plan>     plan(const PlanId& id) const;
std::vector<PlanId>     plan_ids() const;
```

**Executions**

```cpp
Result<ExecutionId>             start_execution(const PlanId& plan);
std::optional<ExecutionView>    execution(const ExecutionId& id) const;
std::vector<ExecutionId>        execution_ids() const;
Result<bool>                    cancel_execution(const ExecutionId& id, std::string_view reason);
Result<bool>                    begin_compensation(const ExecutionId& id, std::string_view reason);
Result<bool>                    abandon_execution(const ExecutionId& id, std::string_view reason);
```

**Driving and auditing**

```cpp
Result<TickReport>                  tick();          // synchronous, deterministic, idempotent
void                                start_worker();  // the same tick() on a thread
void                                stop_worker();
bool                                worker_running() const noexcept;
std::string                         snapshot_json() const;
Result<std::vector<ExecutionId>>    audit_authority();
```

`tick()` is the whole engine. Calling it twice with no new evidence changes
nothing the second time; `TickReport::idle` says so.

## Invariants

These are enforced in code and defended by tests, not aspirations.

1. A plan is a proposal, not authority.
2. A dispatched request is not an effect.
3. An acknowledgement is not verified completion.
4. A verified effect must satisfy the exact expectation bound to that attempt and
   must still be under currently accepted authority for the domain.
5. Every plan, step, attempt and execution is bound to external authority
   generations and content digests. Nothing is bound to a wall clock or a name.
6. A relevant change **fences or replans deterministically**. The coordinator
   never silently inherits authority, evidence or a plan across a change.
7. A dependency join is satisfied only when *every* incoming edge is satisfied.
   There is no quorum semantics.
8. A readiness gate that cannot be evaluated is **not** satisfied.
9. An attempt is durably recorded **before** any consequential external dispatch.
10. After a restart, an unresolved ambiguous request is **never** automatically
    reissued. Only a proven "this authority has no record of that key" permits a
    re-dispatch, and the re-dispatch uses a new key.
11. Compensation is bounded, never automatic, and refused once a point of no
    return has been verified.
12. `Complete` requires every step `Verified`. There is no partial completion.
13. Terminal states are never reopened.
14. One mutex, one lock order. Adapters are called under the lock and may not
    re-enter the engine.
15. A journal is authoritative only when every committed record verifies. A torn
    tail is truncated and reported; interior corruption refuses the file.

## Build

```powershell
# Windows, MSVC 2022 Build Tools + Ninja
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/release'
```

```bash
# Linux / macOS
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

CMake options: `RC_BUILD_TESTS` (ON), `RC_BUILD_TOOLS` (ON),
`RC_BUILD_BENCHMARKS` (ON), `RC_WARNINGS_AS_ERRORS` (ON),
`RC_ENABLE_SANITIZERS` (OFF, non-MSVC toolchains only).

Every option above changes what is built or how it is checked. There is no
declared-but-inert switch, because a switch that does nothing is worse than no
switch. Malformed-input coverage lives in `tests/suite_adversarial.cpp`, where
it is deterministic and runs on every supported toolchain.

First-party warnings are errors. A build that emits one is not a passing build.

## Test

```powershell
build/release/tests/recovery_tests.exe                    # everything
build/release/tests/recovery_tests.exe --filter journal   # by name fragment
ctest --test-dir build/release --output-on-failure
```

The suites use no timeouts. A hang in this project is a defect, and the suite is
required to finish on its own.

### What the suites cover

| Suite | Tests | What it proves |
| --- | --- | --- |
| `suite_core` | 8 | SHA-256 against published vectors, digest strictness, canonical JSON, RFC 3339, identity |
| `suite_plan` | 14 | shape rejection (duplicate, cycle, self-edge, unknown predecessor, zero identity, empty name, missing or unused compensation, oversize), deterministic topology, content digest |
| `suite_authority` | 9 | generation regression refused, same-generation digest conflict refused, newer generation accepted, retired visible but not current, per-domain deltas |
| `suite_evidence` | 9 | newest generation wins, lower generation is history only, identity conflict refused, replay idempotent, freshness bound, fence and unfence, total order, domain isolation |
| `suite_attempt_lifecycle` | 6 | refusal retried to budget then `Refused`, unreachable adapter leaves an ambiguous attempt, indeterminate is never success, a proven absence re-dispatches only under the policy that permits it, and two fail-closed claim tests |
| `suite_gates` | 6 | each of the four blocking conditions with its exact reason, the passing control case, and a missing authority |
| `suite_compensation` | 3 | compensation walk to `Compensated`, point of no return to `Abandoned`, bounded rounds |
| `suite_journal` | 8 | round trip, reopen identity, torn tail truncated and reported, torn tail refused when disallowed, interior corruption refused whole, wrong checksum, header checksum, single-writer lock |
| `suite_adversarial` | 21 | malformed journals (bad magic, wrong version, truncated header, bad header CRC, oversize payload, missing trailer, wrong checksum, wrong chain, sequence gaps, splices from another journal) and malformed JSON |
| `suite_property` | 3 | randomized plans and schedules compared against an independent reference model |
| `suite_multiprocess` | 6 | real child processes killed at four points, lock contention and release by death, no duplicate application, no automatic reissue of an ambiguous request, restart liveness |
| `suite_end_to_end` | 3 | a four-step staged plan reaching `Complete`, evidence under a superseded authority never verifying, a completed claim without evidence never verifying |
| `suite_engine_replay` | 1 | an engine reconstructs its full state from a journal it wrote |

Measured on the release commit: **100 tests, 0 failures**, and `ctest` reports
`100% tests passed, 0 tests failed out of 1`.

## Inspect, validate, run

```powershell
build/release/tools/recoveryctl.exe verify --journal <path>
build/release/tools/recoveryctl.exe dump   --journal <path> [--json]
build/release/tools/coordinator_child.exe --journal <path> --world <path> --epoch <n> --incarnation <name>
```

`coordinator_child` is a real process that can be killed at a chosen point
(`--crash-after <n>`, `--crash-at-end`, `--lose-response-after <n>`). It exists
so that process death is *tested*, not reasoned about.

## Install and consume

```powershell
cmake --install build/release --prefix <prefix>
```

```cmake
find_package(RecoveryCoordinator 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE RecoveryCoordinator::recovery_coordinator)
```

## Benchmarks

```powershell
build/release/benchmarks/recovery_benchmarks.exe --directory <scratch-dir>
```

Every measurement reports its provenance. **REAL** means the work is done by the
shipped code on real files and real bytes. **SYNTHETIC** means the facility model
is synthetic — which is every facility effect in this repository, without
exception.

Measured on the release commit (MSVC 19.44, Release, single run):

| Scenario | Provenance | Scale | Throughput |
| --- | --- | --- | --- |
| canonical encode + SHA-256 | REAL | 200 000 framed and digested values | ~2.3 M ops/s |
| durable journal append with a device flush per record | REAL | single file | ~1 100 records/s |
| durable journal read-back with chain verification | REAL | single file | ~280 k records/s |
| plan shape validation | REAL | 2 000-step DAG, 3 997 edges | ~260 validations/s |
| ready-set computation | REAL | fan-out root + 2 999 dependents, ready width 2 999 | ~550 computations/s |
| end-to-end staged recovery | SYNTHETIC | 200-step chain, in-memory durable log | ~4 200 recoveries/s |

The end-to-end line is SYNTHETIC because the facility it recovers is the
synthetic model. It is not a claim about real hardware.

The benchmark fails closed: `--tick-budget <n>` lowers the end-to-end workload,
and the scenario then prints the lifecycle and per-step states it observed and
exits non-zero. That path is how the "a stalled recovery is reported, not hidden"
claim is tested rather than asserted.

## The adjacent authorities in this repository are synthetic

There is no real facility hardware behind this code, and nothing here claims
otherwise. The power, cooling, capacity, workload and network authorities used by
the tests and benchmarks are **synthetic models** implemented in
`src/adapters/synthetic.cpp`.

They are proof instruments. They enforce exactly-once application per
idempotency key, they keep an effect journal readable from outside the
coordinator, and they can be scripted to refuse, fail, answer ambiguously, lose a
response after applying the effect, or become unreachable entirely.

## How the durable layout is proven

Not by reading the code. By behaviour:

* a journal is written by one process, that process is killed, and a second
  process opens the same file and reconstructs the same state;
* every committed record is re-verified on open against its checksum **and** its
  chain, so a single flipped bit in a committed record is detected;
* a truncated append is repaired and reported while the committed prefix
  survives, and corruption followed by valid records refuses the whole file;
* a second process cannot take the single-writer lock while the first holds it,
  and can take it once the first is killed — because the kernel releases it;
* exactly-once application is counted in the authority's own effect journal
  across a kill and a restart, not inferred from the coordinator's state.

## Documentation

| Document | Contents |
| --- | --- |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | module map, lock model, durability protocol, state machines, exact semantics |
| [docs/OPERATIONS.md](docs/OPERATIONS.md) | running `recoveryctl`, what to do when an execution stops |
| [CONTRIBUTING.md](CONTRIBUTING.md) | build, test, style, frozen-format rules |

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Copyright 2026 Summon Labs.
