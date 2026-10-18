// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <osal/bus/osal_signal_premium.hpp>
#include <osal/diagnostics.hpp>
#include <osal/mutex.hpp>
#include <osal/object_wait_set.hpp>
#include <osal/queue.hpp>
#include <osal/semaphore.hpp>
#include <osal/stop_token.hpp>
#include <osal/thread.hpp>
#include <osal/wait_signal.hpp>

#include <atomic>
#include <utility>

namespace osal_test
{

struct immovable_payload
{
    int value{};

    immovable_payload() noexcept                           = default;
    immovable_payload(const immovable_payload&)            = delete;
    immovable_payload& operator=(const immovable_payload&) = delete;
    immovable_payload(immovable_payload&&)                 = delete;
    immovable_payload& operator=(immovable_payload&&)      = delete;
};

template<typename Check>
void verify_owned_loans(Check check)
{
    using topic_type = osal::zero_copy_signal<immovable_payload, 2, 1, 2>;
    topic_type::pool_type pool;
    topic_type            topic{pool};
    check(topic.valid(), "owned topic initialization");
    if (!topic.valid())
    {
        return;
    }

    osal::subscriber_id first_id{}, second_id{};
    const bool          first_subscribed  = topic.subscribe(first_id);
    const bool          second_subscribed = topic.subscribe(second_id);
    check(first_subscribed && second_subscribed, "two subscriber registrations");
    if (!first_subscribed || !second_subscribed)
    {
        return;
    }

    auto producer = pool.try_loan();
    check(static_cast<bool>(producer), "producer loan acquisition");
    if (!producer)
    {
        return;
    }
    producer->value       = 42;
    const auto* address   = producer.get();
    const auto  published = topic.publish_zero_copy(std::move(producer));
    check(published.accepted == 2U && published.dropped == 0U, "exact zero-copy fanout");
    check(!producer, "successful publication consumes producer ownership");

    topic_type::read_loan first, second;
    const bool            first_received  = topic.receive_loan(first_id, first);
    const bool            second_received = topic.receive_loan(second_id, second);
    check(first_received && second_received, "both subscribers receive loans");
    if (!first_received || !second_received)
    {
        return;
    }
    check(first.get() == address && second.get() == address, "subscribers share original payload address");
    check(first->value == 42 && second->value == 42, "shared payload remains intact");

    auto spare     = pool.try_loan();
    auto exhausted = pool.try_loan();
    check(static_cast<bool>(spare) && !exhausted, "pool capacity is enforced");
    const auto occupied = pool.diagnostics();
    check(occupied.capacity == 2U && occupied.in_use == 2U && occupied.high_water == 2U,
          "pool diagnostics record actual occupancy and high-water mark");
    check(occupied.failures == 1U, "pool diagnostics record exhaustion");
    spare.release();

    check(topic.unsubscribe(first_id), "first unsubscribe");
    check(topic.unsubscribe(second_id), "second unsubscribe");
    check(first->value == 42 && second->value == 42, "read loans survive unsubscribe");
    first.release();
    check(pool.diagnostics().in_use == 1U, "last subscriber retains payload");
    second.release();
    check(pool.diagnostics().in_use == 0U, "last release returns pool slot");
}

template<typename Check>
void verify_typed_routing(Check check)
{
    using topic_type = osal::osal_signal_premium<int, 2, 2>;
    osal::topic_registry<int, 2> registry;
    topic_type                   source, destination;
    const bool                   initialized = registry.valid() && source.valid() && destination.valid();
    check(initialized, "registry and copying topics initialize");
    if (!initialized)
    {
        return;
    }

    osal::signal_id     target{};
    osal::subscriber_id subscriber{};
    const bool          subscribed = destination.subscribe(subscriber);
    const bool          registered = registry.register_topic(destination, target);
    check(subscribed && registered, "topic registration and subscription");
    if (!subscribed || !registered)
    {
        if (registered)
        {
            check(registry.unregister_topic(target) == osal::route_status::delivered, "registration cleanup");
        }
        return;
    }

    check(source.route_to(registry, target, 73) == osal::route_status::delivered, "explicit typed routing");
    int received{};
    check(destination.try_receive(subscriber, received) && received == 73, "routed payload delivery");
    check(source.bind_registry(registry), "registry binding");
    check(source.route_to(target, 91), "bound routing");
    check(destination.try_receive(subscriber, received) && received == 91, "bound payload delivery");
    check(source.unbind_registry(), "registry unbinding");
    check(registry.unregister_topic(target) == osal::route_status::delivered, "checked unregister");
    check(registry.dispatch(target, 0) == osal::route_status::not_found, "stale route rejection");
    check(destination.unsubscribe(subscriber), "routing subscription cleanup");
}

inline void count_stop(void* argument) noexcept
{
    ++*static_cast<unsigned*>(argument);
}

template<typename Check>
void verify_checked_cancellation(Check check)
{
    osal::stop_source source;
    check(source.initialization_status().ok(), "stop source initialization");
    if (!source.valid())
    {
        return;
    }
    unsigned            calls{};
    osal::stop_callback callback{source.get_token(), count_stop, &calls};
    check(callback.status().ok(), "callback registration");
    bool first_request = false;
    check(source.request_stop(first_request).ok() && first_request, "first checked stop request");
    check(calls == 1U && source.get_token().stop_requested(), "callback dispatch and sticky token");
    first_request = true;
    check(source.request_stop(first_request).ok() && !first_request, "repeat request is successful but not first");
    check(calls == 1U, "callback executes exactly once");
    check(callback.close().ok(), "checked callback close");
    check(callback.close().ok(), "callback close is idempotent");

    osal::stop_callback late{source.get_token(), count_stop, &calls};
    check(late.status().ok() && calls == 2U, "late registration dispatches synchronously");
    check(late.close().ok(), "late callback close");
    check(source.status().ok(), "source retains successful synchronization status");
}

struct wake_context
{
    osal::wait_signal&            signal;
    osal::semaphore&              entered;
    osal::stop_token              token;
    std::atomic<bool>             ready{false};
    std::atomic<osal::error_code> announced{osal::error_code::not_initialized};
    std::atomic<osal::error_code> status{osal::error_code::not_initialized};
};

inline void wait_worker(void* argument) noexcept
{
    auto&      context   = *static_cast<wake_context*>(argument);
    bool       announced = false;
    const auto predicate = [&]() noexcept
    {
        if (!announced)
        {
            context.announced.store(context.entered.checked_give().code(), std::memory_order_release);
            announced = true;
        }
        return context.ready.load(std::memory_order_acquire);
    };
    context.status.store(
        osal::wait_until_ready(context.signal, predicate, osal::milliseconds{1000}, context.token).code(),
        std::memory_order_release);
}

template<typename Check>
void verify_task_wakeup(Check check, bool cancel, void* stack, osal::stack_size_t stack_bytes)
{
    static_assert(osal::wait_signal::is_supported);
    osal::wait_signal signal;
    osal::stop_source source;
    osal::semaphore   entered{osal::semaphore_type::binary, 0U};
    const bool        initialized = signal.status().ok() && source.valid() && entered.valid();
    check(initialized, "native wake primitives initialize");
    if (!initialized)
    {
        return;
    }

    wake_context        context{signal, entered, cancel ? source.get_token() : osal::stop_token{}};
    osal::thread_config config{};
    config.entry = wait_worker;
    config.arg   = &context;
    osal::thread invalid;
    check(invalid.create(config) == osal::error_code::invalid_argument, "missing native stack reports an error");
    check(invalid.initialization_status() == osal::error_code::invalid_argument && !invalid.valid(),
          "failed creation retains initialization error without ownership");
    config.stack       = stack;
    config.stack_bytes = stack_bytes;
    osal::thread worker;
    const auto   created = worker.create(config);
    check(created.ok(), "native waiter thread creation");
    if (!created.ok())
    {
        return;
    }
    check(worker.create(config) == osal::error_code::already_exists && worker.valid(),
          "duplicate creation preserves the first thread handle");
    check(entered.checked_take(osal::milliseconds{1000}).ok(), "waiter reaches readiness probe");
    if (cancel)
    {
        bool first_request = false;
        check(source.request_stop(first_request).ok() && first_request, "cancellation signals registered waiter");
    }
    else
    {
        context.ready.store(true, std::memory_order_release);
        check(signal.notify().ok(), "producer explicitly notifies after state mutation");
    }
    check(worker.join().ok(), "waiter cooperatively joins");
    check(context.announced.load(std::memory_order_acquire) == osal::error_code::ok, "waiter handshake succeeds");
    check(context.status.load(std::memory_order_acquire) ==
              (cancel ? osal::error_code::cancelled : osal::error_code::ok),
          "wait returns exact cancellation or readiness result");
    check(signal.status().ok() && source.status().ok(), "wake and cancellation retain successful status");
}

template<typename Check>
void verify_object_waiting(Check check)
{
    osal::queue<int, 1> queue;
    osal::queue<int, 1> other_queue;
    osal::wait_signal   signal;
    osal::stop_source   source;
    const bool          initialized = queue.valid() && other_queue.valid() && signal.status().ok() && source.valid();
    check(initialized, "multi-object wait primitives initialize");
    if (!initialized)
    {
        return;
    }
    osal::object_wait_set objects;
    check(objects.add(queue, 41).ok(), "queue readiness registration");
    check(objects.add(other_queue, 42).ok(), "second queue readiness registration");
    check(queue.send(37).ok() && other_queue.send(38).ok(), "both objects become ready");
    check(signal.notify().ok(), "producer notifies multi-object waiter");
    int         ids[2]{};
    std::size_t count{};
    check(objects.wait(signal, ids, 2U, count, osal::milliseconds{100}).ok(), "event-driven multi-object wait");
    check(count == 2U && ids[0] == 41 && ids[1] == 42, "both readiness IDs are reported");
    int received{};
    check(queue.receive_for(received, osal::milliseconds{0}) && received == 37, "readiness preserves queue data");
    check(other_queue.receive_for(received, osal::milliseconds{0}) && received == 38,
          "readiness preserves second queue data");
    bool first_request = false;
    check(source.request_stop(first_request).ok() && first_request, "multi-object wait cancellation request");
    check(objects.wait(signal, ids, 2U, count, osal::milliseconds{100}, source.get_token()) ==
                  osal::error_code::cancelled &&
              count == 0U,
          "cancelled multi-object wait reports no ready objects");
}

template<typename Check>
void verify_policy_and_diagnostics(Check check)
{
    static_assert(osal::mutex::supports_protocol(osal::mutex_protocol::priority_inheritance));
    osal::mutex_config config{};
    config.protocol = osal::mutex_protocol::priority_inheritance;
    osal::mutex mutex{config};
    check(mutex.initialization_status().ok(), "explicit priority-inheritance policy initializes");
    if (!mutex.valid())
    {
        return;
    }
    {
        osal::mutex::checked_lock_guard guard{mutex};
        check(guard.status().ok() && guard.owns_lock(), "checked guard owns acquired mutex");
        check(guard.unlock().ok() && !guard.owns_lock(), "checked guard releases ownership");
    }
    check(mutex.close().ok() && mutex.close().ok(), "mutex close is checked and idempotent");

    const auto          before = osal::diagnostics(osal::diagnostic_resource::queue).snapshot();
    osal::queue<int, 1> queue;
    check(queue.initialization_status().ok(), "queue initialization preserves result");
    if (!queue.valid())
    {
        return;
    }
    check(osal::diagnostics(osal::diagnostic_resource::queue).snapshot().in_use == before.in_use + 1U,
          "global diagnostics count owning queue handle");
    check(queue.send(42).ok(), "first checked queue send succeeds");
    check(!queue.try_send(43), "full queue rejects nonblocking send");
    const auto statistics = queue.statistics();
    check(statistics.capacity == 1U && statistics.high_water == 1U && statistics.failures == 1U,
          "instance diagnostics measure capacity, high-water and failed send");
    int value{};
    check(queue.receive_for(value, osal::milliseconds{0}) && value == 42,
          "queue data remains intact after failed send");
    check(queue.close().ok() && queue.close().ok(), "queue close is checked and idempotent");
    check(osal::diagnostics(osal::diagnostic_resource::queue).snapshot().in_use == before.in_use,
          "global diagnostics return to baseline after close");
}

}  // namespace osal_test
