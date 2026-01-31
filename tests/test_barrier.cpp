// SPDX-License-Identifier: Apache-2.0
/// @file test_barrier.cpp
/// @brief Tests for osal::barrier.
///
/// On backends where has_barrier == false (FreeRTOS, Zephyr, ThreadX, …)
/// every operation returns error_code::not_supported.
///
/// On backends where has_barrier == true (POSIX, Linux, NuttX, QNX) the
/// pthread_barrier_t native primitive is exercised via cross-thread tests.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("barrier: construction with count 1")
{
    osal::barrier b{1U};
    if constexpr (osal::active_capabilities::has_barrier)
    {
        CHECK(b.valid());
    }
    else
    {
        CHECK_FALSE(b.valid());
    }
}

// ---------------------------------------------------------------------------
// Stub path: all ops return not_supported
// ---------------------------------------------------------------------------

TEST_CASE("barrier: not_supported on stub backends")
{
    if constexpr (!osal::active_capabilities::has_barrier)
    {
        osal::barrier b{2U};
        CHECK(b.wait() == osal::error_code::not_supported);
    }
}

// ---------------------------------------------------------------------------
// Native path: single thread — count 1 unblocks the sole caller immediately
// ---------------------------------------------------------------------------

TEST_CASE("barrier: count-1 releases immediately (native)")
{
    if constexpr (osal::active_capabilities::has_barrier)
    {
        osal::barrier b{1U};
        REQUIRE(b.valid());
        // The single waiter is the "serial" thread.
        const osal::result r = b.wait();
        CHECK((r.ok() || r == osal::error_code::barrier_serial));
    }
}

// ---------------------------------------------------------------------------
// Native path: two-thread rendezvous
// ---------------------------------------------------------------------------

TEST_CASE("barrier: two-thread rendezvous (native)")
{
    if constexpr (osal::active_capabilities::has_barrier)
    {
        static osal::barrier barr{2U};
        static std::atomic<int> passed{0};

        REQUIRE(barr.valid());
        passed.store(0);

        struct Ctx { };
        static Ctx ctx{};

        auto worker = [](void*) {
            barr.wait();
            passed.fetch_add(1);
        };

        alignas(16) static std::uint8_t stack_a[65536];
        alignas(16) static std::uint8_t stack_b[65536];

        osal::thread ta, tb;
        osal::thread_config ca{};
        ca.entry       = worker;
        ca.arg         = &ctx;
        ca.stack       = stack_a;
        ca.stack_bytes = sizeof(stack_a);
        ca.name        = "ba";
        osal::thread_config cb = ca;
        cb.stack       = stack_b;
        cb.name        = "bb";

        REQUIRE(ta.create(ca).ok());
        REQUIRE(tb.create(cb).ok());

        ta.join();
        tb.join();

        // Both threads must have passed the barrier.
        CHECK(passed.load() == 2);
    }
}

// ---------------------------------------------------------------------------
// Native path: serial-thread identification
// ---------------------------------------------------------------------------

TEST_CASE("barrier: exactly one thread receives barrier_serial (native)")
{
    if constexpr (osal::active_capabilities::has_barrier)
    {
        static osal::barrier barr{3U};
        REQUIRE(barr.valid());

        static std::atomic<int> serial_count{0};
        serial_count.store(0);

        auto worker = [](void*) {
            const osal::result r = barr.wait();
            if (r == osal::error_code::barrier_serial)
            {
                serial_count.fetch_add(1);
            }
        };

        alignas(16) static std::uint8_t stacks[3][65536];
        osal::thread threads[3];
        for (int i = 0; i < 3; ++i)
        {
            osal::thread_config cfg{};
            cfg.entry       = worker;
            cfg.arg         = nullptr;
            cfg.stack       = stacks[i];
            cfg.stack_bytes = sizeof(stacks[i]);
            cfg.name        = "bserial";
            REQUIRE(threads[i].create(cfg).ok());
        }
        for (auto& t : threads)
        {
            t.join();
        }

        // Exactly one thread must have been the serial thread.
        CHECK(serial_count.load() == 1);
    }
}
