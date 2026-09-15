// SPDX-License-Identifier: Apache-2.0
/// @file test_object_wait_set.cpp
/// @brief Tests for osal::object_wait_set.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
namespace
{

template<std::size_t Capacity>
concept value_wait_capacity =
    requires(osal::object_wait_set& wait_set) { wait_set.template wait_expected<Capacity>(); };

static_assert(!value_wait_capacity<0U>);
static_assert(value_wait_capacity<1U>);

}  // namespace
#endif

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

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
TEST_CASE("object_wait_set: wait_expected returns expected vector")
{
    osal::object_wait_set         ws;
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());
    REQUIRE(ws.add(q, 71).ok());
    REQUIRE(q.send(42U).ok());

    auto exp_ready = ws.wait_expected<4>(osal::milliseconds{20});
    REQUIRE(exp_ready.has_value());
    REQUIRE(exp_ready->size() == 1U);
    CHECK((*exp_ready)[0] == 71);
    CHECK(q.count() == 1U);
}

TEST_CASE("object_wait_set: wait_expected preserves poll and timed timeout errors")
{
    osal::object_wait_set         wait_set;
    osal::queue<std::uint32_t, 1> queue;
    REQUIRE(queue.valid());
    REQUIRE(wait_set.add(queue, 11).ok());

    const auto polled = wait_set.wait_expected<1>(osal::milliseconds{0});
    REQUIRE_FALSE(polled.has_value());
    CHECK(polled.error() == osal::error_code::timeout);

    const auto timed = wait_set.wait_expected<1>(osal::milliseconds{1});
    REQUIRE_FALSE(timed.has_value());
    CHECK(timed.error() == osal::error_code::timeout);
}

TEST_CASE("object_wait_set: wait_expected fills capacity and truncates ready ids")
{
    osal::object_wait_set         wait_set;
    osal::queue<std::uint32_t, 1> queue;
    REQUIRE(queue.valid());
    for (std::size_t index = 0U; index < OSAL_OBJECT_WAIT_SET_MAX_ENTRIES; ++index)
    {
        REQUIRE(wait_set.add(queue, static_cast<int>(index)).ok());
    }
    REQUIRE(queue.try_send(42U));

    const auto all_ready = wait_set.wait_expected<>(osal::milliseconds{0});
    REQUIRE(all_ready.has_value());
    REQUIRE(all_ready->size() == OSAL_OBJECT_WAIT_SET_MAX_ENTRIES);
    CHECK(all_ready->full());
    for (std::size_t index = 0U; index < all_ready->size(); ++index)
    {
        CHECK((*all_ready)[index] == static_cast<int>(index));
    }

    const auto truncated = wait_set.wait_expected<1>(osal::milliseconds{0});
    REQUIRE(truncated.has_value());
    REQUIRE(truncated->size() == 1U);
    CHECK(truncated->full());
    CHECK((*truncated)[0] == 0);
    CHECK(queue.count() == 1U);
}

TEST_CASE("object_wait_set: wait_expected skips removed and nonready registrations")
{
    osal::object_wait_set         wait_set;
    osal::queue<std::uint32_t, 1> ready_queue;
    osal::queue<std::uint32_t, 1> empty_queue;
    REQUIRE(ready_queue.valid());
    REQUIRE(empty_queue.valid());
    REQUIRE(wait_set.add(ready_queue, 10).ok());
    REQUIRE(wait_set.add(ready_queue, 20).ok());
    REQUIRE(wait_set.remove(10).ok());
    REQUIRE(ready_queue.try_send(42U));

    const auto sparse = wait_set.wait_expected<2>(osal::milliseconds{0});
    REQUIRE(sparse.has_value());
    REQUIRE(sparse->size() == 1U);
    CHECK((*sparse)[0] == 20);

    REQUIRE(wait_set.add(empty_queue, 30).ok());
    const auto reused = wait_set.wait_expected<2>(osal::milliseconds{0});
    REQUIRE(reused.has_value());
    REQUIRE(reused->size() == 1U);
    CHECK((*reused)[0] == 20);
}

TEST_CASE("object_wait_set: wait_expected retains clear-on-exit effects when output is truncated")
{
    osal::object_wait_set wait_set;
    osal::notification<2> notification;
    REQUIRE(notification.valid());
    REQUIRE(wait_set.add(notification, 0U, 10, true).ok());
    REQUIRE(wait_set.add(notification, 1U, 20, true).ok());
    REQUIRE(notification.notify(1U, osal::notification_action::overwrite, 0U).ok());
    REQUIRE(notification.notify(2U, osal::notification_action::overwrite, 1U).ok());

    const auto ready = wait_set.wait_expected<1>(osal::milliseconds{0});
    REQUIRE(ready.has_value());
    REQUIRE(ready->size() == 1U);
    CHECK((*ready)[0] == 10);
    CHECK_FALSE(notification.pending(0U));
    CHECK_FALSE(notification.pending(1U));
}
#endif
