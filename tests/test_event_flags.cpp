// SPDX-License-Identifier: Apache-2.0
/// @file test_event_flags.cpp
/// @brief Tests for osal::event_flags.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

TEST_CASE("event_flags: construction succeeds")
{
    osal::event_flags ef;
    CHECK(ef.valid());
}

TEST_CASE("event_flags: starts with all bits clear")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());
    CHECK(ef.get() == 0U);
}

TEST_CASE("event_flags: set and get")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0x05).ok());  // Set bits 0 and 2.
    CHECK((ef.get() & 0x05) == 0x05);
}

TEST_CASE("event_flags: clear")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0xFF).ok());
    REQUIRE(ef.clear(0x0F).ok());
    CHECK((ef.get() & 0x0F) == 0U);
    CHECK((ef.get() & 0xF0) == 0xF0);
}

TEST_CASE("event_flags: wait_any immediate")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0x02).ok());

    osal::event_bits_t actual = 0;
    auto               r      = ef.wait_any(0x02, &actual, false, osal::milliseconds{0});
    CHECK(r.ok());
    CHECK((actual & 0x02) != 0U);
}

TEST_CASE("event_flags: wait_any timeout when no bits set")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    auto r = ef.wait_any(0x01, nullptr, false, osal::milliseconds{20});
    CHECK_FALSE(r.ok());
}

TEST_CASE("event_flags: wait_all immediate")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0x03).ok());

    osal::event_bits_t actual = 0;
    auto               r      = ef.wait_all(0x03, &actual, false, osal::milliseconds{0});
    CHECK(r.ok());
    CHECK((actual & 0x03) == 0x03);
}

TEST_CASE("event_flags: wait_all fails if only partial bits set")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0x01).ok());
    auto r = ef.wait_all(0x03, nullptr, false, osal::milliseconds{20});
    CHECK_FALSE(r.ok());
}

TEST_CASE("event_flags: clear_on_exit")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0x04).ok());

    osal::event_bits_t actual = 0;
    auto               r      = ef.wait_any(0x04, &actual, /*clear_on_exit=*/true, osal::milliseconds{100});
    CHECK(r.ok());

    // Bit should have been auto-cleared.
    CHECK((ef.get() & 0x04) == 0U);
}

TEST_CASE("event_flags: cross-thread signalling")
{
    static osal::event_flags ef;
    REQUIRE(ef.valid());

    // Clear any leftover bits.
    (void)ef.clear(0xFFFFFFFFU);

    auto setter = [](void*)
    {
        osal::thread::sleep_for(osal::milliseconds{20});
        (void)ef.set(0x10);
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    t;
    osal::thread_config             cfg{};
    cfg.entry       = setter;
    cfg.arg         = nullptr;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "ef_set";
    REQUIRE(t.create(cfg).ok());

    osal::event_bits_t actual = 0;
    auto               r      = ef.wait_any(0x10, &actual, true, osal::milliseconds{2000});
    CHECK(r.ok());
    CHECK((actual & 0x10) != 0U);

    REQUIRE(t.join().ok());
}
