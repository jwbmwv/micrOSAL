# micrOSAL Bus + Signal Layer — Overview

The bus layer adds typed, fixed-capacity messaging abstractions on top of the
existing MicrOSAL primitives.

| API | Class template | Current status |
| --- | --- | --- |
| Point-to-point | `osal::osal_bus<T, Capacity, BackendTag>` | Implemented on the generic backend and all delegated backends |
| LCD pub/sub | `osal::osal_signal<T, MaxSubscribers, PerSubCapacity, BackendTag>` | Implemented on the generic backend and all delegated backends |
| Premium wrapper | `osal::osal_signal_premium<T, MaxSubscribers, PerSubCapacity, BackendTag>` | Implemented; current Zephyr and mock backends expose backend-owned observer hooks, zero-copy still falls back to copy, and native routing remains a stub |

## Current Backend Status

| Backend tag | Current runtime status |
| --- | --- |
| `bus_backend_generic` | Canonical implementation; uses the MicrOSAL queue C ABI plus `osal::mutex` |
| Delegated `bus_backend_*` tags | Fully implemented by delegating to `bus_backend_generic` |
| `bus_backend_mock` | Test-only backend used by hosted premium tests |
| `bus_backend_zephyr` | Dedicated Zephyr runtime uses native `k_msgq` queues on real Zephyr builds; hosted instantiations still delegate to `bus_backend_generic` |

On Zephyr, the default backend tag now selects that dedicated queue-backed
runtime. Hosted builds that instantiate `bus_backend_zephyr` for compile-time
coverage still reuse the generic implementation.

## Design Constraints

- C++20, bounded storage, no hidden global state.
- Deterministic `O(MaxSubscribers)` publish cost for the LCD implementation.
- Traits-based, compile-time backend selection.
- Generic behavior builds on the existing MicrOSAL queue and mutex primitives.

## File Layout

```text
include/
  microsal/
    microsal_config.hpp
    bus/
      ... compatibility wrappers ...
  osal/
    bus/
      osal_bus.hpp
      osal_signal.hpp
      osal_signal_premium.hpp
      detail/
        osal_bus_fwd.hpp
        osal_signal_traits.hpp
        osal_bus_backend_generic.hpp
        osal_bus_backend_delegated.hpp
        osal_bus_backend_mock.hpp
        osal_bus_backend_zephyr.hpp
        osal_signal_backend_generic.hpp
        osal_signal_backend_delegated.hpp
        osal_signal_backend_mock.hpp
        osal_signal_backend_zephyr.hpp

tests/bus/
  test_osal_signal_lcd.cpp
  test_osal_signal_premium.cpp
  test_osal_signal_backends.cpp
```

## Backend Selection

`MICROSAL_DEFAULT_BACKEND_TAG` is selected in `include/osal/bus/config.hpp`
from the active `OSAL_BACKEND_*` macro. Every supported OSAL backend has a
matching `bus_backend_*` tag, and if no OSAL backend macro is present the bus
layer defaults to `bus_backend_generic`. Public bus headers are now canonical
under `include/osal/bus/`; `include/microsal/bus/` remains as a compatibility
shim for older include paths.

See [Backend Reference](backends.md) for the full mapping table and status notes.

## Quick Start

```cpp
#include <osal/bus/osal_signal.hpp>

osal::osal_signal<std::uint32_t, 4, 8, osal::bus_backend_generic> topic;

osal::subscriber_id sub{osal::invalid_subscriber_id};
topic.subscribe(sub);
topic.publish(42U);

std::uint32_t value{};
topic.try_receive(sub, value);

topic.unsubscribe(sub);
```

## Diagrams

- [Architecture](../diagrams/bus_architecture.puml)
- [Class Diagram](../diagrams/bus_class_diagram.puml)
- [Publish Sequence](../diagrams/bus_sequence_publish.puml)

## Further Reading

- [LCD Pub/Sub](lcd_pubsub.md)
- [Premium Pub/Sub](premium_pubsub.md)
- [Backend Reference](backends.md)
