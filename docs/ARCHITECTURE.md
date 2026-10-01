# Recovery Coordinator architecture

## 1. Boundary

Recovery Coordinator is DCCP boundary 52. It owns one thing: an
evidence-bound orchestration protocol for cross-domain facility recovery.

It owns:

* recovery-plan identity and revisions, goals and scopes;
* dependency-aware sequencing over a plan DAG;
* readiness gates evaluated against accepted evidence;
* bounded requests to adjacent authorities;
* durable attempt identity and idempotency keys;
* acknowledgement, observation and verified-effect tracking;
* compensation and rollback where it is physically meaningful;
* point-of-no-return semantics;
* replan and stale-authority fencing;
* recovery completion proof and durable audit state.

It does not own, and never models as its own authority: incident lifecycle,
power switching, cooling actuation, capacity truth, hardware lifecycle state,
rack evacuation execution, ASI workload recovery, DFI network recovery, or
facility policy definition.

Every facility change in this repository is performed by an *adjacent
authority*. In the shipped code the adjacent authorities are synthetic models
(see section 8); the coordinator's contract with them is the same contract it
would have with a real one.

## 2. Module map

```
include/recovery/
  result types and canonical encoding   error.hpp canonical.hpp digest.hpp sha256.hpp json.hpp
  identity and time                     id.hpp time.hpp epoch.hpp
  evidence and authority                observation.hpp authority.hpp
  plan model and validation             plan.hpp
  port surface (adapters, log, clock)   ports.hpp
  durability                            fileio.hpp lockfile.hpp journal.hpp
  orchestration                         engine.hpp
  process control (tests and tools)     process.hpp
  synthetic adjacent authorities        adapters/synthetic.hpp

src/
  the same modules, plus:
  codec.hpp/.cpp     the single definition of the durable record format
  engine_internal.hpp internal state types shared by the engine and the codec
  engine.cpp         decision logic (one file, deliberately)
  process.cpp        real child process control
  adapters/synthetic.cpp  the synthetic facility model

tools/      recoveryctl (inspect and validate), coordinator_child (a real process that can be killed)
tests/      the suites, plus a small self-contained framework in tests/test.hpp
benchmarks/ measurements with explicit REAL / SYNTHETIC provenance
```

## 3. Determinism

Nothing in the decision path reads the wall clock, a random device, a host
name, a process identifier, or a hash table iteration order.

* Time enters through `Clock`; tests use `FixedClock`.
* Identity is minted from a monotone per-incarnation counter, never from a
  clock or a device.
* Every collection that reaches a decision or a canonical encoding is ordered
  by an explicitly defined key.
* The evidence identity is *content derived*: the same observation content
  always produces the same evidence identity, which is what makes replay an
  idempotent no-op rather than a duplicate.

## 4. Threading and lock model

There is exactly one mutex in the engine. It protects all coordinator state.

* Every public engine entry point takes that one mutex with a plain
  `std::lock_guard`. There is one lock order in the process, so lock inversion
  is impossible by construction rather than by convention.
* Adapters are called only while the mutex is held. Adapter code must not call
  back into the engine; the engine enforces that with an `adapterActive` flag
  and refuses a re-entrant call with `engine.reentrant` instead of deadlocking.
* The worker thread runs the same `tick()` entry point a caller may run, so
  there is no second code path to audit.
* `shutdown()` signals the worker and joins it *without* holding the mutex, so
  the worker can always finish the tick it is running.
* The mutex is never held while waiting on a condition variable, joining a
  thread, or sleeping.

## 5. Durability protocol

### 5.1 Publication order

One journal file is one authoritative generation of coordinator state. A record
is committed when its bytes are on the device.

```
record := prefix(48) payload(N) chain(32) trailer(8)
prefix := magic(8) version(2) kind(2) payloadLength(4) uuid(16) sequence(8) checksum(4) reserved(4)
```

Publication order for one record is fixed:

1. The record is assembled in a fixed-size buffer; the checksum field is zero
   and the chain region is zero.
2. The chain value is derived over the previous chain value followed by
   prefix(48) + payload(N) + chain(32) as they stand, and written into the chain
   region.
3. The checksum is computed over prefix(48) + payload(N) + chain(32) with the
   checksum field zero and patched into the checksum field.
4. The trailer magic is appended, and the whole record is written and flushed
   to the device before success is returned.

A verifier repeats steps 2 and 3 over the bytes it read. Because the checksum
field is zero in both derivations, the checksum is a fixed point: writing the
computed value cannot invalidate the computation that produced it. This is the
property that makes a checksum verifiable at all, and it is the property that
the earlier drafts of this file got wrong.

### 5.2 Torn tail versus interior corruption

On open, the reader walks records from the header. Each record must be
structurally valid, checksum-correct, chain-correct, and exactly one past its
predecessor.

If the walk stops early, the remainder is inspected:

* if no structurally valid record exists anywhere after the stop point, the
  remainder is a **torn tail** — an append that never committed. It is
  truncated away, reported in `JournalOpenInfo`, and the committed prefix is
  preserved;
* if a structurally valid record does exist after the stop point, committed
  records follow the damage, and the journal is **refused as a whole**. The
  coordinator never guesses which half of a damaged generation to keep.

### 5.3 Single writer

The journal takes an exclusive kernel lock on `<journal>.lock`. The kernel
releases that lock when the holder dies, including an abrupt kill. A live
holder causes `lock.contended`; a dead holder never blocks a successor.

The lock grants mutual exclusion only. It carries no authority: a successor
must claim a strictly greater epoch before it may dispatch anything, and
evidence accepted by a previous incarnation is fenced so that no view is
current until it is presented again.

## 6. State machines

### 6.1 Execution lifecycle

```
Idle -> Running -> { Complete | Failed | Cancelled | Abandoned | Compensated }
                 -> Blocked -> { Running | Stale }
                 -> Stale
                 -> Compensating -> Compensated
```

Terminal states are never reopened. `Stale` means an authority the plan was
bound to is no longer current; the execution stops rather than adapting.
`Abandoned` means a point of no return was passed, so the coordinator refuses
to pretend the plan can be reversed.

### 6.2 Step states

```
Pending -> Ready -> Dispatched -> Acknowledged -> Executing -> Verified
                 -> Refused | Failed
                 -> Compensating -> Compensated | Uncompensable
```

An acknowledgement moves a step to `Acknowledged`. It never moves it to
`Verified`. Only an effect that was checked against the exact expectation bound
to that attempt, under authority that is still current, produces `Verified`.

### 6.3 Attempt states

```
Pending -> Dispatched -> { EffectPresent | Failed | Refused | Unverifiable }
                      -> Superseded
```

`Unverifiable` is an uncertain state, not a failure: the coordinator could not
obtain a definite outcome within the observation budget. `ambiguous()` marks an
attempt the coordinator cannot prove either way; such an attempt is never
reissued on the coordinator's own initiative.

## 7. Semantics

### 7.1 Readiness and joins

A step becomes ready when every dependency edge is satisfied:

* `await_verified` is satisfied by `Verified` (or `Compensated` while
  compensating);
* `await_resolved` is satisfied by `Verified`, `Failed`, or `Refused`;
* `on_failure` is satisfied by `Failed` or `Refused`.

A join is satisfied only when every incoming edge is satisfied. Partial
completion never advances a join: there is no quorum semantics anywhere in the
planner.

### 7.2 Readiness gates

A gate is evaluated against accepted evidence only. A gate fails when the
domain has no established view, when the view is not bound to currently
accepted authority, when the view is older than `max_age`, when a required
reading is absent, or when an expected value does not match. A gate that cannot
be evaluated is not satisfied.

A zero `Duration` in a freshness bound means **no bound was named**, not "the
evidence must have been taken at exactly this instant". The same convention
applies to `observation_budget`. A bound of zero that instead meant "infinitely
fresh" would make every default-constructed gate unsatisfiable and silently turn
"no requirement" into "impossible requirement".

### 7.3 Stale evidence and stale authority

Evidence carries the authority reference it was observed under. A view is
current only while that reference equals the currently accepted authority for
the domain and the view is inside its freshness bound.

Two things that are easy to conflate, and are deliberately kept apart:

* **authority identity** — who is entitled to speak for the domain: the
  authority name, the instance, and the authority generation. This is what an
  `AuthorityRef` commits to, and it changes only when authority itself changes.
* **state generation** — a monotone stamp on the domain's readings. It changes
  every time the domain's state changes.

Applying an effect changes state. It does **not** change who the authority is.
If applying an effect changed the authority reference, then the moment a step
succeeded the coordinator would consider its own authority stale, the next
readiness gate could never pass, and no multi-step recovery could ever finish.
A synthetic authority that bumps its authority generation on every applied
effect is therefore modelling the wrong thing, not exercising a safety
property.

What keeps invariant 4 strong is not generation churn but the pair of checks
below. An assessment is promoted to a verified effect only when **both** hold:

1. the assessment's own digests match the exact expectation the attempt was
   bound to (`expectation` and `observed` are compared, not just "it
   completed"); and
2. the authority reference attached to the assessment equals the reference the
   coordinator currently accepts for that domain.

Evidence presented under a superseded reference is refused as a verified effect
and stays unverifiable. A test proves exactly that.

When an authority changes, every execution bound to the old reference is
fenced: it records the fence, becomes `Stale`, and stops. The coordinator never
re-points a plan at new authority on its own.

### 7.4 Idempotency and restart

An attempt is recorded durably before any consequential request is dispatched.
The idempotency key is derived from the execution, the step, the attempt number,
the attempt kind, and the exact request binding, so replaying a dispatch after
a restart carries the identical key.

After a restart, an attempt that was in flight is never automatically
reissued. It is reconciled:

* `RestartPolicy::ReportOnly` never contacts the authority; the attempt is
  reported and left for an operator;
* `RestartPolicy::ReconcileOnly` (the default) asks the authority what it
  recorded and adopts that answer, but issues nothing;
* `RestartPolicy::ReconcileThenRedispatch` re-dispatches only when the
  authority proves it has no record of the key, which is proof that no effect
  was produced. The re-dispatch uses a new attempt number and therefore a new
  key.

### 7.5 Compensation

Compensation is a bounded reverse walk over the steps that produced a verified
effect. It is never automatic: an operator calls `begin_compensation`.

It is refused when a verified request had `point_of_no_return` set. In that
case the execution can only be abandoned, and it is recorded as `Abandoned`,
never as `Complete`.

The number of compensation rounds per execution is bounded.

### 7.6 Completion proof

An execution reaches `Complete` only when every step is `Verified`. There is no
"mostly complete", no partial success, and no completion inferred from the
absence of failures.

## 8. Synthetic adjacent authorities

The power, cooling, capacity, workload and network authorities used by the test
suites are **synthetic models**. Nothing in this repository talks to real
facility hardware, a BMS, a DCIM, a PDU, a UPS, a generator, a chiller, a CDU, a
rack controller, an accelerator, or an RDMA fabric.

The model exists to be a proof instrument, not a simulator:

* it enforces exactly-once application per idempotency key through a ledger that
  is written before the effect is reported, so a test can prove that a request
  the coordinator believed it had reissued was in fact applied once;
* it can be scripted to refuse, fail, answer ambiguously, lose a response after
  applying the effect, or become unreachable;
* it keeps an effect journal so the exactly-once claim is checkable from
  outside the coordinator.

## 9. Why the code is not the proof

The implementation is not evidence that the layout is correct. The layout is
proven by tests that read and write real files and by a lock that the kernel
releases when a holder is killed. A green build proves only that the code
compiles. In particular:

* journal framing is proven by appending to a real file, killing the writer, and
  reopening;
* single-writer exclusion is proven by a second process failing to take the
  lock while the first holds it, and succeeding once the first is killed;
* no-duplicate-dispatch is proven by counting applications in the authority's
  effect journal across a kill and a restart.
