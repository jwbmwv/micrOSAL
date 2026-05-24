# Premium Pub/Sub — `osal::osal_signal_premium`

`osal::osal_signal_premium` extends `osal_signal` with observer callbacks,
copy-avoidance hooks, and a routing API surface.

The methods are always present. The backend traits tell you whether the backend
has a native implementation behind those methods or whether the premium wrapper
is using its portable fallback behavior.

## Current Runtime Status

| Backend family | Trait flags | Current behavior |
| --- | --- | --- |
| `bus_backend_generic` and delegated backends | `native_* == false`, `zero_copy == false` | Local observer table, synchronous observer callbacks, `publish_zero_copy()` falls back to `publish(*ptr)`, `route_to()` returns `false` |
| `bus_backend_mock` | All premium flags `true` | Hosted test backend; uses the same portable observer/copy fallback behavior while exercising the premium API surface |
| `bus_backend_zephyr` | All premium flags `true` | Intended Zbus-native path, but the current runtime specialisations are still TODO stubs |

Use `osal::osal_signal_capabilities<BackendTag>` and the `native_*` concepts in
`osal_signal_traits.hpp` when you need to branch on native backend support.

## Additional API

### Observer Subscription

```cpp
using observer_fn = void (*)(const T&) noexcept;

bool subscribe_observer(observer_fn fn);
bool unsubscribe_observer(observer_fn fn);
std::size_t observer_count() const;
```

On the currently implemented backends, observers are invoked synchronously
during `publish()`. The intended Zephyr-native model is a Zbus listener thread,
but that runtime path is not wired up yet.

### `publish_zero_copy()`

```cpp
bool publish_zero_copy(T* ptr);
```

Today, every implemented backend copies from `ptr` and then uses `publish()`.
The API is present so a future native backend can replace that fallback with a
true zero-copy or near-zero-copy path.

### `route_to()`

```cpp
bool route_to(signal_id dest, const T& msg);
```

`route_to()` is currently a stub on every backend and returns `false` until a
topic registry or native routing integration is added.

## Example

```cpp
#include <microsal/bus/osal_signal_premium.hpp>

osal::osal_signal_premium<std::uint32_t, 4, 8, osal::bus_backend_mock> topic;

topic.subscribe_observer([](const std::uint32_t& value) noexcept {
    log("received: %u", value);
});

osal::subscriber_id sub{osal::invalid_subscriber_id};
topic.subscribe(sub);

topic.publish(42U);

std::uint32_t msg = 99U;
topic.publish_zero_copy(&msg);
```

## Capability Detection

```cpp
if constexpr (osal::native_observer_backend<BackendTag>)
{
    // Backend intends native observer support.
}

if constexpr (osal::zero_copy_backend<BackendTag>)
{
    // Backend intends a native zero-copy path.
}
```

These concepts describe backend-native capability, not mere API presence.

## See Also

- [LCD Pub/Sub](lcd_pubsub.md)
- [Backend Reference](backends.md)
