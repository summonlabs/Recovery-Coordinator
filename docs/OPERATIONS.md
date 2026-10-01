# Recovery Coordinator operations

## 1. What you have

Three things ship:

| Artifact | Purpose |
| --- | --- |
| `recovery_coordinator` | the library an adjacent system embeds |
| `recoveryctl` | inspect, validate and drive a journal from a shell |
| `coordinator_child` | a real process that opens a journal, runs a plan, and can be killed |

## 2. Building

```powershell
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/release'
```

Install and consume from another project:

```powershell
cmake --install build/release --prefix <prefix>
# in the consumer:
#   find_package(RecoveryCoordinator 1.0 REQUIRED)
#   target_link_libraries(app PRIVATE RecoveryCoordinator::recovery_coordinator)
```

## 3. Reading a journal

```powershell
recoveryctl verify --journal <path>          # records, last sequence, claimed epoch, torn tail
recoveryctl dump   --journal <path>          # every committed record, human readable
recoveryctl dump   --journal <path> --json   # the same records as canonical JSON
recoveryctl snapshot --journal <path> [--world <path>]   # replay read-only, print durable state
recoveryctl check  --snapshot <path>         # is this snapshot canonically encoded?
recoveryctl plan   --snapshot <path> [--plan <id>]        # one stored plan, canonical JSON
```

`snapshot` replays an in-process copy of the committed records. It never writes
to the operator's journal, and the snapshot it prints is explicitly *not* a
published generation: it names the epoch it claimed for the replay so that
nobody mistakes a read-only projection for coordinator state.

`verify` prints one of:

* `ok journal=<path> uuid=<id> records=<n> lastSequence=<n> lastClaimedEpoch=<n> tornTailRepaired=<bool> tornTailBytes=<n>`
  — the journal is authoritative and complete through `lastSequence`, and
  `lastClaimedEpoch` is the epoch a successor must exceed. Claim strictly more
  than that value, or the coordinator will refuse to start;
* `tornTailRepaired=true` with a positive `tornTailBytes` means an append never
  committed. The committed prefix is authoritative and the bytes were removed;
* a refusal names the reason. In that case **do not** delete the file: the
  damaged generation is evidence. Copy it aside first.

A refused journal means the coordinator has no authoritative state. It will not
start from it and it will not guess. Recovery from a refusal is an operator
decision: either present a known-good copy, or accept a new generation from
scratch, which discards the history the damaged file represents.

## 4. Running a plan

```powershell
coordinator_child --journal <path> --world <path> --epoch <n> --incarnation <name>
```

Options that exist to make failure testable:

| Option | Effect |
| --- | --- |
| `--epoch <n>` | the epoch to claim; must be strictly greater than any epoch in the journal |
| `--incarnation <name>` | the human name recorded with the epoch |
| `--lose-response-after <n>` | the synthetic authority applies the effect and then the response is lost |
| `--crash-after <n>` | the process exits abruptly after record `n` |
| `--crash-at-end` | the process exits abruptly without a graceful close |

The child prints one summary line per tick and a final
`result lifecycle=<state> steps=<n> attempts=<n> records=<n>`.

## 5. What an operator does when an execution stops

The coordinator never makes a physical decision. When it stops, it names why and
waits for a human.

### `Blocked`

No step can become ready with the evidence available, or a step failed and its
attempt budget is spent. The coordinator has not chosen to reverse anything.

Options, in order of preference:

1. present the evidence the gate is waiting for, then `tick()`;
2. change the plan (a new revision) if the goal itself changed;
3. `begin_compensation()` if a reversal is physically meaningful and safe;
4. leave it blocked and escalate.

### `Stale`

An authority the plan was bound to is no longer current. The execution is fenced
and has stopped.

The coordinator will not re-point the plan at the new authority, because the
plan was authored against the old authority's evidence. Options:

1. present fresh evidence under the new authority and author a new plan
   revision, then start a new execution;
2. abandon the execution if recovery is being handled outside this coordinator.

### `Abandoned`

A point of no return was verified; the plan cannot be reversed by this
coordinator. Recovery continues under whatever authority owns the irreversible
change.

### `Compensated`

Every verified step was reversed (or proven uncompensable). The facility is where
it was before the execution started. This is not `Complete`: the recovery goal
was not achieved.

### `Unverifiable` attempts

An attempt the coordinator could not resolve within the observation budget. It
is reported, never silently treated as a success or a failure. Ask the adjacent
authority what it recorded, then act.

## 6. Evidence and authority

The coordinator accepts authority per domain as a generation-stamped reference.
A reference is current only while it matches what was accepted.

Operationally:

* never edit a journal, an authority model, or a world file by hand — the
  checksum and chain will not match, and the coordinator will refuse the file;
* to retire authority, call `retire_authority()`; the reference stays visible in
  the audit trail but is no longer current;
* evidence accepted under a previous incarnation is fenced at restart. Present
  it again if it is still true: the coordinator will not assume it is.

## 7. Escalation checklist

1. Copy the journal, the world file, and any logs aside before touching anything.
2. Run `recoveryctl verify --journal <copy>` and record the exact line.
3. Run `recoveryctl dump --journal <copy> --json` and keep the output.
4. Record which execution stopped, its lifecycle, and the reason string the
   coordinator reported. The reason is durable and is part of the audit trail.
5. Only then decide: present evidence, replan, compensate, abandon, or escalate.
