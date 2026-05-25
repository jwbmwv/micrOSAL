# LCD Pub/Sub — `osal::osal_signal`

`osal::osal_signal<T, MaxSubscribers, PerSubCapacity, BackendTag>` is the
portable fixed-capacity pub/sub topic in the bus layer.

The implemented LCD behavior is provided by `bus_backend_generic`, every
delegated `bus_backend_*` tag, and the dedicated Zephyr runtime. On real
Zephyr builds, `bus_backend_zephyr` uses native `k_msgq` queues while
preserving the same per-subscriber FIFO contract as the generic backend.

## Guarantees

| Property | Value |
| --- | --- |
| Dynamic allocation | None in the implemented generic and delegated backends |
| Publish complexity | `O(MaxSubscribers)` non-blocking sends |
| Subscriber model | Per-subscriber FIFO queue (pull model) |
| Thread safety | `subscribe()`, `unsubscribe()`, and `publish()` are mutex-protected |
| Slot reuse | Reused subscriber slots are purged before reassignment |
| Full subscriber queue | That subscriber silently drops the publish; the overall call returns `true` if any active subscriber accepted the message |

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
| --- | --- |
| `bool subscribe(subscriber_id& out_id)` | Allocate a subscriber slot and write its ID to `out_id` |
| `bool unsubscribe(subscriber_id id)` | Release a subscriber slot |
| `bool publish(const T& msg)` | Fan out to all active subscriber queues using non-blocking sends |
| `bool try_receive(subscriber_id id, T& out)` | Non-blocking dequeue from one subscriber queue |
| `bool receive(subscriber_id id, T& out, tick_t timeout = WAIT_FOREVER)` | Blocking dequeue with an OSAL tick timeout |
| `std::size_t subscriber_count() const` | Number of active subscribers |
| `static constexpr std::size_t max_subscribers()` | Compile-time subscriber bound |
| `static constexpr std::size_t per_subscriber_capacity()` | Compile-time queue depth per subscriber |

## Example

```cpp
#include <osal/bus/osal_signal.hpp>

using SensorTopic = osal::osal_signal<SensorData, 4, 16, osal::bus_backend_generic>;

SensorTopic sensor_topic;

void producer_task()
{
    sensor_topic.publish(SensorData{42});
}

void consumer_task()
{
    osal::subscriber_id id{osal::invalid_subscriber_id};
    sensor_topic.subscribe(id);

    SensorData data{};
    while (sensor_topic.receive(id, data, osal::WAIT_FOREVER))
    {
        process(data);
    }

    sensor_topic.unsubscribe(id);
}
```

## Memory Layout

For `osal_signal<T, N, M, bus_backend_generic>`:

```text
osal_signal {
    subscriber_slot slots_[N] {
        bool in_use_
        osal_bus<T, M, bus_backend_generic> queue_
    }
    osal::mutex mutex_
    std::size_t subscriber_count_
}
```

Each `osal_bus<T, M, bus_backend_generic>` owns its queue handle and backing
storage inline, so the topic object's footprint scales with `N` and `M`.

## See Also

- [Premium Pub/Sub](premium_pubsub.md)
- [Bus Overview](bus_overview.md)
