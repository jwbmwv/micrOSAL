// SPDX-License-Identifier: Apache-2.0
/// @file test_notification.cpp
/// @brief Tests for osal::notification<Slots>.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

#include <atomic>

TEST_CASE("notification: construction succeeds")
{
    osal::notification<2> note;
    CHECK(note.valid());
    CHECK_FALSE(note.pending(0));
    CHECK(note.peek(0) == 0U);
}

TEST_CASE("notification: overwrite and wait round-trip")
{
    osal::notification<2> note;
    REQUIRE(note.valid());

    static std::atomic<std::uint32_t> received{0U};
    static osal::semaphore            ready{osal::semaphore_type::binary, 0U};
    static osal::semaphore            done{osal::semaphore_type::binary, 0U};
    REQUIRE(ready.valid());
    REQUIRE(done.valid());

    auto worker = [](void* arg)
    {
        auto* n = static_cast<osal::notification<2>*>(arg);
        ready.give();
        std::uint32_t value = 0U;
        const auto    r     = n->wait(1U, osal::milliseconds{2000}, &value);
        if (r.ok())
        {
            received.store(value);
        }
        done.give();
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    t;
    osal::thread_config             cfg{};
    cfg.entry       = worker;
    cfg.arg         = &note;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "note_worker";
    REQUIRE(t.create(cfg).ok());

    CHECK(ready.take_for(osal::milliseconds{2000}));
    CHECK(note.notify(0x12345678U, osal::notification_action::overwrite, 1U).ok());
    CHECK(done.take_for(osal::milliseconds{2000}));
    CHECK(received.load() == 0x12345678U);
    CHECK_FALSE(note.pending(1U));
    REQUIRE(t.join().ok());
}

TEST_CASE("notification: no_overwrite reports would_block when pending")
{
    osal::notification<1> note;
    REQUIRE(note.valid());

    CHECK(note.notify(0xAAU, osal::notification_action::overwrite, 0U).ok());
    CHECK(note.notify(0x55U, osal::notification_action::no_overwrite, 0U) == osal::error_code::would_block);
    CHECK(note.peek(0U) == 0xAAU);
}

TEST_CASE("notification: set_bits and increment update the slot")
{
    osal::notification<1> note;
    REQUIRE(note.valid());

    CHECK(note.notify(0x01U, osal::notification_action::overwrite, 0U).ok());
    CHECK(note.notify(0x04U, osal::notification_action::set_bits, 0U).ok());
    CHECK(note.peek(0U) == 0x05U);

    CHECK(note.notify(0U, osal::notification_action::increment, 0U).ok());
    CHECK(note.peek(0U) == 0x06U);
}

TEST_CASE("notification: slots are isolated")
{
    osal::notification<3> note;
    REQUIRE(note.valid());

    CHECK(note.notify(7U, osal::notification_action::overwrite, 0U).ok());
    CHECK(note.notify(9U, osal::notification_action::overwrite, 2U).ok());

    CHECK(note.pending(0U));
    CHECK_FALSE(note.pending(1U));
    CHECK(note.pending(2U));
    CHECK(note.peek(0U) == 7U);
    CHECK(note.peek(2U) == 9U);
}

TEST_CASE("notification: wait times out when no notification arrives")
{
    osal::notification<1> note;
    REQUIRE(note.valid());

    std::uint32_t value = 0U;
    CHECK(note.wait(0U, osal::milliseconds{20}, &value) == osal::error_code::timeout);
    CHECK(value == 0U);
}