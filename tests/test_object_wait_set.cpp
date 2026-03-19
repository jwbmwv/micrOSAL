// SPDX-License-Identifier: Apache-2.0
/// @file test_object_wait_set.cpp
/// @brief Tests for osal::object_wait_set.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

TEST_CASE("object_wait_set: construction succeeds")
{
    osal::object_wait_set ws;
    CHECK(ws.valid());
}

TEST_CASE("object_wait_set: queue readiness returns the registered id")
{
    osal::object_wait_set         ws;
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());
    REQUIRE(ws.add(q, 11).ok());
    REQUIRE(q.send(7U).ok());

    int         ready[4]{};
    std::size_t n_ready = 0U;
    REQUIRE(ws.wait(ready, 4U, n_ready, osal::milliseconds{20}).ok());
    CHECK(n_ready == 1U);
    CHECK(ready[0] == 11);
}

TEST_CASE("object_wait_set: event_flags any can clear on exit")
{
    osal::object_wait_set ws;
    osal::event_flags     flags;
    REQUIRE(flags.valid());
    REQUIRE(ws.add_any(flags, 0x03U, 21, true).ok());
    REQUIRE(flags.set(0x01U).ok());

    int         ready[4]{};
    std::size_t n_ready = 0U;
    REQUIRE(ws.wait(ready, 4U, n_ready, osal::milliseconds{20}).ok());
    CHECK(n_ready == 1U);
    CHECK(ready[0] == 21);
    CHECK((flags.get() & 0x01U) == 0U);
}

TEST_CASE("object_wait_set: notification readiness is reported")
{
    osal::object_wait_set ws;
    osal::notification<2> note;
    REQUIRE(note.valid());
    REQUIRE(ws.add(note, 1U, 31, true).ok());
    REQUIRE(note.notify(0x55U, osal::notification_action::overwrite, 1U).ok());

    int         ready[4]{};
    std::size_t n_ready = 0U;
    REQUIRE(ws.wait(ready, 4U, n_ready, osal::milliseconds{20}).ok());
    CHECK(n_ready == 1U);
    CHECK(ready[0] == 31);
    CHECK_FALSE(note.pending(1U));
}

TEST_CASE("object_wait_set: delayable work can be observed as pending")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped - backend lacks timers");
        return;
    }

    alignas(16) static std::uint8_t stack[65536];
    osal::work_queue                wq{stack, sizeof(stack), 8, "ow_wq"};
    REQUIRE(wq.valid());

    osal::delayable_work work{wq, +[](void*) {}, nullptr, "ow_dw"};
    REQUIRE(work.valid());

    osal::object_wait_set ws;
    REQUIRE(ws.add(work, 41).ok());
    REQUIRE(work.schedule(osal::milliseconds{50}).ok());

    int         ready[4]{};
    std::size_t n_ready = 0U;
    REQUIRE(ws.wait(ready, 4U, n_ready, osal::milliseconds{20}).ok());
    CHECK(n_ready == 1U);
    CHECK(ready[0] == 41);
    REQUIRE(work.cancel().ok());
}

TEST_CASE("object_wait_set: remove clears an entry")
{
    osal::object_wait_set         ws;
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());
    REQUIRE(ws.add(q, 51).ok());
    REQUIRE(ws.remove(51).ok());
    REQUIRE(q.send(1U).ok());

    int         ready[4]{};
    std::size_t n_ready = 0U;
    CHECK(ws.wait(ready, 4U, n_ready, osal::milliseconds{20}) == osal::error_code::timeout);
    CHECK(n_ready == 0U);
}

TEST_CASE("object_wait_set: wait times out when nothing is ready")
{
    osal::object_wait_set         ws;
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());
    REQUIRE(ws.add(q, 61).ok());

    int         ready[4]{};
    std::size_t n_ready = 0U;
    CHECK(ws.wait(ready, 4U, n_ready, osal::milliseconds{20}) == osal::error_code::timeout);
    CHECK(n_ready == 0U);
}