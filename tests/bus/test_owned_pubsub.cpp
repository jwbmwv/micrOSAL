// SPDX-License-Identifier: Apache-2.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/bus/osal_signal_premium.hpp>
#include <atomic>
#include <thread>
#include <type_traits>

namespace
{
struct payload
{
    int value{};
    payload() noexcept                 = default;
    payload(const payload&)            = delete;
    payload& operator=(const payload&) = delete;
};
using owned_topic = osal::zero_copy_signal<payload, 2U, 1U, 2U>;
static_assert(!std::is_copy_constructible_v<owned_topic::producer_loan>);
static_assert(std::is_same_v<decltype(std::declval<owned_topic::read_loan>().get()), const payload*>);
static_assert(!osal::osal_signal_capabilities<osal::bus_backend_mock>::zero_copy);
static_assert(std::same_as<decltype(std::declval<const owned_topic&>().diagnostics()), osal::diagnostic_snapshot>);
static_assert(
    std::same_as<decltype(std::declval<const owned_topic::pool_type&>().diagnostics()), osal::diagnostic_snapshot>);
static_assert(osal::detail::bus_diagnostic_gauge(std::numeric_limits<std::size_t>::max()) ==
              std::numeric_limits<std::uint32_t>::max());
using topic = osal::osal_signal_premium<int, 2U, 2U, osal::bus_backend_generic>;
template<typename Registry, typename Topic>
concept registered_topic = requires(Registry& registry, Topic& object, osal::signal_id& id) {
    { registry.register_topic(object, id) } -> std::same_as<bool>;
};
static_assert(!registered_topic<osal::topic_registry<int, 2U>, osal::osal_signal_premium<std::uint32_t, 2U, 2U>>);
static_assert(!registered_topic<osal::topic_registry<payload, 2U>, owned_topic>);
osal::topic_registry<int, 2U>* registry_ptr{};
osal::signal_id                cycle_id{};
osal::signal_id                other_cycle_id{};
osal::route_status             nested_status{};
osal::route_status             unregister_status{};
std::atomic<bool>              entered{};
std::atomic<bool>              finish{};
void                           cycle_observer(const int& value) noexcept
{
    unregister_status = registry_ptr->unregister_topic(cycle_id);
    nested_status     = registry_ptr->dispatch(cycle_id, value);
}
void cycle_forward(const int& value) noexcept
{
    (void)registry_ptr->dispatch(other_cycle_id, value);
}
void blocking_observer(const int&) noexcept
{
    entered.store(true);
    while (!finish.load())
        std::this_thread::yield();
}
topic* observed_topic{};
void   self_remove(const int&) noexcept
{
    (void)observed_topic->unsubscribe_observer(self_remove);
}
}  // namespace

TEST_CASE("owned pubsub pointer identity, immutable fanout and final release")
{
    owned_topic::pool_type pool;
    owned_topic            signal{pool};
    REQUIRE(signal.valid());
    osal::subscriber_id a{}, b{};
    REQUIRE(signal.subscribe(a));
    REQUIRE(signal.subscribe(b));
    auto producer = signal.try_loan();
    REQUIRE(producer);
    producer->value     = 42;
    const auto* address = producer.get();
    const auto  result  = signal.publish_zero_copy(std::move(producer));
    CHECK(result.accepted == 2U);
    CHECK(result.dropped == 0U);
    CHECK(result.status == owned_topic::publish_status::delivered);
    CHECK_FALSE(producer);
    owned_topic::read_loan first, second;
    REQUIRE(signal.receive_loan(a, first));
    REQUIRE(signal.receive_loan(b, second));
    CHECK(first.get() == address);
    CHECK(second.get() == address);
    CHECK(first->value == 42);
    auto retained = first;
    first.release();
    REQUIRE(signal.unsubscribe(a));
    REQUIRE(signal.unsubscribe(b));
    CHECK(second->value == 42);
    CHECK(pool.snapshot().current == 1U);
    second.release();
    CHECK(pool.snapshot().current == 1U);
    retained.release();
    CHECK(pool.snapshot().current == 0U);
    CHECK(pool.snapshot().high_water == 1U);
    auto reused = pool.try_loan();
    CHECK(reused.get() == address);
}

TEST_CASE("owned pubsub full and partial drops preserve failed producer")
{
    owned_topic::pool_type pool;
    owned_topic            signal{pool};
    auto                   unaccepted = signal.try_loan();
    CHECK(signal.publish_zero_copy(std::move(unaccepted)).status == owned_topic::publish_status::no_subscribers);
    CHECK(unaccepted);
    unaccepted.release();
    osal::subscriber_id a{}, b{};
    REQUIRE(signal.subscribe(a));
    REQUIRE(signal.subscribe(b));
    auto first = signal.try_loan();
    REQUIRE(signal.publish_zero_copy(std::move(first)));
    auto second = signal.try_loan();
    auto full   = signal.publish_zero_copy(std::move(second));
    CHECK(full.accepted == 0U);
    CHECK(full.dropped == 2U);
    CHECK(full.status == owned_topic::publish_status::full);
    REQUIRE(second);
    owned_topic::read_loan read;
    REQUIRE(signal.receive_loan(a, read));
    auto partial = signal.publish_zero_copy(std::move(second));
    CHECK(partial.accepted == 1U);
    CHECK(partial.dropped == 1U);
    CHECK(partial.status == owned_topic::publish_status::partial);
    CHECK_FALSE(second);
    CHECK_FALSE(pool.try_loan());
    CHECK(pool.snapshot().exhaustion == 1U);
    REQUIRE(signal.unsubscribe(b));
    CHECK(read);
    read.release();
    REQUIRE(signal.unsubscribe(a));
    CHECK(pool.snapshot().current == 0U);
    CHECK(pool.snapshot().high_water == 2U);
    CHECK(signal.snapshot().drops == 3U);
    CHECK(signal.snapshot().backlog == 0U);
    CHECK_FALSE(signal.receive_loan(a, read));
    osal::subscriber_id replacement{};
    REQUIRE(signal.subscribe(replacement));
    CHECK(replacement != a);
    CHECK_FALSE(signal.unsubscribe(a));
}

TEST_CASE("owned loans survive topic destruction and foreign pools are rejected")
{
    owned_topic::pool_type pool, foreign;
    owned_topic::read_loan read;
    {
        owned_topic         signal{pool};
        osal::subscriber_id id{};
        REQUIRE(signal.subscribe(id));
        auto wrong = foreign.try_loan();
        CHECK(signal.publish_zero_copy(std::move(wrong)).status == owned_topic::publish_status::invalid_loan);
        CHECK(wrong);
        auto loan   = signal.try_loan();
        loan->value = 71;
        REQUIRE(signal.publish_zero_copy(std::move(loan)));
        REQUIRE(signal.receive_loan(id, read));
        auto queued = signal.try_loan();
        REQUIRE(signal.publish_zero_copy(std::move(queued)));
    }
    CHECK(pool.snapshot().current == 1U);
    CHECK(read->value == 71);
    read.release();
    CHECK(pool.snapshot().current == 0U);
}

TEST_CASE("typed registry dispatch, bounded ids, route binding and cycles")
{
    osal::topic_registry<int, 2U> registry;
    topic                         source, target, extra;
    osal::signal_id               source_id{}, target_id{}, extra_id{};
    REQUIRE(registry.register_topic(source, source_id));
    REQUIRE(registry.register_topic(target, target_id));
    CHECK_FALSE(registry.register_topic(extra, extra_id));
    CHECK(extra_id == osal::invalid_signal_id);
    CHECK_FALSE(registry.register_topic(target, extra_id));
    osal::subscriber_id sub{};
    REQUIRE(target.subscribe(sub));
    REQUIRE(source.bind_registry(registry));
    REQUIRE(source.route_to(target_id, 23));
    int value{};
    REQUIRE(target.try_receive(sub, value));
    CHECK(value == 23);
    CHECK(source.route_to(registry, target_id, 24) == osal::route_status::delivered);
    CHECK(registry.dispatch(999U, 0) == osal::route_status::not_found);
    registry_ptr = &registry;
    cycle_id     = source_id;
    REQUIRE(source.subscribe_observer(cycle_observer));
    CHECK(registry.dispatch(source_id, 1) == osal::route_status::delivered);
    CHECK(nested_status == osal::route_status::busy);
    CHECK(unregister_status == osal::route_status::busy);
    CHECK(registry.unregister_topic(target_id) == osal::route_status::delivered);
    CHECK_FALSE(source.route_to(target_id, 1));
    REQUIRE(registry.register_topic(extra, extra_id));
    CHECK(extra_id != target_id);
    CHECK(registry.dispatch(extra_id, 1) == osal::route_status::no_subscribers);
    REQUIRE(source.unbind_registry());
    CHECK_FALSE(source.route_to(source_id, 1));
}

TEST_CASE("registry concurrent unregister is busy without holding callback lock")
{
    osal::topic_registry<int, 2U> registry;
    topic                         target;
    osal::signal_id               id{};
    REQUIRE(registry.register_topic(target, id));
    REQUIRE(target.subscribe_observer(blocking_observer));
    entered = false;
    finish  = false;
    std::thread publisher{[&] { CHECK(registry.dispatch(id, 1) == osal::route_status::delivered); }};
    while (!entered.load())
        std::this_thread::yield();
    CHECK(registry.unregister_topic(id) == osal::route_status::busy);
    CHECK(registry.dispatch(id, 2) == osal::route_status::busy);
    finish = true;
    publisher.join();
    CHECK(registry.unregister_topic(id) == osal::route_status::delivered);
}

TEST_CASE("typed multi-topic cycle terminates at active entry")
{
    osal::topic_registry<int, 2U> registry;
    topic                         first, second;
    REQUIRE(registry.register_topic(first, cycle_id));
    REQUIRE(registry.register_topic(second, other_cycle_id));
    registry_ptr = &registry;
    REQUIRE(first.subscribe_observer(cycle_forward));
    REQUIRE(second.subscribe_observer(cycle_observer));
    CHECK(registry.dispatch(cycle_id, 3) == osal::route_status::delivered);
    CHECK(nested_status == osal::route_status::busy);
    CHECK(unregister_status == osal::route_status::busy);
    CHECK(registry.unregister_topic(cycle_id) == osal::route_status::delivered);
    CHECK(registry.unregister_topic(other_cycle_id) == osal::route_status::delivered);
}

TEST_CASE("premium pointer API remains a copying overload")
{
    topic               copying;
    osal::subscriber_id id{};
    REQUIRE(copying.subscribe(id));
    int input = 6;
    REQUIRE(copying.publish_zero_copy(&input));
    input = 7;
    int output{};
    REQUIRE(copying.try_receive(id, output));
    CHECK(output == 6);
    CHECK_FALSE(copying.publish_zero_copy(nullptr));
}

TEST_CASE("generic observer snapshot supports reentrant removal")
{
    topic target;
    observed_topic = &target;
    REQUIRE(target.subscribe_observer(self_remove));
    REQUIRE(target.publish(1));
    CHECK(target.observer_count() == 0U);
    CHECK_FALSE(target.publish(2));
}

TEST_CASE("owned receive reserves subscriber while waiting")
{
    owned_topic::pool_type pool;
    owned_topic            signal{pool};
    osal::subscriber_id    id{};
    REQUIRE(signal.subscribe(id));
    owned_topic::read_loan read;
    std::thread            receiver{[&] { CHECK(signal.receive_loan(id, read, osal::WAIT_FOREVER)); }};
    while (signal.snapshot().receivers == 0U)
        std::this_thread::yield();
    CHECK_FALSE(signal.unsubscribe(id));
    auto loan   = signal.try_loan();
    loan->value = 19;
    REQUIRE(signal.publish_zero_copy(std::move(loan)));
    receiver.join();
    CHECK(read->value == 19);
    CHECK(signal.unsubscribe(id));
}

TEST_CASE("owned pool and topic expose unified operation diagnostics")
{
    owned_topic::pool_type pool;
    auto                   first  = pool.try_loan();
    auto                   second = pool.try_loan();
    REQUIRE(first);
    REQUIRE(second);
    CHECK_FALSE(pool.try_loan());
    const auto exhausted = pool.diagnostics();
    CHECK(exhausted.capacity == 2U);
    CHECK(exhausted.in_use == 2U);
    CHECK(exhausted.high_water == 2U);
    CHECK(exhausted.attempts == 3U);
    CHECK(exhausted.successes == 2U);
    CHECK(exhausted.failures == 1U);
    second.release();
    CHECK(pool.diagnostics().in_use == 1U);

    owned_topic signal{pool};
    CHECK_FALSE(signal.publish_zero_copy(std::move(first)));
    osal::subscriber_id a{}, b{};
    REQUIRE(signal.subscribe(a));
    REQUIRE(signal.subscribe(b));
    // NOLINTNEXTLINE(bugprone-use-after-move): zero acceptance preserves producer ownership.
    REQUIRE(signal.publish_zero_copy(std::move(first)));
    second = pool.try_loan();
    REQUIRE(second);
    CHECK_FALSE(signal.publish_zero_copy(std::move(second)));
    owned_topic::read_loan read;
    REQUIRE(signal.receive_loan(a, read));
    // NOLINTNEXTLINE(bugprone-use-after-move): zero acceptance preserves producer ownership.
    CHECK(signal.publish_zero_copy(std::move(second)).status == owned_topic::publish_status::partial);
    const auto published = signal.diagnostics();
    CHECK(published.capacity == 2U);
    CHECK(published.in_use == 2U);
    CHECK(published.backlog == 2U);
    CHECK(published.high_water == 2U);
    CHECK(published.attempts == 4U);
    CHECK(published.successes == 2U);
    CHECK(published.failures == 2U);
    CHECK(published.drops == 3U);
    CHECK(pool.diagnostics().attempts == 4U);
    CHECK(pool.diagnostics().successes == 3U);
    REQUIRE(signal.unsubscribe(a));
    REQUIRE(signal.unsubscribe(b));
    CHECK(signal.diagnostics().in_use == 0U);
    CHECK(signal.diagnostics().backlog == 0U);
    CHECK(signal.diagnostics().high_water == 2U);
    CHECK(pool.diagnostics().in_use == 1U);
    read.release();
    CHECK(pool.diagnostics().in_use == 0U);
    CHECK(pool.diagnostics().high_water == 2U);
}
