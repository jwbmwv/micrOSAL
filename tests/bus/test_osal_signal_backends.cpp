// SPDX-License-Identifier: Apache-2.0
/// @file test_osal_signal_backends.cpp
/// @brief Compilation and functional tests for all delegated channel backends
/// @details Verifies that every bus_backend_* tag compiles as a fully
///          functional osal_bus + osal_signal + osal_signal_premium, and
///          that the delegated backends behave identically to
///          bus_backend_generic.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <microsal/bus/osal_signal_premium.hpp>

#include <cstdint>

// ---------------------------------------------------------------------------
// Helper: exercises the full LCD + Premium API on a given backend tag
// ---------------------------------------------------------------------------

template<typename BackendTag>
static void exercise_lcd_api()
{
    osal::osal_signal<std::uint32_t, 4U, 8U, BackendTag> topic;

    // subscribe
    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));
    CHECK(id != osal::invalid_subscriber_id);
    CHECK(topic.subscriber_count() == 1U);

    // publish + receive
    REQUIRE(topic.publish(42U));
    std::uint32_t val{0U};
    REQUIRE(topic.try_receive(id, val));
    CHECK(val == 42U);

    // receive from empty queue
    CHECK_FALSE(topic.try_receive(id, val));

    // unsubscribe
    REQUIRE(topic.unsubscribe(id));
    CHECK(topic.subscriber_count() == 0U);

    // static dimensions
    CHECK(topic.max_subscribers() == 4U);
    CHECK(topic.per_subscriber_capacity() == 8U);
}

template<typename BackendTag>
static void exercise_channel_api()
{
    osal::osal_bus<std::uint32_t, 8U, BackendTag> ch;
    CHECK(ch.valid());

    CHECK(ch.try_send(100U));
    std::uint32_t v{0U};
    CHECK(ch.try_receive(v));
    CHECK(v == 100U);

    CHECK_FALSE(ch.try_receive(v));
}

template<typename BackendTag>
static void exercise_premium_api()
{
    osal::osal_signal_premium<std::uint32_t, 4U, 8U, BackendTag> topic;

    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));

    REQUIRE(topic.publish(77U));
    std::uint32_t val{0U};
    REQUIRE(topic.try_receive(id, val));
    CHECK(val == 77U);

    // publish_zero_copy
    std::uint32_t msg = 99U;
    REQUIRE(topic.publish_zero_copy(&msg));
    REQUIRE(topic.try_receive(id, val));
    CHECK(val == 99U);

    // publish_zero_copy nullptr guard
    CHECK_FALSE(topic.publish_zero_copy(nullptr));

    // route_to stub
    CHECK_FALSE(topic.route_to(osal::invalid_signal_id, 1U));

    (void)topic.unsubscribe(id);
}

// ---------------------------------------------------------------------------
// Capability trait compile-time checks
// ---------------------------------------------------------------------------

template<typename BackendTag>
static void check_lcd_capabilities()
{
    static_assert(!osal::osal_signal_capabilities<BackendTag>::native_pubsub,
                  "delegated backend should not claim native pubsub");
    static_assert(!osal::osal_signal_capabilities<BackendTag>::native_observers,
                  "delegated backend should not claim native observers");
    static_assert(!osal::osal_signal_capabilities<BackendTag>::native_routing,
                  "delegated backend should not claim native routing");
    static_assert(!osal::osal_signal_capabilities<BackendTag>::zero_copy,
                  "delegated backend should not claim zero_copy");
    static_assert(osal::bus_backend_tag<BackendTag>,
                  "backend must satisfy bus_backend_tag concept");
}

// ===========================================================================
// Per-backend test cases
// ===========================================================================

// --- FreeRTOS ---
TEST_CASE("bus_backend_freertos: LCD API")     { exercise_lcd_api<osal::bus_backend_freertos>(); }
TEST_CASE("bus_backend_freertos: channel API")  { exercise_channel_api<osal::bus_backend_freertos>(); }
TEST_CASE("bus_backend_freertos: premium API")  { exercise_premium_api<osal::bus_backend_freertos>(); }
TEST_CASE("bus_backend_freertos: capabilities") { check_lcd_capabilities<osal::bus_backend_freertos>(); }

// --- POSIX ---
TEST_CASE("bus_backend_posix: LCD API")         { exercise_lcd_api<osal::bus_backend_posix>(); }
TEST_CASE("bus_backend_posix: channel API")     { exercise_channel_api<osal::bus_backend_posix>(); }
TEST_CASE("bus_backend_posix: premium API")     { exercise_premium_api<osal::bus_backend_posix>(); }
TEST_CASE("bus_backend_posix: capabilities")    { check_lcd_capabilities<osal::bus_backend_posix>(); }

// --- Bare Metal ---
TEST_CASE("bus_backend_bare_metal: LCD API")        { exercise_lcd_api<osal::bus_backend_bare_metal>(); }
TEST_CASE("bus_backend_bare_metal: channel API")    { exercise_channel_api<osal::bus_backend_bare_metal>(); }
TEST_CASE("bus_backend_bare_metal: premium API")    { exercise_premium_api<osal::bus_backend_bare_metal>(); }
TEST_CASE("bus_backend_bare_metal: capabilities")   { check_lcd_capabilities<osal::bus_backend_bare_metal>(); }

// --- ThreadX ---
TEST_CASE("bus_backend_threadx: LCD API")       { exercise_lcd_api<osal::bus_backend_threadx>(); }
TEST_CASE("bus_backend_threadx: channel API")   { exercise_channel_api<osal::bus_backend_threadx>(); }
TEST_CASE("bus_backend_threadx: premium API")   { exercise_premium_api<osal::bus_backend_threadx>(); }
TEST_CASE("bus_backend_threadx: capabilities")  { check_lcd_capabilities<osal::bus_backend_threadx>(); }

// --- PX5 ---
TEST_CASE("bus_backend_px5: LCD API")           { exercise_lcd_api<osal::bus_backend_px5>(); }
TEST_CASE("bus_backend_px5: channel API")       { exercise_channel_api<osal::bus_backend_px5>(); }
TEST_CASE("bus_backend_px5: premium API")       { exercise_premium_api<osal::bus_backend_px5>(); }
TEST_CASE("bus_backend_px5: capabilities")      { check_lcd_capabilities<osal::bus_backend_px5>(); }

// --- Linux ---
TEST_CASE("bus_backend_linux: LCD API")         { exercise_lcd_api<osal::bus_backend_linux>(); }
TEST_CASE("bus_backend_linux: channel API")     { exercise_channel_api<osal::bus_backend_linux>(); }
TEST_CASE("bus_backend_linux: premium API")     { exercise_premium_api<osal::bus_backend_linux>(); }
TEST_CASE("bus_backend_linux: capabilities")    { check_lcd_capabilities<osal::bus_backend_linux>(); }

// --- VxWorks ---
TEST_CASE("bus_backend_vxworks: LCD API")       { exercise_lcd_api<osal::bus_backend_vxworks>(); }
TEST_CASE("bus_backend_vxworks: channel API")   { exercise_channel_api<osal::bus_backend_vxworks>(); }
TEST_CASE("bus_backend_vxworks: premium API")   { exercise_premium_api<osal::bus_backend_vxworks>(); }
TEST_CASE("bus_backend_vxworks: capabilities")  { check_lcd_capabilities<osal::bus_backend_vxworks>(); }

// --- NuttX ---
TEST_CASE("bus_backend_nuttx: LCD API")         { exercise_lcd_api<osal::bus_backend_nuttx>(); }
TEST_CASE("bus_backend_nuttx: channel API")     { exercise_channel_api<osal::bus_backend_nuttx>(); }
TEST_CASE("bus_backend_nuttx: premium API")     { exercise_premium_api<osal::bus_backend_nuttx>(); }
TEST_CASE("bus_backend_nuttx: capabilities")    { check_lcd_capabilities<osal::bus_backend_nuttx>(); }

// --- Micrium ---
TEST_CASE("bus_backend_micrium: LCD API")       { exercise_lcd_api<osal::bus_backend_micrium>(); }
TEST_CASE("bus_backend_micrium: channel API")   { exercise_channel_api<osal::bus_backend_micrium>(); }
TEST_CASE("bus_backend_micrium: premium API")   { exercise_premium_api<osal::bus_backend_micrium>(); }
TEST_CASE("bus_backend_micrium: capabilities")  { check_lcd_capabilities<osal::bus_backend_micrium>(); }

// --- ChibiOS ---
TEST_CASE("bus_backend_chibios: LCD API")       { exercise_lcd_api<osal::bus_backend_chibios>(); }
TEST_CASE("bus_backend_chibios: channel API")   { exercise_channel_api<osal::bus_backend_chibios>(); }
TEST_CASE("bus_backend_chibios: premium API")   { exercise_premium_api<osal::bus_backend_chibios>(); }
TEST_CASE("bus_backend_chibios: capabilities")  { check_lcd_capabilities<osal::bus_backend_chibios>(); }

// --- embOS ---
TEST_CASE("bus_backend_embos: LCD API")         { exercise_lcd_api<osal::bus_backend_embos>(); }
TEST_CASE("bus_backend_embos: channel API")     { exercise_channel_api<osal::bus_backend_embos>(); }
TEST_CASE("bus_backend_embos: premium API")     { exercise_premium_api<osal::bus_backend_embos>(); }
TEST_CASE("bus_backend_embos: capabilities")    { check_lcd_capabilities<osal::bus_backend_embos>(); }

// --- QNX ---
TEST_CASE("bus_backend_qnx: LCD API")           { exercise_lcd_api<osal::bus_backend_qnx>(); }
TEST_CASE("bus_backend_qnx: channel API")       { exercise_channel_api<osal::bus_backend_qnx>(); }
TEST_CASE("bus_backend_qnx: premium API")       { exercise_premium_api<osal::bus_backend_qnx>(); }
TEST_CASE("bus_backend_qnx: capabilities")      { check_lcd_capabilities<osal::bus_backend_qnx>(); }

// --- RTEMS ---
TEST_CASE("bus_backend_rtems: LCD API")         { exercise_lcd_api<osal::bus_backend_rtems>(); }
TEST_CASE("bus_backend_rtems: channel API")     { exercise_channel_api<osal::bus_backend_rtems>(); }
TEST_CASE("bus_backend_rtems: premium API")     { exercise_premium_api<osal::bus_backend_rtems>(); }
TEST_CASE("bus_backend_rtems: capabilities")    { check_lcd_capabilities<osal::bus_backend_rtems>(); }

// --- INTEGRITY ---
TEST_CASE("bus_backend_integrity: LCD API")         { exercise_lcd_api<osal::bus_backend_integrity>(); }
TEST_CASE("bus_backend_integrity: channel API")     { exercise_channel_api<osal::bus_backend_integrity>(); }
TEST_CASE("bus_backend_integrity: premium API")     { exercise_premium_api<osal::bus_backend_integrity>(); }
TEST_CASE("bus_backend_integrity: capabilities")    { check_lcd_capabilities<osal::bus_backend_integrity>(); }

// --- CMSIS-RTOS v1 ---
TEST_CASE("bus_backend_cmsis_rtos: LCD API")        { exercise_lcd_api<osal::bus_backend_cmsis_rtos>(); }
TEST_CASE("bus_backend_cmsis_rtos: channel API")    { exercise_channel_api<osal::bus_backend_cmsis_rtos>(); }
TEST_CASE("bus_backend_cmsis_rtos: premium API")    { exercise_premium_api<osal::bus_backend_cmsis_rtos>(); }
TEST_CASE("bus_backend_cmsis_rtos: capabilities")   { check_lcd_capabilities<osal::bus_backend_cmsis_rtos>(); }

// --- CMSIS-RTOS2 v2 ---
TEST_CASE("bus_backend_cmsis_rtos2: LCD API")       { exercise_lcd_api<osal::bus_backend_cmsis_rtos2>(); }
TEST_CASE("bus_backend_cmsis_rtos2: channel API")   { exercise_channel_api<osal::bus_backend_cmsis_rtos2>(); }
TEST_CASE("bus_backend_cmsis_rtos2: premium API")   { exercise_premium_api<osal::bus_backend_cmsis_rtos2>(); }
TEST_CASE("bus_backend_cmsis_rtos2: capabilities")  { check_lcd_capabilities<osal::bus_backend_cmsis_rtos2>(); }

// ===========================================================================
// Cross-backend: default tag resolves for the current build
// ===========================================================================

TEST_CASE("default backend tag: LCD API")
{
    // Uses MICROSAL_DEFAULT_BACKEND_TAG implicitly
    osal::osal_signal<std::uint32_t, 4U, 8U> topic;

    osal::subscriber_id id{osal::invalid_subscriber_id};
    REQUIRE(topic.subscribe(id));
    REQUIRE(topic.publish(55U));

    std::uint32_t val{0U};
    REQUIRE(topic.try_receive(id, val));
    CHECK(val == 55U);

    (void)topic.unsubscribe(id);
}
