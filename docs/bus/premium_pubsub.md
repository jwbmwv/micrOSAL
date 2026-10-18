# Premium Pub/Sub — owned loans and typed routing

`osal::osal_signal_premium` preserves the copying `osal_signal` API, adds
synchronous observers, and dispatches to registered typed topics.
`osal::zero_copy_signal` provides a separate, genuinely zero-payload-copy
ownership transport. Both use existing OSAL primitives, without new backend ABI,
virtual dispatch, RTTI, or helper-managed heap allocation.

## Current Runtime Status

| Transport | Ownership / capabilities |
| --- | --- |
| `osal_signal` and `osal_signal_premium` | Copy each payload into subscriber queues. `publish_zero_copy(T*)` is a compatibility **copying** overload, not an ownership transfer. |
| `zero_copy_signal<T, M, P, N>` | Portable owned transport. `zero_copy == true`; queues transport pool indices, never payloads. |
| `topic_registry<T, N>` | Portable routing of copying `publish(const T&)` topics, not backend-native routing or owned-loan routing. |

All existing backend `zero_copy` and `native_routing` traits are false, including
the mock backend. Mock observer hooks remain supported. Real Zephyr provides
native pub/sub and observer hooks, not a native owned payload path.

## Additional API

### Observer Subscription

```cpp
bool subscribe_observer(void (*fn)(const T&) noexcept);
bool unsubscribe_observer(void (*fn)(const T&) noexcept);
std::size_t observer_count() const;
```

Publishing first fans out to queues, then invokes callbacks synchronously.
Generic observer registration/count access is mutex-protected; publish takes a
bounded snapshot and releases the mutex before callbacks. A callback can remove
itself or modify registration without deadlock. Removal prevents *future*
snapshots, not callbacks already captured by another publisher. Callback code and
any externally referenced state must outlive those in-flight callbacks. Native
backends retain their observer semantics. Callbacks must not throw or block.

### Compatibility `publish_zero_copy(T*)`

The premium raw-pointer overload checks for null and calls `publish(*ptr)`.
It cannot safely acquire ownership of arbitrary caller memory. The pointer is
borrowed only for the synchronous call; no payload lifetime extension occurs.
Existing code remains source-compatible.

### Real zero-copy ownership

```cpp
#include <osal/bus/zero_copy_signal.hpp>

struct measurement { int value{}; };
using topic_type = osal::zero_copy_signal<measurement, 4, 8, 16>;

topic_type::pool_type pool;       // caller-owned bounded storage
topic_type topic{pool};          // must be initialized after OSAL/RTOS startup
osal::subscriber_id id{};
if (topic.subscribe(id)) {
    auto producer = topic.try_loan(); // or pool.try_loan()
    if (producer) {
        producer->value = 42;     // writable, move-only producer
        auto result = topic.publish_zero_copy(std::move(producer));
        // result.accepted / dropped describe per-subscriber fanout.
        // result.status distinguishes delivered, partial, full,
        // no_subscribers, and invalid_loan.
    }
    topic_type::read_loan received;
    if (topic.receive_loan(id, received, osal::NO_WAIT)) {
        consume(*received);      // only const payload access
        received.release();      // also released automatically on destruction
    }
    (void)topic.unsubscribe(id);
}
```

`message_pool<T, Capacity>` holds default-constructed `T` objects and reference
counts in caller-owned storage. `T` must be nothrow default constructible and
nothrow destructible; it need not be copyable, movable, trivially copyable, or
standard layout. Reused objects retain their previous values; producers initialize
the fields they need. No payload construction/copy/move occurs during fanout or
receive. Only pool indices cross the backend queues.

* A producer loan owns one pool reference and is move-only. Empty/exhausted pool
  acquisition returns an empty loan. Its destructor or `release()` returns it.
* Successful publish to **at least one** subscriber consumes the producer loan.
  Subscriber handles expose only `const T*`/`const T&`. Do not retain or use mutable
  aliases obtained from the producer after publication. Like other C++ ownership
  APIs, previously extracted raw pointers cannot be mechanically revoked.
* Zero acceptance (no subscribers, all full, invalid/foreign loan, initialization
  failure) does **not** consume a valid producer loan. A partial fanout consumes
  it and explicitly reports dropped subscribers. Publish does not wait for space.
* Each queued reference and each read handle retains the payload. Copying a read
  handle increments the reference count, not the payload copy count. Move is
  constant-time. Release/reset/destruction decrements it; pool reuse occurs only
  after the final producer/queue/read reference is released.
* Unsubscribe drains queued references. Already received read handles remain
  usable even after unsubscribe or topic destruction. Topic destruction also
  drains queued references. Destruction must not race with any topic operation.
* A blocking receive reserves the subscriber slot before releasing the topic
  mutex. Unsubscribe fails while that receive is in progress. `receive_loan`
  defaults to `NO_WAIT` and accepts raw OSAL ticks, including `WAIT_FOREVER`.
  On failure it leaves the output handle unchanged.
* Subscriber tokens are monotonic and never recycled within a topic lifetime;
  registration fails rather than wrapping to a stale token.

**Lifetime:** the pool must outlive every associated topic, producer loan, and
read handle, including copies of handles retained after unsubscribe. Destructing
a pool with outstanding references violates the contract (asserted in debug
builds). The topic owns no pool memory. All these APIs use mutexes and are
**task-context only**, not ISR-safe. No claim is made that a hosted backend's
primitive control blocks avoid its normal host-managed allocations.

**Metadata synchronization:** each pool, owned topic, and registry uses an
existing `osal::mutex`, acquired through `std::lock_guard`. Initialization is
checked through `valid()` before public operations; failure returns an empty
loan, false, or the API's invalid status without acquiring the uninitialized
mutex. Acquisition uses the backend's blocking `WAIT_FOREVER` mutex operation,
not an `atomic_flag` spin/yield loop. Priority inheritance and scheduling
guarantees remain those of the selected backend's default mutex. Nonblocking
loan/queue attempts do not wait for capacity, but may wait to acquire metadata
mutexes. Loan copy/release likewise may wait for the pool mutex.

Owned-topic operations acquire the topic mutex before the pool mutex, never in
the reverse order. A blocking receive releases the topic mutex while waiting
on its queue. Registry dispatch and generic observer callbacks run without
their registry/observer mutex held; routing also releases its binding mutex
before dispatch. Diagnostic atomics update/sample counters only: they are not
ownership locks protecting payload, subscription, or registry metadata.

Local `pool.snapshot()` reports current occupied slots, high-water slots, and
exhaustion count. `topic.snapshot()` reports drops, backlog, high-water backlog,
and outstanding receive reservations. Backlog includes dequeues whose receive
reservation has not yet completed, so concurrent snapshots conservatively include
in-progress delivery.

Both objects also expose `diagnostics() -> osal::diagnostic_snapshot`, the common
resource/operation schema. Pool capacity/in-use/high-water count payload slots;
pool attempts/successes/failures count producer-loan acquisition calls, including
real exhaustion failures. Topic capacity counts all subscriber queue slots;
in-use/backlog/high-water count queued or in-progress deliveries. Topic
attempts/successes/failures count publication calls: partial fanout is a success,
zero acceptance is a failure, and drops count individual rejected subscriber
deliveries. The counters wrap modulo 2^32. Capacity conversions are checked at
compile time, and sampled gauges saturate at the 32-bit limit. The snapshots are
per-instance, not global bus diagnostics; contention is zero because lock
contention is not measured. Counters and mutex-protected gauges are sampled
separately, so the common snapshot is approximate during concurrent operations.

Header discovery: `<osal/bus/osal_signal_premium.hpp>` includes both
`topic_registry.hpp` and `zero_copy_signal.hpp`. They may also be included
directly. `<osal/bus/osal_bus.hpp>` remains the channel-only umbrella and
`<osal/bus/osal_signal.hpp>` remains the copying-topic umbrella; neither pulls in
premium features. The core `<osal/osal.hpp>` does not include the bus API.

### `route_to()` and typed registry

```cpp
#include <osal/bus/osal_signal_premium.hpp>

using topic_type = osal::osal_signal_premium<int, 4, 8>;
osal::topic_registry<int, 8> registry;
topic_type source, destination;
osal::signal_id destination_id{};
if (registry.register_topic(destination, destination_id)) {
    (void)source.bind_registry(registry);
    bool accepted = source.route_to(destination_id, 7);
    auto status = source.route_to(registry, destination_id, 8);
    // status: delivered, no_subscribers, not_found, busy, or invalid
    (void)accepted;
    (void)status;
    (void)source.unbind_registry();
    auto removed = registry.unregister_topic(destination_id);
    (void)removed;
}
```

`register_topic(topic, out_id)` accepts only a topic with matching `value_type`
and nothrow `bool publish(const T&)`. Payload type mismatches fail at compile time.
Registration is allocation-free with `MaxTopics` entries; full/duplicate
registration fails and writes `invalid_signal_id`. IDs are bounded 32-bit
monotonic tokens, never recycled in a registry lifetime; exhaustion fails rather
than wrapping. IDs belong to one registry instance, not a global namespace.

`registry.dispatch(id, value)` and the explicit-registry `route_to` overload return
`route_status`. The original `route_to(id, value)` uses `bind_registry` and returns
true only for `delivered`; unbound, busy, stale/missing, and no-subscriber routes
return false. Routing really invokes the destination's `publish`, including its
observer callbacks. It does not establish a permanent forwarding subscription.

**Concurrency and cycles:** a dispatch pins its entry under the registry mutex,
releases the mutex before publishing/calling observers, then unpins the entry.
Concurrent dispatch to an already active target returns `busy` (does not wait).
Unregister of an active target also returns `busy`; retry after dispatch ends.
Reentrant cycles, including multi-topic/multi-registry cycles, eventually revisit
an active entry and fail with `busy` instead of recursing indefinitely. Within
one registry recursive distinct-target dispatch depth is bounded by `MaxTopics`.
Delivery before a cycle is discovered is not rolled back.

**Borrowed lifetimes:** unregister must succeed before destroying a registered
topic, and registry destruction must not overlap registry operations. A bound
source borrows its registry until `unbind_registry()` succeeds; unbind/rebind
fails during source routing, so the caller can safely retry after in-flight routes
finish. Explicit-registry routes borrow the registry for their synchronous call.
Neither registry nor binding owns topic memory or automatically manages shutdown.
Do not destroy either while callbacks are in flight.

## Capability Detection

`osal_signal_capabilities<BackendTag>` and its concepts describe backend-native
capability, not API presence. Portable typed routing works even when
`native_routing == false`; use `zero_copy_signal::zero_copy` to identify the
owned transport, not `zero_copy_backend<BackendTag>` for raw-pointer publishing.

## See Also

- [LCD Pub/Sub](lcd_pubsub.md)
- [Backend Reference](backends.md)
