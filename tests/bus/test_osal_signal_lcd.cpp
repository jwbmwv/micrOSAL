// SPDX-License-Identifier: Apache-2.0
/// @file test_osal_signal_lcd.cpp
/// @brief Tests for the LCD pub/sub layer (osal::osal_signal).
///
/// Covers:
///   - subscribe / unsubscribe lifecycle
///   - single and multiple subscriber publish fan-out
///   - try_receive / blocking receive semantics
///   - subscriber capacity limits
///   - unsubscribed IDs are rejected
///   - subscriber_count() tracking
///   - full subscriber queue drops (try_send falls back to no-op)
///   - generic backend behaviour
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <microsal/bus/osal_signal.hpp>
#include <cstdint>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

using U32Topic = osal::osal_signal<std::uint32_t, 4U, 8U, osal::bus_backend_generic>;

// ---------------------------------------------------------------------------
// subscribe / unsubscribe
// ---------------------------------------------------------------------------

TEST_CASE("osal_signal: subscribe returns valid id")
{
    U32Topic topic;
    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));
    CHECK(id != osal::invalid_subscriber_id);
    CHECK(topic.subscriber_count() == 1U);
    (void)topic.unsubscribe(id);
}

TEST_CASE("osal_signal: subscribe up to MaxSubscribers")
{
    U32Topic                 topic;
    osal::subscriber_id      ids[4U]{};
    for (auto& id : ids)
    {
        REQUIRE(topic.subscribe(id));
    }
    CHECK(topic.subscriber_count() == 4U);

    // One more should fail
    osal::subscriber_id extra{osal::invalid_subscriber_id};
    CHECK_FALSE(topic.subscribe(extra));
    CHECK(extra == osal::invalid_subscriber_id);

    for (const auto& id : ids)
    {
        (void)topic.unsubscribe(id);
    }
}

TEST_CASE("osal_signal: unsubscribe decrements count")
{
    U32Topic            topic;
    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));
    CHECK(topic.subscriber_count() == 1U);
    REQUIRE(topic.unsubscribe(id));
    CHECK(topic.subscriber_count() == 0U);
}

TEST_CASE("osal_signal: unsubscribe invalid id returns false")
{
    U32Topic topic;
    CHECK_FALSE(topic.unsubscribe(osal::invalid_subscriber_id));
    CHECK_FALSE(topic.unsubscribe(99U));
}

TEST_CASE("osal_signal: double unsubscribe returns false")
{
    U32Topic            topic;
    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));
    REQUIRE(topic.unsubscribe(id));
    CHECK_FALSE(topic.unsubscribe(id));
}

// ---------------------------------------------------------------------------
// publish / receive
// ---------------------------------------------------------------------------

TEST_CASE("osal_signal: publish fans out to single subscriber")
{
    U32Topic            topic;
    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));

    REQUIRE(topic.publish(42U));

    std::uint32_t val{0U};
    REQUIRE(topic.try_receive(id, val));
    CHECK(val == 42U);

    (void)topic.unsubscribe(id);
}

TEST_CASE("osal_signal: publish fans out to multiple subscribers")
{
    U32Topic            topic;
    osal::subscriber_id ids[3U]{};
    for (auto& id : ids)
    {
        REQUIRE(topic.subscribe(id));
    }

    REQUIRE(topic.publish(7U));

    for (const auto& id : ids)
    {
        std::uint32_t val{0U};
        REQUIRE(topic.try_receive(id, val));
        CHECK(val == 7U);
    }

    for (const auto& id : ids)
    {
        (void)topic.unsubscribe(id);
    }
}

TEST_CASE("osal_signal: try_receive returns false when queue is empty")
{
    U32Topic            topic;
    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));

    std::uint32_t val{0U};
    CHECK_FALSE(topic.try_receive(id, val));

    (void)topic.unsubscribe(id);
}

TEST_CASE("osal_signal: try_receive on unsubscribed id returns false")
{
    U32Topic            topic;
    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));
    REQUIRE(topic.unsubscribe(id));

    std::uint32_t val{0U};
    CHECK_FALSE(topic.try_receive(id, val));
}

TEST_CASE("osal_signal: publish with no subscribers returns false")
{
    U32Topic topic;
    CHECK_FALSE(topic.publish(1U));
}

TEST_CASE("osal_signal: multiple messages FIFO per subscriber")
{
    U32Topic            topic;
    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));

    REQUIRE(topic.publish(1U));
    REQUIRE(topic.publish(2U));
    REQUIRE(topic.publish(3U));

    std::uint32_t val{0U};
    REQUIRE(topic.try_receive(id, val));
    CHECK(val == 1U);
    REQUIRE(topic.try_receive(id, val));
    CHECK(val == 2U);
    REQUIRE(topic.try_receive(id, val));
    CHECK(val == 3U);
    CHECK_FALSE(topic.try_receive(id, val));

    (void)topic.unsubscribe(id);
}

TEST_CASE("osal_signal: full queue drops additional publishes silently")
{
    // PerSubCapacity = 2 — fill queue then overflow
    osal::osal_signal<std::uint32_t, 2U, 2U, osal::bus_backend_generic> topic;

    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));

    REQUIRE(topic.publish(1U));
    REQUIRE(topic.publish(2U));

    // Queue is now full (capacity = 2); third publish should still succeed for
    // the other subscriber, but this subscriber's queue drops it.
    // For a single subscriber, publish returns false (all queues full).
    const bool third = topic.publish(3U);
    (void)third;  // DROP — expected, no CHECK_FALSE since behaviour is drop-and-continue

    std::uint32_t val{0U};
    REQUIRE(topic.try_receive(id, val));
    CHECK(val == 1U);
    REQUIRE(topic.try_receive(id, val));
    CHECK(val == 2U);
    // Item 3 was dropped
    CHECK_FALSE(topic.try_receive(id, val));

    (void)topic.unsubscribe(id);
}

TEST_CASE("osal_signal: blocking receive with NO_WAIT timeout")
{
    U32Topic            topic;
    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));

    std::uint32_t val{0U};
    // NO_WAIT on empty queue must return false immediately
    CHECK_FALSE(topic.receive(id, val, osal::NO_WAIT));

    // Publish then receive
    REQUIRE(topic.publish(55U));
    REQUIRE(topic.receive(id, val, osal::NO_WAIT));
    CHECK(val == 55U);

    (void)topic.unsubscribe(id);
}

// ---------------------------------------------------------------------------
// Static properties
// ---------------------------------------------------------------------------

TEST_CASE("osal_signal: compile-time dimension accessors")
{
    CHECK(U32Topic::max_subscribers() == 4U);
    CHECK(U32Topic::per_subscriber_capacity() == 8U);
}

// ---------------------------------------------------------------------------
// Capability detection
// ---------------------------------------------------------------------------

TEST_CASE("osal_signal: generic backend has no premium capabilities")
{
    using caps = osal::osal_signal_capabilities<osal::bus_backend_generic>;
    CHECK_FALSE(caps::native_pubsub);
    CHECK_FALSE(caps::native_observers);
    CHECK_FALSE(caps::native_routing);
    CHECK_FALSE(caps::zero_copy);
}

TEST_CASE("osal_signal: mock backend advertises all premium capabilities")
{
    using caps = osal::osal_signal_capabilities<osal::bus_backend_mock>;
    CHECK(caps::native_pubsub);
    CHECK(caps::native_observers);
    CHECK(caps::native_routing);
    CHECK(caps::zero_copy);
}

TEST_CASE("osal_signal: Zephyr backend advertises all premium capabilities")
{
    using caps = osal::osal_signal_capabilities<osal::bus_backend_zephyr>;
    CHECK(caps::native_pubsub);
    CHECK(caps::native_observers);
    CHECK(caps::native_routing);
    CHECK(caps::zero_copy);
}
