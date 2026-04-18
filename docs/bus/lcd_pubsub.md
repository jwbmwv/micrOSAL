# LCD Pub/Sub — `osal_signal`

`osal::osal_signal<T, MaxSubscribers, PerSubCapacity, BackendTag>` provides
**Lowest Common Denominator** pub/sub that works on any micrOSAL backend.

## Guarantees

| Property                | Value                                          |
|-------------------------|------------------------------------------------|
| Dynamic allocation      | **None** — all storage is compile-time static. |
| Publish complexity      | O(MaxSubscribers) try_send calls.              |
| Subscriber model        | Per-subscriber FIFO queue (pull model).        |
| Thread safety           | subscribe/unsubscribe/publish mutex-protected. |
| Missed messages         | Full subscriber queue silently drops publish.  |

## API Reference

```cpp
template<typename T,
         std::size_t MaxSubscribers,
         std::size_t PerSubCapacity,
         typename BackendTag = MICROSAL_DEFAULT_BACKEND_TAG>
class osal_signal;
```

### Methods

| Signature | Description |
|-----------|-------------|
| `bool subscribe(subscriber_id& out_id)` | Allocates a slot; writes ID to `out_id`. Returns `false` when at capacity. |
| `bool unsubscribe(subscriber_id id)` | Releases the slot. Returns `false` if ID is invalid or not active. |
| `bool publish(const T& msg)` | Fans out to all active subscriber queues (try_send). Returns `true` if at least one queue accepted the message. |
| `bool try_receive(subscriber_id id, T& out)` | Non-blocking dequeue. Returns `true` if a message was available. |
| `bool receive(subscriber_id id, T& out, tick_t timeout = WAIT_FOREVER)` | Blocking dequeue. Uses `osal::WAIT_FOREVER` or `osal::NO_WAIT` as timeout. |
| `std::size_t subscriber_count() const` | Current active subscriber count. |
| `static constexpr std::size_t max_subscribers()` | Compile-time maximum. |
| `static constexpr std::size_t per_subscriber_capacity()` | Compile-time queue depth. |

## Example

```cpp
#include <microsal/bus/osal_signal.hpp>

osal::osal_signal<SensorData, 4, 16> sensor_topic;

// Producer thread
void producer_task()
{
    sensor_topic.publish(SensorData{42});
}

// Consumer A thread
void consumer_a()
{
    osal::subscriber_id id{osal::invalid_subscriber_id};
    sensor_topic.subscribe(id);

    SensorData data{};
    while (sensor_topic.receive(id, data))  // blocks until data arrives
    {
        process(data);
    }
    sensor_topic.unsubscribe(id);
}
```

## Memory Layout

For `osal_signal<T, N, M, bus_backend_generic>`:

```
osal_signal {
    subscribers[N] {
        bool in_use
        osal_bus<T, M> {      // ~= osal::queue
            queue_handle_t handle
            uint8_t storage[M * sizeof(T)]
        }
    }
    mutex (osal::mutex)
    size_t subscriber_count
}
```

Total static footprint ≈ N × (sizeof(osal::active_traits::queue_handle_t) + M × sizeof(T)) + sizeof(osal::mutex).

## See Also

- [Premium Pub/Sub](premium_pubsub.md)
- [Channel Overview](channel_overview.md)
