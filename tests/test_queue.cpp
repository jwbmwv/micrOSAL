// SPDX-License-Identifier: Apache-2.0
/// @file test_queue.cpp
/// @brief Tests for osal::queue<T, N>.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

TEST_CASE("queue: construction succeeds")
{
    osal::queue<std::uint32_t, 4> q;
    CHECK(q.valid());
}

TEST_CASE("queue: starts empty")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());
    CHECK(q.empty());
    CHECK_FALSE(q.full());
    CHECK(q.count() == 0);
    CHECK(q.free_slots() == 4);
}

TEST_CASE("queue: send and receive single item")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    REQUIRE(q.send(42U).ok());
    CHECK(q.count() == 1);

    std::uint32_t val = 0;
    REQUIRE(q.receive(val).ok());
    CHECK(val == 42U);
    CHECK(q.empty());
}

TEST_CASE("queue: FIFO ordering")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    REQUIRE(q.send(1U).ok());
    REQUIRE(q.send(2U).ok());
    REQUIRE(q.send(3U).ok());

    std::uint32_t val = 0;
    REQUIRE(q.receive(val).ok());
    CHECK(val == 1U);
    REQUIRE(q.receive(val).ok());
    CHECK(val == 2U);
    REQUIRE(q.receive(val).ok());
    CHECK(val == 3U);
}

TEST_CASE("queue: full detection")
{
    osal::queue<std::uint32_t, 2> q;
    REQUIRE(q.valid());

    REQUIRE(q.try_send(10U));
    REQUIRE(q.try_send(20U));
    CHECK(q.full());
    CHECK_FALSE(q.try_send(30U));
}

TEST_CASE("queue: try_receive on empty returns false")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    std::uint32_t val = 0;
    CHECK_FALSE(q.try_receive(val));
}

TEST_CASE("queue: peek does not remove item")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    REQUIRE(q.send(99U).ok());

    std::uint32_t val = 0;
    CHECK(q.peek(val));
    CHECK(val == 99U);
    CHECK(q.count() == 1);  // Still there.
}

TEST_CASE("queue: struct type")
{
    struct msg_t
    {
        std::uint16_t id;
        std::uint32_t payload;
    };

    osal::queue<msg_t, 8> q;
    REQUIRE(q.valid());

    msg_t out{42, 0xDEADBEEF};
    REQUIRE(q.send(out).ok());

    msg_t in{};
    REQUIRE(q.receive(in).ok());
    CHECK(in.id == 42);
    CHECK(in.payload == 0xDEADBEEF);
}

TEST_CASE("queue: cross-thread send/receive")
{
    static osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    auto producer = [](void*) {
        for (std::uint32_t i = 1; i <= 3; ++i)
        {
            (void)q.send(i);
        }
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread t;
    osal::thread_config cfg{};
    cfg.entry = producer;
    cfg.arg = nullptr;
    cfg.stack = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name = "q_prod";
    REQUIRE(t.create(cfg).ok());

    std::uint32_t val = 0;
    for (std::uint32_t i = 1; i <= 3; ++i)
    {
        REQUIRE(q.receive(val).ok());
        CHECK(val == i);
    }

    REQUIRE(t.join().ok());
}
