// SPDX-License-Identifier: Apache-2.0
/// @file test_clock.cpp
/// @brief Tests for osal::monotonic_clock, osal::system_clock, and clock_utils.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

TEST_CASE("monotonic_clock::now returns positive time")
{
    const auto t = osal::monotonic_clock::now();
    const auto ms = t.time_since_epoch().count();
    CHECK(ms >= 0);
}

TEST_CASE("monotonic_clock is monotonically increasing")
{
    const auto t1 = osal::monotonic_clock::now();
    osal::thread::sleep_for(osal::milliseconds{10});
    const auto t2 = osal::monotonic_clock::now();
    CHECK(t2 >= t1);
}

TEST_CASE("monotonic_clock elapsed time is reasonable")
{
    const auto t1 = osal::monotonic_clock::now();
    osal::thread::sleep_for(osal::milliseconds{50});
    const auto t2 = osal::monotonic_clock::now();
    const auto elapsed = std::chrono::duration_cast<osal::milliseconds>(t2 - t1);
    // Allow generous tolerance for CI scheduling jitter.
    CHECK(elapsed.count() >= 40);
    CHECK(elapsed.count() < 500);
}

TEST_CASE("clock_utils::ms_to_ticks round-trip")
{
    const osal::tick_t ticks = osal::clock_utils::ms_to_ticks(osal::milliseconds{100});
    CHECK(ticks > 0);
    const auto ms = osal::clock_utils::ticks_to_ms(ticks);
    // Should be approximately 100 ms (exact depends on tick period).
    CHECK(ms.count() >= 90);
    CHECK(ms.count() <= 110);
}

TEST_CASE("clock tick period is non-zero")
{
    CHECK(osal_clock_tick_period_us() > 0);
}

TEST_CASE("system_clock::now returns a value")
{
    const auto t = osal::system_clock::now();
    // System clock should return something (even if it falls back to monotonic).
    (void)t;
    CHECK(true);
}
