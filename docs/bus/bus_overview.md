# micrOSAL Channel Layer — Overview

The channel layer adds a portable, deterministic pub/sub abstraction on top of
the existing micrOSAL primitives (queue, mutex).  It consists of two API tiers:

| Tier          | Class                 | Guarantee                               |
|---------------|-----------------------|-----------------------------------------|
| **LCD**       | `osal_signal`          | Works on every micrOSAL backend.        |
| **Premium**   | `osal_signal_premium`  | Unlocks observer, zero-copy, routing.   |

## Design Constraints

- C++20, no dynamic allocation, no hidden global state.
- Deterministic O(MaxSubscribers) publish cost.
- Traits-based, compile-time backend selection.
- Generic fallback builds on `osal::queue` + `osal::mutex`.

## File Layout

```
include/microsal/
  microsal_config.hpp                        Channel backend tags + default selection
  channel/
    osal_bus.hpp                         Typed point-to-point channel
    osal_signal.hpp                           LCD pub/sub topic (primary template)
    osal_signal_premium.hpp                   Premium topic (observer, zero-copy, routing)
    detail/
      osal_signal_traits.hpp                  osal_signal_capabilities<BackendTag>
      osal_signal_backend_generic.hpp         Generic fallback (osal::queue backed)
      osal_signal_backend_zephyr.hpp          Zbus-backed backend (skeleton)
      osal_signal_backend_delegated.hpp       16 backends delegating to generic
      osal_signal_backend_mock.hpp            Mock premium backend for tests
```

## Backend Selection

The default backend is chosen automatically from the active `OSAL_BACKEND_*` macro.
All 17 supported OSAL backends have a corresponding `bus_backend_*` tag:

| OSAL backend macro         | Default channel backend          |
|----------------------------|----------------------------------|
| `OSAL_BACKEND_ZEPHYR`     | `bus_backend_zephyr`         |
| `OSAL_BACKEND_FREERTOS`   | `bus_backend_freertos`       |
| `OSAL_BACKEND_LINUX`      | `bus_backend_linux`          |
| All other backends         | Matching `bus_backend_*` tag |
| (none defined)             | `bus_backend_generic`        |

See [Backend Reference](backends.md) for the complete mapping table.

## Quick Start

```cpp
#include <microsal/bus/osal_signal.hpp>

// Topic: uint32_t messages, ≤4 subscribers, queue depth 8 each.
osal::osal_signal<std::uint32_t, 4, 8> topic;

osal::subscriber_id sub{osal::invalid_subscriber_id};
topic.subscribe(sub);
topic.publish(42U);

std::uint32_t val{};
topic.try_receive(sub, val);  // val == 42

topic.unsubscribe(sub);
```

## Diagrams

- [Architecture](../diagrams/channel_architecture.puml)
- [Class Diagram](../diagrams/channel_class_diagram.puml)
- [Publish Sequence](../diagrams/channel_sequence_publish.puml)

## Further Reading

- [LCD Pub/Sub](lcd_pubsub.md)
- [Premium Pub/Sub](premium_pubsub.md)
- [Backend Reference](backends.md)
