# micrOSAL Bus + Signal Layer — Overview

The bus layer adds typed, fixed-capacity messaging abstractions on top of the
existing MicrOSAL primitives.

| API | Class template | Current status |
| --- | --- | --- |
| Point-to-point | `osal::osal_bus<T, Capacity, BackendTag>` | Implemented on the generic backend and all delegated backends |
| LCD pub/sub | `osal::osal_signal<T, MaxSubscribers, PerSubCapacity, BackendTag>` | Implemented on the generic backend and all delegated backends |
| Premium wrapper | `osal::osal_signal_premium<T, MaxSubscribers, PerSubCapacity, BackendTag>` | Implemented with portable observer/copy-fallback logic; native routing remains a stub |

## Current Backend Status

| Backend tag | Current runtime status |
| --- | --- |
| `bus_backend_generic` | Canonical implementation; uses the MicrOSAL queue C ABI plus `osal::mutex` |
| Delegated `bus_backend_*` tags | Fully implemented by delegating to `bus_backend_generic` |
| `bus_backend_mock` | Test-only backend used by hosted premium tests |
| `bus_backend_zephyr` | Dedicated Zephyr tag that currently delegates to `bus_backend_generic`; native Zbus specialisation not implemented yet |

On Zephyr today, the default backend tag behaves like `bus_backend_generic`.
That keeps the runtime usable now while preserving the public tag for a future
native Zbus implementation.

## Design Constraints

- C++20, bounded storage, no hidden global state.
- Deterministic `O(MaxSubscribers)` publish cost for the LCD implementation.
- Traits-based, compile-time backend selection.
- Generic behavior builds on the existing MicrOSAL queue and mutex primitives.

## File Layout

```text
include/microsal/
  microsal_config.hpp
  bus/
    osal_bus.hpp
    osal_signal.hpp
    osal_signal_premium.hpp
    detail/
      osal_bus_fwd.hpp
      osal_signal_traits.hpp
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

`MICROSAL_DEFAULT_BACKEND_TAG` is selected in `include/microsal/microsal_config.hpp`
from the active `OSAL_BACKEND_*` macro. Every supported OSAL backend has a
matching `bus_backend_*` tag, and if no OSAL backend macro is present the bus
layer defaults to `bus_backend_generic`.

See [Backend Reference](backends.md) for the full mapping table and status notes.

## Quick Start

```cpp
#include <microsal/bus/osal_signal.hpp>

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
