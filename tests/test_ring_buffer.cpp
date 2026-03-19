// SPDX-License-Identifier: Apache-2.0
/// @file test_ring_buffer.cpp
/// @brief Tests for osal::ring_buffer (lock-free SPSC ring).
/// @note  ring_buffer has zero OS dependency — no threads needed for most cases.
///        Multi-thread producer/consumer tests are included to exercise the
///        actual SPSC use-case.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>
#include <cstdint>

// ---------------------------------------------------------------------------
// Construction and initial state
// ---------------------------------------------------------------------------

TEST_CASE("ring_buffer: default construction is empty")
{
    osal::ring_buffer<int, 4> rb;
    CHECK(rb.empty());
    CHECK_FALSE(rb.full());
    CHECK(rb.size() == 0U);
    CHECK(rb.free() == 4U);
    CHECK(rb.capacity() == 4U);
}

// ---------------------------------------------------------------------------
// try_push and try_pop — basic
// ---------------------------------------------------------------------------

TEST_CASE("ring_buffer: try_push and try_pop single item")
{
    osal::ring_buffer<int, 4> rb;
    CHECK(rb.try_push(42));
    CHECK(rb.size() == 1U);
    CHECK_FALSE(rb.empty());

    int val = 0;
    CHECK(rb.try_pop(val));
    CHECK(val == 42);
    CHECK(rb.empty());
}

TEST_CASE("ring_buffer: items are FIFO")
{
    osal::ring_buffer<int, 8> rb;
    for (int i = 0; i < 8; ++i)
    {
        CHECK(rb.try_push(i));
    }
    CHECK(rb.full());

    for (int i = 0; i < 8; ++i)
    {
        int val = -1;
        CHECK(rb.try_pop(val));
        CHECK(val == i);
    }
    CHECK(rb.empty());
}

// ---------------------------------------------------------------------------
// Full buffer — try_push returns false
// ---------------------------------------------------------------------------

TEST_CASE("ring_buffer: try_push fails when full")
{
    osal::ring_buffer<int, 2> rb;
    CHECK(rb.try_push(1));
    CHECK(rb.try_push(2));
    CHECK(rb.full());
    CHECK_FALSE(rb.try_push(3));
    CHECK(rb.size() == 2U);
}

// ---------------------------------------------------------------------------
// Empty buffer — try_pop returns false
// ---------------------------------------------------------------------------

TEST_CASE("ring_buffer: try_pop fails when empty")
{
    osal::ring_buffer<int, 4> rb;
    int                       val = 0;
    CHECK_FALSE(rb.try_pop(val));
}

// ---------------------------------------------------------------------------
// peek — does not consume
// ---------------------------------------------------------------------------

TEST_CASE("ring_buffer: peek does not remove the item")
{
    osal::ring_buffer<int, 4> rb;
    CHECK(rb.try_push(77));

    int v1 = 0;
    CHECK(rb.peek(v1));
    CHECK(v1 == 77);
    CHECK(rb.size() == 1U);  // Item still present.

    int v2 = 0;
    CHECK(rb.try_pop(v2));
    CHECK(v2 == 77);
    CHECK(rb.empty());
}

TEST_CASE("ring_buffer: peek fails when empty")
{
    osal::ring_buffer<int, 4> rb;
    int                       val = 0;
    CHECK_FALSE(rb.peek(val));
}

// ---------------------------------------------------------------------------
// Wrap-around — push/pop past end of internal array
// ---------------------------------------------------------------------------

TEST_CASE("ring_buffer: wrap-around maintains FIFO order")
{
    osal::ring_buffer<int, 4> rb;  // Internal capacity N+1=5.

    // Fill to capacity.
    for (int i = 0; i < 4; ++i)
        CHECK(rb.try_push(i));

    // Consume two.
    int v;
    CHECK(rb.try_pop(v));
    CHECK(v == 0);
    CHECK(rb.try_pop(v));
    CHECK(v == 1);

    // Push two more (triggers wrap-around).
    CHECK(rb.try_push(10));
    CHECK(rb.try_push(11));

    // Now drain: should be 2, 3, 10, 11.
    CHECK(rb.try_pop(v));
    CHECK(v == 2);
    CHECK(rb.try_pop(v));
    CHECK(v == 3);
    CHECK(rb.try_pop(v));
    CHECK(v == 10);
    CHECK(rb.try_pop(v));
    CHECK(v == 11);
    CHECK(rb.empty());
}

// ---------------------------------------------------------------------------
// reset()
// ---------------------------------------------------------------------------

TEST_CASE("ring_buffer: reset discards all items")
{
    osal::ring_buffer<int, 8> rb;
    for (int i = 0; i < 5; ++i)
        rb.try_push(i);
    CHECK(rb.size() == 5U);

    rb.reset();
    CHECK(rb.empty());
    CHECK(rb.size() == 0U);
    CHECK(rb.free() == 8U);
}

// ---------------------------------------------------------------------------
// Works with non-int trivially-copyable types
// ---------------------------------------------------------------------------

TEST_CASE("ring_buffer: works with struct types")
{
    struct Pair
    {
        int x;
        int y;
    };
    osal::ring_buffer<Pair, 4> rb;

    CHECK(rb.try_push({1, 2}));
    CHECK(rb.try_push({3, 4}));

    Pair p{};
    CHECK(rb.try_pop(p));
    CHECK(p.x == 1);
    CHECK(p.y == 2);
    CHECK(rb.try_pop(p));
    CHECK(p.x == 3);
    CHECK(p.y == 4);
}

// ---------------------------------------------------------------------------
// Capacity == 1
// ---------------------------------------------------------------------------

TEST_CASE("ring_buffer: capacity-1 edge case")
{
    osal::ring_buffer<int, 1> rb;
    CHECK(rb.capacity() == 1U);
    CHECK(rb.empty());

    CHECK(rb.try_push(99));
    CHECK(rb.full());
    CHECK_FALSE(rb.try_push(100));

    int v = 0;
    CHECK(rb.try_pop(v));
    CHECK(v == 99);
    CHECK(rb.empty());
}

// ---------------------------------------------------------------------------
// SPSC: concurrent producer and consumer (integration style)
// ---------------------------------------------------------------------------

TEST_CASE("ring_buffer: SPSC producer + consumer correctness")
{
    static osal::ring_buffer<std::uint32_t, 64> rb;
    rb.reset();

    static std::atomic<bool>          producer_done{false};
    static std::atomic<std::uint32_t> consumed_count{0};
    static std::atomic<bool>          order_violation{false};
    producer_done.store(false);
    consumed_count.store(0);
    order_violation.store(false);

    constexpr std::uint32_t kItems = 10000U;

    auto producer = [](void*)
    {
        for (std::uint32_t i = 0; i < kItems; ++i)
        {
            while (!rb.try_push(i))
            {
                osal::thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    };

    auto consumer = [](void*)
    {
        std::uint32_t expected = 0;
        while (expected < kItems)
        {
            std::uint32_t val;
            if (rb.try_pop(val))
            {
                if (val != expected)
                {
                    order_violation.store(true);
                }
                ++expected;
                consumed_count.fetch_add(1);
            }
            else
            {
                osal::thread::yield();
            }
        }
    };

    alignas(16) static std::uint8_t stack_p[65536];
    alignas(16) static std::uint8_t stack_c[65536];
    osal::thread                    tp, tc;
    osal::thread_config             cfg{};

    cfg.entry       = producer;
    cfg.arg         = nullptr;
    cfg.stack       = stack_p;
    cfg.stack_bytes = sizeof(stack_p);
    cfg.name        = "prod";
    REQUIRE(tp.create(cfg).ok());

    cfg.entry       = consumer;
    cfg.arg         = nullptr;
    cfg.stack       = stack_c;
    cfg.stack_bytes = sizeof(stack_c);
    cfg.name        = "cons";
    REQUIRE(tc.create(cfg).ok());

    REQUIRE(tp.join().ok());
    REQUIRE(tc.join().ok());

    CHECK_FALSE(order_violation.load());
    CHECK(consumed_count.load() == kItems);
    CHECK(rb.empty());
}
