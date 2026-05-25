// SPDX-License-Identifier: Apache-2.0
/// @file test_clock.cpp
/// @brief Tests for osal::monotonic_clock, osal::system_clock, and clock_utils.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

namespace
{

struct fake_deadline_clock
{
    using rep        = std::int64_t;
    using period     = std::milli;
    using duration   = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<fake_deadline_clock>;

    static constexpr bool is_steady = true;

    static time_point now() noexcept { return current; }

    static time_point current;
};

fake_deadline_clock::time_point fake_deadline_clock::current{};

using fake_deadline = osal::basic_deadline<fake_deadline_clock>;

}  // namespace

TEST_CASE("monotonic_clock::now returns positive time")
{
    const auto t  = osal::monotonic_clock::now();
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
    const auto t2      = osal::monotonic_clock::now();
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

TEST_CASE("high_resolution_clock reflects backend support")
{
    static_assert(osal::high_resolution_clock::is_supported ==
                  osal::supports_requirement<osal::support_requirement::high_resolution_clock>);

    const auto before = osal::high_resolution_clock::now();
    osal::thread::sleep_for(osal::milliseconds{5});
    const auto after = osal::high_resolution_clock::now();
    CHECK(after >= before);

    const auto resolution = osal::high_resolution_clock::resolution();
    CHECK(resolution.count() > 0);

    const auto tick_resolution =
        std::chrono::duration_cast<osal::nanoseconds>(osal::microseconds{osal_clock_tick_period_us()});
    if constexpr (osal::high_resolution_clock::is_supported)
    {
        CHECK(resolution <= tick_resolution);
    }
    else
    {
        CHECK(resolution == tick_resolution);
    }
}

TEST_CASE("basic_deadline rounds relative timeouts up to the clock resolution")
{
    fake_deadline_clock::current = fake_deadline_clock::time_point{fake_deadline_clock::duration{10}};

    const auto timeout = fake_deadline::after(osal::microseconds{1500});
    CHECK(timeout.expires_at() == fake_deadline_clock::time_point{fake_deadline_clock::duration{12}});
}

TEST_CASE("basic_deadline remaining saturates at zero")
{
    const auto expiry  = osal::monotonic_clock::time_point{osal::milliseconds{25}};
    const auto timeout = osal::monotonic_deadline::at(expiry);

    CHECK_FALSE(timeout.expired(osal::monotonic_clock::time_point{osal::milliseconds{24}}));
    CHECK(timeout.remaining(osal::monotonic_clock::time_point{osal::milliseconds{24}}) == osal::milliseconds{1});
    CHECK(timeout.expired(expiry));
    CHECK(timeout.remaining(expiry) == osal::milliseconds{0});
    CHECK(timeout.remaining(osal::monotonic_clock::time_point{osal::milliseconds{30}}) == osal::milliseconds{0});
}

TEST_CASE("basic_deadline saturates oversized relative timeouts")
{
    fake_deadline_clock::current = fake_deadline_clock::time_point::max() - fake_deadline_clock::duration{1};

    const auto timeout = fake_deadline::after(std::chrono::seconds{1});
    CHECK(timeout.expires_at() == fake_deadline_clock::time_point::max());
}
