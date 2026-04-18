// SPDX-License-Identifier: Apache-2.0
/// @file test_osal_signal_premium.cpp
/// @brief Tests for the premium pub/sub layer (osal::osal_signal_premium).
///
/// Covers:
///   - Observer subscription / unsubscription
///   - Observer invocation on publish
///   - Multiple observers receive independent callbacks
///   - Observer table capacity limit
///   - publish() fans out to both queue-subscribers AND observers
///   - publish_zero_copy() falls back to copy on non-zero-copy backends
///   - route_to() stub returns false (pending registry)
///   - Premium capability flags on mock backend
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <microsal/bus/osal_signal_premium.hpp>
#include <cstdint>
#include <array>

// ---------------------------------------------------------------------------
// Helper types
// ---------------------------------------------------------------------------

using MockTopic = osal::osal_signal_premium<std::uint32_t, 4U, 8U, osal::bus_backend_mock>;

// ---------------------------------------------------------------------------
// Observer helpers (file-scope, stateless counters)
// ---------------------------------------------------------------------------

static std::uint32_t g_obs_a_last{0U};
static std::uint32_t g_obs_b_last{0U};
static std::size_t   g_obs_a_calls{0U};
static std::size_t   g_obs_b_calls{0U};

static void reset_observers() noexcept
{
    g_obs_a_last  = 0U;
    g_obs_b_last  = 0U;
    g_obs_a_calls = 0U;
    g_obs_b_calls = 0U;
}

static void obs_a(const std::uint32_t& v) noexcept
{
    g_obs_a_last = v;
    ++g_obs_a_calls;
}

static void obs_b(const std::uint32_t& v) noexcept
{
    g_obs_b_last = v;
    ++g_obs_b_calls;
}

// ---------------------------------------------------------------------------
// Observer subscription / unsubscription
// ---------------------------------------------------------------------------

TEST_CASE("osal_signal_premium: subscribe_observer succeeds")
{
    reset_observers();
    MockTopic topic;
    REQUIRE(topic.subscribe_observer(obs_a));
    CHECK(topic.observer_count() == 1U);
}

TEST_CASE("osal_signal_premium: subscribe_observer nullptr returns false")
{
    MockTopic topic;
    CHECK_FALSE(topic.subscribe_observer(nullptr));
    CHECK(topic.observer_count() == 0U);
}

TEST_CASE("osal_signal_premium: unsubscribe_observer succeeds")
{
    reset_observers();
    MockTopic topic;
    REQUIRE(topic.subscribe_observer(obs_a));
    REQUIRE(topic.unsubscribe_observer(obs_a));
    CHECK(topic.observer_count() == 0U);
}

TEST_CASE("osal_signal_premium: unsubscribe not-registered observer returns false")
{
    MockTopic topic;
    CHECK_FALSE(topic.unsubscribe_observer(obs_a));
}

TEST_CASE("osal_signal_premium: observer table capacity limit")
{
    MockTopic topic;
    // MaxSubscribers == 4 → max 4 observers

    // Fill all 4 slots with the same pointer (simplification for test)
    static void (*fn1)(const std::uint32_t&) noexcept = [](const std::uint32_t&) noexcept {};
    static void (*fn2)(const std::uint32_t&) noexcept = [](const std::uint32_t&) noexcept {};
    static void (*fn3)(const std::uint32_t&) noexcept = [](const std::uint32_t&) noexcept {};
    static void (*fn4)(const std::uint32_t&) noexcept = [](const std::uint32_t&) noexcept {};

    REQUIRE(topic.subscribe_observer(fn1));
    REQUIRE(topic.subscribe_observer(fn2));
    REQUIRE(topic.subscribe_observer(fn3));
    REQUIRE(topic.subscribe_observer(fn4));
    CHECK(topic.observer_count() == 4U);

    // Fifth registration should fail
    CHECK_FALSE(topic.subscribe_observer(obs_a));
}

// ---------------------------------------------------------------------------
// Observer invocation
// ---------------------------------------------------------------------------

TEST_CASE("osal_signal_premium: publish invokes observer callback")
{
    reset_observers();
    MockTopic topic;
    REQUIRE(topic.subscribe_observer(obs_a));

    (void)topic.publish(42U);
    CHECK(g_obs_a_calls == 1U);
    CHECK(g_obs_a_last == 42U);
}

TEST_CASE("osal_signal_premium: publish invokes all registered observers")
{
    reset_observers();
    MockTopic topic;
    REQUIRE(topic.subscribe_observer(obs_a));
    REQUIRE(topic.subscribe_observer(obs_b));

    (void)topic.publish(7U);
    CHECK(g_obs_a_calls == 1U);
    CHECK(g_obs_a_last == 7U);
    CHECK(g_obs_b_calls == 1U);
    CHECK(g_obs_b_last == 7U);
}

TEST_CASE("osal_signal_premium: observer not called after unsubscribe")
{
    reset_observers();
    MockTopic topic;
    REQUIRE(topic.subscribe_observer(obs_a));
    REQUIRE(topic.unsubscribe_observer(obs_a));

    (void)topic.publish(99U);
    CHECK(g_obs_a_calls == 0U);
}

// ---------------------------------------------------------------------------
// Publish fans out to BOTH queue-subscribers and observers
// ---------------------------------------------------------------------------

TEST_CASE("osal_signal_premium: publish reaches queue-subscriber AND observer")
{
    reset_observers();
    MockTopic topic;

    osal::subscriber_id sub{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(sub));
    REQUIRE(topic.subscribe_observer(obs_a));

    REQUIRE(topic.publish(55U));

    // Queue-subscriber received it
    std::uint32_t val{0U};
    REQUIRE(topic.try_receive(sub, val));
    CHECK(val == 55U);

    // Observer was called
    CHECK(g_obs_a_calls == 1U);
    CHECK(g_obs_a_last == 55U);

    (void)topic.unsubscribe(sub);
}

// ---------------------------------------------------------------------------
// publish_zero_copy
// ---------------------------------------------------------------------------

TEST_CASE("osal_signal_premium: publish_zero_copy with valid pointer")
{
    reset_observers();
    MockTopic topic;

    osal::subscriber_id sub{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(sub));
    REQUIRE(topic.subscribe_observer(obs_a));

    std::uint32_t msg{123U};
    REQUIRE(topic.publish_zero_copy(&msg));

    std::uint32_t val{0U};
    REQUIRE(topic.try_receive(sub, val));
    CHECK(val == 123U);
    CHECK(g_obs_a_last == 123U);

    (void)topic.unsubscribe(sub);
}

TEST_CASE("osal_signal_premium: publish_zero_copy nullptr returns false")
{
    MockTopic topic;
    CHECK_FALSE(topic.publish_zero_copy(nullptr));
}

// ---------------------------------------------------------------------------
// route_to (stub)
// ---------------------------------------------------------------------------

TEST_CASE("osal_signal_premium: route_to stub returns false")
{
    MockTopic topic;
    std::uint32_t msg{1U};
    CHECK_FALSE(topic.route_to(1U, msg));
}

// ---------------------------------------------------------------------------
// Capability detection
// ---------------------------------------------------------------------------

TEST_CASE("osal_signal_premium: capability concepts on mock backend")
{
    static_assert(osal::native_pubsub_backend<osal::bus_backend_mock>);
    static_assert(osal::native_observer_backend<osal::bus_backend_mock>);
    static_assert(osal::zero_copy_backend<osal::bus_backend_mock>);
    static_assert(osal::native_routing_backend<osal::bus_backend_mock>);
    CHECK(true);  // concepts checked at compile time
}

TEST_CASE("osal_signal_premium: capability concepts on generic backend")
{
    static_assert(!osal::native_pubsub_backend<osal::bus_backend_generic>);
    static_assert(!osal::native_observer_backend<osal::bus_backend_generic>);
    static_assert(!osal::zero_copy_backend<osal::bus_backend_generic>);
    static_assert(!osal::native_routing_backend<osal::bus_backend_generic>);
    CHECK(true);
}
