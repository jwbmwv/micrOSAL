// SPDX-License-Identifier: Apache-2.0
/// @file test_barrier.cpp
/// @brief Tests for osal::barrier.
///
/// All current MicrOSAL backends expose barriers.
///
/// POSIX-family backends use native pthread barriers; the remaining backends
/// use the shared emulated barrier built from mutex + condvar.

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
    CHECK(b.valid());
}

TEST_CASE("barrier: construction with count 0 is invalid")
{
    osal::barrier b{0U};
    CHECK_FALSE(b.valid());
}

// ---------------------------------------------------------------------------
// Native path: single thread — count 1 unblocks the sole caller immediately
// ---------------------------------------------------------------------------

TEST_CASE("barrier: count-1 releases immediately")
{
    osal::barrier b{1U};
    REQUIRE(b.valid());
    const osal::result r = b.wait();
    CHECK((r.ok() || r == osal::error_code::barrier_serial));
}

// ---------------------------------------------------------------------------
// Native path: two-thread rendezvous
// ---------------------------------------------------------------------------

TEST_CASE("barrier: two-thread rendezvous")
{
    osal::barrier    barr{2U};
    std::atomic<int> passed{0};
    struct ctx_t
    {
        osal::barrier*    barrier;
        std::atomic<int>* passed;
    } ctx{&barr, &passed};

    REQUIRE(barr.valid());

    auto worker = [](void* arg)
    {
        auto*              ctx = static_cast<ctx_t*>(arg);
        const osal::result r   = ctx->barrier->wait();
        if (r.ok() || (r == osal::error_code::barrier_serial))
        {
            ctx->passed->fetch_add(1);
        }
    };

    alignas(16) static std::uint8_t stack_a[65536];
    alignas(16) static std::uint8_t stack_b[65536];

    osal::thread        ta, tb;
    osal::thread_config ca{};
    ca.entry               = worker;
    ca.arg                 = &ctx;
    ca.stack               = stack_a;
    ca.stack_bytes         = sizeof(stack_a);
    ca.name                = "ba";
    osal::thread_config cb = ca;
    cb.stack               = stack_b;
    cb.name                = "bb";

    REQUIRE(ta.create(ca).ok());
    REQUIRE(tb.create(cb).ok());

    ta.join();
    tb.join();

    CHECK(passed.load() == 2);
}

// ---------------------------------------------------------------------------
// Native path: serial-thread identification
// ---------------------------------------------------------------------------

TEST_CASE("barrier: exactly one thread receives barrier_serial")
{
    osal::barrier    barr{3U};
    std::atomic<int> serial_count{0};
    struct ctx_t
    {
        osal::barrier*    barrier;
        std::atomic<int>* serial_count;
    } ctx{&barr, &serial_count};

    REQUIRE(barr.valid());

    auto worker = [](void* arg)
    {
        auto*              ctx = static_cast<ctx_t*>(arg);
        const osal::result r   = ctx->barrier->wait();
        if (r == osal::error_code::barrier_serial)
        {
            ctx->serial_count->fetch_add(1);
        }
    };

    alignas(16) static std::uint8_t stacks[3][65536];
    osal::thread                    threads[3];
    for (int i = 0; i < 3; ++i)
    {
        osal::thread_config cfg{};
        cfg.entry       = worker;
        cfg.arg         = &ctx;
        cfg.stack       = stacks[i];
        cfg.stack_bytes = sizeof(stacks[i]);
        cfg.name        = "bserial";
        REQUIRE(threads[i].create(cfg).ok());
    }
    for (auto& t : threads)
    {
        t.join();
    }

    CHECK(serial_count.load() == 1);
}
