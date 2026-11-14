# gridlane architecture

## 1. Purpose

gridlane is a design, not a product: a model for moving data and messages with
low latency, between processes on one machine or across machines.

- **Layered and loosely coupled.** Each layer is defined by a contract, can be
  extended with new APIs, and can be composed with the others or replaced.
- **Portable.** An application does not change when the transport below it
  changes; a configuration file selects it.
- **Focused on data distribution for parallel algorithms**, while the same
  layers let any two independent applications communicate.

This repository composes the layers and demonstrates the model end to end with
one application (section 8).

## 2. Layers

| Layer | Project | Role in the model | Extension point | Details |
|---|---|---|---|---|
| Transport | OS and third-party APIs | Moves bytes | A new pushseg backend | — |
| Memory | [pushseg](https://github.com/oguzhankynk/pushseg) | Segments: each process reads its own memory and writes into others' | `StreamBackend` / `MappedBackend` | [Memory contract](https://github.com/oguzhankynk/pushseg#memory-contract), [backend SPI](https://github.com/oguzhankynk/pushseg/blob/main/include/pushseg/backend.hpp) |
| Channel | [pairring](https://github.com/oguzhankynk/pairring) | Message boundaries, flow control, CRC, reconnect over remote-writable memory | `RemoteRegion` | [Protocol](https://github.com/oguzhankynk/pairring/blob/main/docs/protocol.md) |
| Messages | [wireforge](https://github.com/oguzhankynk/wireforge) | Typed, versioned message definitions and generated codecs | Any serializer: the channel carries byte strings | [Guide](https://github.com/oguzhankynk/wireforge/blob/master/docs/GUIDE.md), [schema](https://github.com/oguzhankynk/wireforge/blob/master/docs/SCHEMA_SPEC.md) |
| Composition | gridlane | Adapters, data distribution primitives, control conventions, runtime, demonstrator | New primitives and phases | This document |

**Why each layer**

- **pushseg** gives a shared-memory-like data plane with a written contract:
  bulk writes, untorn aligned stores, ordering points, notifications. The
  same contract holds over TCP, POSIX shared memory and any API plugged in
  as a backend, which is what makes the data plane portable.
- **pairring** turns remote-writable memory into a message channel: whole
  messages or none, a ring per direction, corrupt records never delivered,
  either side may restart. It needs only four operations from below, so it
  runs on pushseg or directly on mapped memory.
- **wireforge** defines the control messages once, in a model kept in this
  repository, and generates their code and tests. Versions and compatibility
  of the message set are checked by the tool, not by hand.

**Rule:** a layer never references another layer. Only gridlane knows all of
them, and joins them through adapters (section 3).

Transports exercised in version 1: TCP (a pushseg stream backend), POSIX
shared memory (a pushseg mapped backend), and in-process memory in tests. pushseg's mapped level is also where an external
memory API would plug in; version 1 does not use one.

## 3. Composition

```
 control plane                          data plane
 ┌────────────────────────┐
 │ wireforge codecs        │
 ├────────────────────────┤  adapter: channel ↔ transport interface of the codecs
 │ pairring Channel        │
 ├────────────────────────┤  adapter: RemoteRegion ↔ pushseg Connection
 │ pushseg                 │◄──────────── gridlane primitives (section 6)
 ├────────────────────────┤
 │ TCP / POSIX shm         │
 └────────────────────────┘
```

Two adapters, a few dozen lines each, live in `src/glue/`:

| Adapter | From | To |
|---|---|---|
| Region adapter | pairring `RemoteRegion` (`write`, `store64`, `notify`, `size`) | pushseg `Connection` (`write`, `store<uint64_t>`, `notify`, `size`); the local region is a `LocalSegment`, the doorbell wait its `wait_event` |
| Transport adapter | the codecs' transport interface (`send`, `receive`) | pairring `Channel` (`send`, `receive`) |

## 4. Two planes

| | Control plane | Data plane |
|---|---|---|
| Content | Small, typed, versioned messages | Raw bulk data |
| Path | wireforge → pairring → pushseg | pushseg directly |
| Examples | Version query, heartbeat, phase start/done, progress, result summary | Input distribution, halo exchange, simulation-to-analysis transfer, result collection |

Bulk data does not go through the message layer: no framing, no copy into a
ring, no encoding. It is written where the receiver will read it.

## 5. Programming model

- **Workers.** A worker is a thread. A target (process) runs one or more
  workers. An algorithm sees `rank()` and `size()` over all workers, not the
  targets.
- **SPMD.** Every worker runs the same function and branches on its rank or
  group.
- **Groups.** Fixed subsets of workers, defined in code in version 1 (for
  example the simulation group, the analysis group, pairs inside the analysis
  group).
- **Phases.** The algorithm proceeds in phases separated by barriers.
  Workers wait for each other at the barrier; work is not moved between
  workers.
- **Edges.** A one-way link from one worker to another. Between workers of
  one process it is a memory copy and an atomic flag; between processes it is
  a pushseg segment. The algorithm does not see the difference.

## 6. Data distribution primitives

Built on pushseg's rule: write to the remote side, read local memory.

**Symmetric memory.** Every process has one data segment, and every rank an
area of the same size in the segment of its process. Buffers are allocated in
every process in the same order, so a buffer has the same offset in every
rank's area: a rank addresses another rank's copy of a buffer without asking
(`include/gridlane/fabric.hpp`). This is the symmetric heap of PGAS systems
such as OpenSHMEM.

**Collectives** (`include/gridlane/collective.hpp`) are an arrival (each member
writes to the group's root) followed by a release (the root writes to each
member). A member returns only after the root has used what it sent, so
buffers are reused at once without further flow control, and the root combines
values in member order, so a reduction does not depend on arrival order.

| Primitive | Realization |
|---|---|
| put + signal | Write into the peer's buffer, then `store` a sequence number |
| wait | Wait until a local flag reaches a sequence number |
| barrier (group) | Each member raises its flag at the root; the root releases every member |
| scatter | The root writes each member's part into that member's buffer |
| gather | Each member writes its part into its own slot at the root |
| broadcast | The root writes the same data to every member |
| allreduce (small groups) | Each member writes its value to every member; each reduces locally |
| neighbor exchange | put + signal with two buffers, by step parity |

## 7. Control plane messages

Defined in `model/project.yaml` (wireforge); the generated code is committed
under `generated/` and marked as generated.

| Message | Direction | Purpose |
|---|---|---|
| `VersionQuery` / `VersionReply` | coordinator → worker / back | Software and protocol version, capabilities (threads) |
| `Heartbeat` / `HeartbeatAck` | coordinator → worker / back | Periodic liveness query; sequence number and timestamp |
| `PhaseStart` / `PhaseDone` | coordinator → worker / back | Phase control, per-phase timing |
| `Progress` | worker → coordinator | Step count during the simulation |
| `ResultSummary` | worker → coordinator | Checksums and timing of the worker's part |
| `Shutdown` | coordinator → worker | End of the run |

Version 1 has no error handling: a worker answers heartbeats, and that is all.

## 8. Demonstrator: Gray-Scott in situ pipeline

Gray-Scott reaction-diffusion is the common test application of in situ
analysis research (it ships as an example with ADIOS2). Here it is a vehicle
for the model, not a goal.

- 2D grid, periodic boundaries, two fields U and V, parameters F, k, Du, Dv, dt.
- Deterministic initial condition: a fixed pattern plus noise from a
  specified generator (splitmix64), so every implementation starts from the
  same field.
- Workers split into a simulation group and a smaller analysis group (M × N).

| Phase | Work | Pattern |
|---|---|---|
| 0 Startup | Version query, capabilities | Control plane |
| 1 Distribution | Initial fields to the simulation group | Scatter |
| 2 Simulation | Gray-Scott steps | Neighbor exchange |
| 3 Global measures | Every K steps: ΣU, ΣV | Allreduce, barrier |
| 4 In transit | Every K steps: snapshot from the simulation group to the analysis group | M × N redistribution |
| 5 Analysis | Histogram (PDF) of V; combined in pairs, then pairs of pairs | Subgroups, reduction tree |
| 6 Collection | Fields and results at the coordinator | Gather |
| Throughout | Heartbeats, progress | Control plane |

The exact computation, the split of the work and the tolerances are in
[`gray-scott.md`](gray-scott.md).

### Implementation

| Part | Where |
|---|---|
| Kernel, initial condition, measures, serial run | `apps/gray_scott/kernel.*` |
| The pipeline on the data plane (phases 1 to 6) | `apps/gray_scott/pipeline.*` |
| One control connection: pushseg inbox, pairring channel, wireforge link | `apps/gray_scott/control_channel.hpp` |
| Worker application: control loop, worker threads | `apps/gray_scott/worker_main.cpp` |
| Coordinator application | `apps/gray_scott/coordinator_main.cpp` |
| Python reference, comparison, image | `python/gray_scott_ref.py`, `python/compare.py`, `python/render.py` |
| Launch on one machine; the portability demo | `scripts/run_local.sh`, `scripts/portability_demo.sh` |

Workers report to their application's main thread, which alone sends on the
control channel (a pairring channel has one sending thread).

A wireforge link without framing receives one message per pairring message.
Its receive buffer must be larger than the largest message:
`glue::MessageLink` sizes it so (`include/gridlane/glue/pairring_transport.hpp`).

## 9. Scenarios

Terms: a **target** is a machine, an **application** is a process on a target,
a **worker** is a thread of an application.

| Scenario | Targets × applications × workers |
|---|---|
| S1 | 1 × 1 × 1 |
| S2 | 1 × 1 × N |
| S3 | M × 1 × 1 |
| S4 | M × 1 × K |
| S5 | N × 1 × (1, 2, ..., N) |

Uneven splits (grid sizes not divisible by the worker count) are included.

### Portability: the same run, two deployments

A deployed system may have N targets talking over TCP. The same run can be
reproduced on one machine with N applications talking over shared memory,
with the same binaries and only another configuration file:

| | Deployment A | Deployment B |
|---|---|---|
| Layout | 2 targets × 1 application × 4 workers | 1 target × 2 applications × 4 workers |
| Workers | 8 | 8 |
| Transport between applications | TCP | POSIX shared memory |
| Result | the same, within tolerance | |

Every scenario above runs in both forms: targets as processes over TCP, and
as processes over POSIX shared memory.

## 10. Verification

Transport and arithmetic are verified separately.

- **Transport, exact.** Moving data must not change a byte.
  - Halo identity test: every cell holds its global index; after an exchange
    every halo cell must hold its neighbor's index.
  - Checksums of every part sent by scatter, the in transit transfer and
    gather equal those received.
- **Arithmetic, with tolerance.** A Python (numpy) implementation of the same
  specification is the reference.
  - Initial fields: equal.
  - Fields: `|x - ref| <= atol + rtol * |ref|`; the largest error and its
    position are reported.
  - Histograms: L1 distance below a bound (a value close to a bin edge may
    fall into the neighboring bin).
  - Global measures: relative error.
- **Independence of the split.** Every scenario agrees with S1 within the
  same tolerances.
- **Physical sanity.** U and V stay within [0, 1]; images of V show the
  pattern expected for the chosen parameters.
- **Control plane.** Message counts, heartbeat period within a tolerance, a
  version mismatch rejected.

**Outputs:** field dumps (raw float64 and JSON metadata), histograms and global
measures (JSON), a run report (scenario, configuration, dependency versions,
machine, timings, control plane statistics, verdict). Images and plots are
made by Python from the dumps.

## 11. Success criteria

Functional (pass/fail):

1. The pipeline runs in every scenario over TCP and POSIX shared memory.
2. Results agree with the Python reference within tolerance.
3. Results agree across scenarios within tolerance.
4. Version query, heartbeats and phase messages work.
5. Clean under AddressSanitizer and ThreadSanitizer; CI green.

The model (pass/fail):

6. **Independence:** each layer builds and tests on its own and references no other layer.
7. **Replaceability:** the same binaries run over TCP and POSIX shared memory, changing only the configuration, and give the same results (section 9).
8. **Extensibility:** for each layer, a guide and a small working example of adding a new API.

Recorded, not pass/fail: time per step, halo exchange latency, in transit
transfer latency, control round trip, scaling with threads and targets,
TCP against shared memory. Reproducible with one command.

## 12. Not in version 1

Error handling and fault tolerance, dynamic load balancing, groups defined by
configuration, security, external memory APIs, 3D, comparison with other
systems.

## 13. Dependencies

Fetched with CMake `FetchContent`, pinned: pushseg and pairring to commits
until they are tagged, the wireforge runtime headers to `v0.5.0`. The
wireforge command line is needed only to regenerate code after changing the
model; the build does not need Python.
