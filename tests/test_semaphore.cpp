// SPDX-License-Identifier: Apache-2.0
/// @file test_semaphore.cpp
/// @brief Tests for osal::semaphore (binary and counting).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

// ---------------------------------------------------------------------------
// Binary semaphore
// ---------------------------------------------------------------------------

TEST_CASE("binary semaphore: construction succeeds")
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    CHECK(s.valid());
}

TEST_CASE("binary semaphore: give then take")
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    REQUIRE(s.valid());
    s.give();
    CHECK(s.try_take());
}

TEST_CASE("binary semaphore: try_take fails when empty")
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    REQUIRE(s.valid());
    CHECK_FALSE(s.try_take());
}

TEST_CASE("binary semaphore: initialized with count=1")
{
    osal::semaphore s{osal::semaphore_type::binary, 1U};
    REQUIRE(s.valid());
    CHECK(s.try_take());
    CHECK_FALSE(s.try_take());
}

TEST_CASE("binary semaphore: take_for with timeout")
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    REQUIRE(s.valid());
    // Should time out since no one gives.
    CHECK_FALSE(s.take_for(osal::milliseconds{20}));
}

// ---------------------------------------------------------------------------
// Counting semaphore
// ---------------------------------------------------------------------------

TEST_CASE("counting semaphore: construction succeeds")
{
    osal::semaphore s{osal::semaphore_type::counting, 0U, 10U};
    CHECK(s.valid());
}

TEST_CASE("counting semaphore: give and take multiple")
{
    osal::semaphore s{osal::semaphore_type::counting, 0U, 5U};
    REQUIRE(s.valid());

    s.give();
    s.give();
    s.give();

    CHECK(s.try_take());
    CHECK(s.try_take());
    CHECK(s.try_take());
    CHECK_FALSE(s.try_take());
}

TEST_CASE("counting semaphore: initial count")
{
    osal::semaphore s{osal::semaphore_type::counting, 3U, 5U};
    REQUIRE(s.valid());

    CHECK(s.try_take());
    CHECK(s.try_take());
    CHECK(s.try_take());
    CHECK_FALSE(s.try_take());
}

// ---------------------------------------------------------------------------
// Cross-thread signalling
// ---------------------------------------------------------------------------

TEST_CASE("semaphore: thread signalling")
{
    osal::semaphore s{osal::semaphore_type::binary, 0U};
    REQUIRE(s.valid());

    struct ctx_t
    {
        osal::semaphore* sem;
    } ctx{&s};

    auto entry = [](void* arg) {
        auto* c = static_cast<ctx_t*>(arg);
        osal::thread::sleep_for(osal::milliseconds{20});
        c->sem->give();
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread t;
    osal::thread_config cfg{};
    cfg.entry = entry;
    cfg.arg = &ctx;
    cfg.stack = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name = "sem_signal";
    REQUIRE(t.create(cfg).ok());

    // Should block until the child gives (timed to avoid hanging on failure).
    CHECK(s.take_for(osal::milliseconds{2000}));
    REQUIRE(t.join().ok());
}

// ---------------------------------------------------------------------------
// Config-based construction (FLASH placement)
// ---------------------------------------------------------------------------

TEST_CASE("semaphore: config construction — binary")
{
    const osal::semaphore_config cfg{osal::semaphore_type::binary, 1U, 1U};
    osal::semaphore s{cfg};
    REQUIRE(s.valid());
    CHECK(s.try_take());
    CHECK_FALSE(s.try_take());
}

TEST_CASE("semaphore: config construction — counting")
{
    const osal::semaphore_config cfg{osal::semaphore_type::counting, 3U, 10U};
    osal::semaphore s{cfg};
    REQUIRE(s.valid());

    CHECK(s.try_take());
    CHECK(s.try_take());
    CHECK(s.try_take());
    CHECK_FALSE(s.try_take());
}

TEST_CASE("semaphore: constexpr config compiles")
{
    constexpr osal::semaphore_config cfg{};
    osal::semaphore s{cfg};
    CHECK(s.valid());
}
