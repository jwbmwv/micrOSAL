// SPDX-License-Identifier: Apache-2.0
/// @file test_mailbox.cpp
/// @brief Tests for osal::mailbox<T>.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

#include <cstdint>

TEST_CASE("mailbox: construction succeeds")
{
    osal::mailbox<std::uint32_t> mb;
    CHECK(mb.valid());
    CHECK(mb.empty());
    CHECK(mb.count() == 0U);
}

TEST_CASE("mailbox: send and receive single item")
{
    osal::mailbox<std::uint32_t> mb;
    REQUIRE(mb.valid());

    REQUIRE(mb.send(42U).ok());
    CHECK(mb.full());

    std::uint32_t value = 0U;
    REQUIRE(mb.receive(value).ok());
    CHECK(value == 42U);
    CHECK(mb.empty());
}

TEST_CASE("mailbox: full detection and aliases")
{
    osal::mailbox<std::uint32_t> mb;
    REQUIRE(mb.valid());

    REQUIRE(mb.post(7U).ok());
    CHECK_FALSE(mb.try_post(8U));

    std::uint32_t peeked = 0U;
    REQUIRE(mb.peek(peeked));
    CHECK(peeked == 7U);
    CHECK(mb.count() == 1U);
}

TEST_CASE("mailbox: cross-thread send and receive")
{
    static osal::mailbox<std::uint32_t> mb;
    REQUIRE(mb.valid());

    auto producer = [](void*) { (void)mb.send(99U); };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    thread;
    osal::thread_config             cfg{};
    cfg.entry       = producer;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "mb_prod";

    REQUIRE(thread.create(cfg).ok());

    std::uint32_t value = 0U;
    REQUIRE(mb.receive(value).ok());
    CHECK(value == 99U);
    REQUIRE(thread.join().ok());
}