# MicrOSAL — PlantUML Diagrams

Visual documentation for the MicrOSAL architecture. Render with any
PlantUML-compatible tool (VS Code extension, IntelliJ, `plantuml` CLI,
or the [online server](https://www.plantuml.com/plantuml/uml)).

## Diagram Index

| File | Type | Description |
|------|------|-------------|
| [architecture.puml](architecture.puml) | Component | Layered architecture — app → header API → extern "C" bridge → backends → OS kernel |
| [class_diagram.puml](class_diagram.puml) | Class | All OSAL primitive classes, core types, compile-time traits, and their relationships |
| [backend_map.puml](backend_map.puml) | Component | 17 backends grouped by family (POSIX, commercial RTOS, specialty, ARM CMSIS, bare-metal) with capability highlights |
| [capability_matrix.puml](capability_matrix.puml) | Map | Per-backend feature support matrix (25 capability flags × 17 backends) |
| [emulation_strategy.puml](emulation_strategy.puml) | Component | Native vs emulated breakdown for condvar, work_queue, memory_pool, and rwlock, showing which backends use shared .inl emulations |
| [build_flow.puml](build_flow.puml) | Activity | CMake build pipeline — backend selection → compile → link → test |
| [sequence_mutex.puml](sequence_mutex.puml) | Sequence | Mutex full lifecycle: construct → lock → unlock → destroy, showing all four layers |
| [sequence_condvar.puml](sequence_condvar.puml) | Sequence | Condition variable wait/notify pattern between producer and consumer threads |
| [sequence_work_queue.puml](sequence_work_queue.puml) | Sequence | Work queue submit/execute/flush flow with internal worker thread |
| [sequence_thread_lifecycle.puml](sequence_thread_lifecycle.puml) | Sequence | Thread create → run → join lifecycle through all abstraction layers |
| [sequence_memory_pool.puml](sequence_memory_pool.puml) | Sequence | Memory pool allocate/deallocate lifecycle with pool exhaustion and timed allocation |
| [sequence_rwlock.puml](sequence_rwlock.puml) | Sequence | Read-write lock concurrent readers, exclusive writer, and RAII guards |
| [sequence_ring_buffer.puml](sequence_ring_buffer.puml) | Sequence | Lock-free SPSC ring buffer producer/consumer flow (header-only, no OS) |
| [memory_layout.puml](memory_layout.puml) | Component | Config / data split — FLASH (.rodata) config structs vs RAM runtime handles |

`emulation_strategy.puml`, `capability_matrix.puml`, `class_diagram.puml`, and `architecture.puml` are tuned for presentation readability using landscape orientation and width scaling.
`newpage` page breaks were intentionally avoided because the current PlantUML CLI renderer in this environment fails during PNG/SVG generation on these large diagrams when multi-page splits are enabled.

## Quick render (CLI)

```bash
# Render PNG, SVG, and PDF (when an SVG->PDF backend is available)
./scripts/generate_diagrams.sh

# Render print-oriented PNG, SVG, and PDF into docs/diagrams/print
./scripts/generate_diagrams_print.sh
```
