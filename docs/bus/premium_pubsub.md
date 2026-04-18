# Premium Pub/Sub — `osal_signal_premium`

`osal::osal_signal_premium` extends `osal_signal` with observer callbacks,
zero-copy publish, and routing stubs.  All methods are always compiled;
they fall back to LCD-equivalent behaviour on non-premium backends.

## Premium Capabilities per Backend

| Backend                    | `native_pubsub` | `native_observers` | `zero_copy` | `native_routing` |
|----------------------------|-----------------|--------------------|-------------|------------------|
| `bus_backend_generic`  | ✗               | ✗                  | ✗           | ✗                |
| `bus_backend_zephyr`   | ✓               | ✓                  | ✓           | ✓                |
| `bus_backend_mock`     | ✓               | ✓                  | ✓           | ✓                |

Use `osal::osal_signal_capabilities<BackendTag>` to query flags at compile time.

## Additional API

### Observer Subscription

```cpp
using observer_fn = void (*)(const T&) noexcept;

bool subscribe_observer(observer_fn fn);    // register callback
bool unsubscribe_observer(observer_fn fn);  // deregister callback
std::size_t observer_count() const;         // number of registered observers
```

Observers are invoked synchronously on publish (generic/mock backends).
On Zephyr they are invoked by the Zbus listener thread.

### Zero-Copy Publish

```cpp
bool publish_zero_copy(T* ptr);
```

On capable backends, avoids copying the message into each subscriber queue.
Falls back to `publish(*ptr)` on generic backends.

### Routing (stub)

```cpp
bool route_to(signal_id dest, const T& msg);
```

Routes a message to another topic identified by `signal_id`.
**Currently a stub — returns `false`** until a topic registry is implemented.

## Example

```cpp
#include <microsal/bus/osal_signal_premium.hpp>

osal::osal_signal_premium<std::uint32_t, 4, 8, osal::bus_backend_mock> topic;

// Observer registration
topic.subscribe_observer([](const std::uint32_t& v) noexcept {
    log("received: %u", v);
});

// Queue-based subscriber
osal::subscriber_id sub{osal::invalid_subscriber_id};
topic.subscribe(sub);

// Publish — both queue-subscribers and observers are notified
topic.publish(42U);

// Zero-copy publish
std::uint32_t msg = 99U;
topic.publish_zero_copy(&msg);
```

## Capability Detection

```cpp
if constexpr (osal::native_observer_backend<BackendTag>)
{
    // use topic.subscribe_observer(fn)
}

if constexpr (osal::zero_copy_backend<BackendTag>)
{
    // use topic.publish_zero_copy(&msg)
}
```

## See Also

- [LCD Pub/Sub](lcd_pubsub.md)
- [Backend Reference](backends.md)
