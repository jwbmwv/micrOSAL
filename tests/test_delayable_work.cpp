// SPDX-License-Identifier: Apache-2.0
/// @file test_delayable_work.cpp
/// @brief Tests for osal::delayable_work.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

#include <atomic>

alignas(16) static std::uint8_t dw_stack[65536];

TEST_CASE("delayable_work: construction succeeds")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped - backend lacks timers");
        return;
    }

    osal::work_queue    wq{dw_stack, sizeof(dw_stack), 8, "dw_wq"};
    osal::delayable_work work{wq, +[](void*) {}, nullptr, "dw"};
    REQUIRE(wq.valid());
    CHECK(work.valid());
    CHECK_FALSE(work.pending());
}


TEST_CASE("delayable_work: schedule and flush executes callback")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped - backend lacks timers");
        return;
    }

    osal::work_queue wq{dw_stack, sizeof(dw_stack), 8, "dw_wq"};
    REQUIRE(wq.valid());

    static std::atomic<int> count{0};
    count.store(0);

    osal::delayable_work work{wq, +[](void*) { count.fetch_add(1); }, nullptr, "dw"};
    REQUIRE(work.valid());

    const auto before = osal::monotonic_clock::now();
    REQUIRE(work.schedule(osal::milliseconds{25}).ok());
    REQUIRE(work.flush(osal::milliseconds{2000}).ok());
    const auto after = osal::monotonic_clock::now();

    CHECK(count.load() == 1);
    CHECK(std::chrono::duration_cast<osal::milliseconds>(after - before).count() >= 10);
    CHECK_FALSE(work.pending());
}

TEST_CASE("delayable_work: reschedule updates an armed item")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped - backend lacks timers");
        return;
    }

    osal::work_queue wq{dw_stack, sizeof(dw_stack), 8, "dw_wq"};
    REQUIRE(wq.valid());

    static std::atomic<int> count{0};
    count.store(0);

    osal::delayable_work work{wq, +[](void*) { count.fetch_add(1); }, nullptr, "dw"};
    REQUIRE(work.valid());

    REQUIRE(work.schedule(osal::milliseconds{100}).ok());
    osal::thread::sleep_for(osal::milliseconds{20});
    REQUIRE(work.reschedule(osal::milliseconds{20}).ok());
    REQUIRE(work.flush(osal::milliseconds{2000}).ok());

    CHECK(count.load() == 1);
}

TEST_CASE("delayable_work: cancel prevents execution while armed")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped - backend lacks timers");
        return;
    }

    osal::work_queue wq{dw_stack, sizeof(dw_stack), 8, "dw_wq"};
    REQUIRE(wq.valid());

    static std::atomic<int> count{0};
    count.store(0);

    osal::delayable_work work{wq, +[](void*) { count.fetch_add(1); }, nullptr, "dw"};
    REQUIRE(work.valid());

    REQUIRE(work.schedule(osal::milliseconds{100}).ok());
    REQUIRE(work.cancel().ok());
    osal::thread::sleep_for(osal::milliseconds{150});
    CHECK(count.load() == 0);
    CHECK_FALSE(work.pending());
}

TEST_CASE("delayable_work: reschedule while queued or running reports would_block")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped - backend lacks timers");
        return;
    }

    osal::work_queue wq{dw_stack, sizeof(dw_stack), 8, "dw_wq"};
    REQUIRE(wq.valid());

    static osal::semaphore gate{osal::semaphore_type::binary, 0U};
    REQUIRE(gate.valid());

    osal::delayable_work work{wq, +[](void*) { gate.take_for(osal::milliseconds{2000}); }, nullptr, "dw"};
    REQUIRE(work.valid());

    REQUIRE(work.schedule(osal::milliseconds{0}).ok());
    osal::thread::sleep_for(osal::milliseconds{20});
    CHECK(work.reschedule(osal::milliseconds{10}) == osal::error_code::would_block);

    gate.give();
    REQUIRE(work.flush(osal::milliseconds{2000}).ok());
}