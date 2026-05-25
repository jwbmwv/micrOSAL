// SPDX-License-Identifier: Apache-2.0
/// @file test_timer.cpp
/// @brief Tests for osal::timer.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>

static_assert(osal::timer::is_supported == osal::active_capabilities::has_timer);

static std::atomic<std::uint32_t> g_timer_count{0U};

static void timer_callback(void* /*arg*/)
{
    g_timer_count.fetch_add(1U);
}

TEST_CASE("timer: construction succeeds")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend does not support timers");
        return;
    }

    osal::timer t{timer_callback, nullptr, osal::milliseconds{100}};
    CHECK(t.valid());
}

TEST_CASE("timer: one-shot fires once")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend does not support timers");
        return;
    }

    g_timer_count.store(0U);
    osal::timer t{timer_callback, nullptr, osal::milliseconds{30}, osal::timer_mode::one_shot};
    REQUIRE(t.valid());

    REQUIRE(t.start().ok());
    osal::thread::sleep_for(osal::milliseconds{200});
    REQUIRE(t.stop().ok());

    CHECK(g_timer_count.load() == 1U);
}

TEST_CASE("timer: periodic fires multiple times")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend does not support timers");
        return;
    }

    g_timer_count.store(0U);
    osal::timer t{timer_callback, nullptr, osal::milliseconds{25}, osal::timer_mode::periodic};
    REQUIRE(t.valid());

    REQUIRE(t.start().ok());
    osal::thread::sleep_for(osal::milliseconds{200});
    REQUIRE(t.stop().ok());

    // With 25 ms period over 200 ms, expect at least 4 fires (allow scheduling slack).
    CHECK(g_timer_count.load() >= 4U);
}

TEST_CASE("timer: is_active reflects state")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend does not support timers");
        return;
    }

    g_timer_count.store(0U);
    osal::timer t{timer_callback, nullptr, osal::milliseconds{500}, osal::timer_mode::one_shot};
    REQUIRE(t.valid());

    CHECK_FALSE(t.is_active());
    REQUIRE(t.start().ok());
    CHECK(t.is_active());
    REQUIRE(t.stop().ok());
    CHECK_FALSE(t.is_active());
}

TEST_CASE("timer: set_period changes interval")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend does not support timers");
        return;
    }

    g_timer_count.store(0U);
    osal::timer t{timer_callback, nullptr, osal::milliseconds{1000}, osal::timer_mode::periodic};
    REQUIRE(t.valid());

    // Change to a shorter period before starting.
    REQUIRE(t.set_period(osal::milliseconds{25}).ok());
    REQUIRE(t.start().ok());
    osal::thread::sleep_for(osal::milliseconds{200});
    REQUIRE(t.stop().ok());

    CHECK(g_timer_count.load() >= 4U);
}

TEST_CASE("timer: callback receives arg")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend does not support timers");
        return;
    }

    std::atomic<std::uint32_t> value{0U};

    auto cb = [](void* arg) { static_cast<std::atomic<std::uint32_t>*>(arg)->store(0xCAFEU); };

    osal::timer t{cb, &value, osal::milliseconds{20}, osal::timer_mode::one_shot};
    REQUIRE(t.valid());

    REQUIRE(t.start().ok());
    osal::thread::sleep_for(osal::milliseconds{200});

    CHECK(value.load() == 0xCAFEU);
}

// ---------------------------------------------------------------------------
// Config-based construction (FLASH placement)
// ---------------------------------------------------------------------------

static std::atomic<std::uint32_t> g_config_timer_count{0U};

static void config_timer_callback(void* /*arg*/)
{
    g_config_timer_count.fetch_add(1U);
}

TEST_CASE("timer: config construction — one-shot")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend does not support timers");
        return;
    }

    g_config_timer_count.store(0U);
    const osal::timer_config cfg{config_timer_callback, nullptr, osal::milliseconds{30}, osal::timer_mode::one_shot,
                                 "cfg_os"};
    osal::timer              t{cfg};
    REQUIRE(t.valid());

    REQUIRE(t.start().ok());
    osal::thread::sleep_for(osal::milliseconds{200});
    REQUIRE(t.stop().ok());

    CHECK(g_config_timer_count.load() == 1U);
}

TEST_CASE("timer: config construction — periodic")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend does not support timers");
        return;
    }

    g_config_timer_count.store(0U);
    const osal::timer_config cfg{config_timer_callback, nullptr, osal::milliseconds{25}, osal::timer_mode::periodic,
                                 "cfg_per"};
    osal::timer              t{cfg};
    REQUIRE(t.valid());

    REQUIRE(t.start().ok());
    osal::thread::sleep_for(osal::milliseconds{200});
    REQUIRE(t.stop().ok());

    CHECK(g_config_timer_count.load() >= 4U);
}
