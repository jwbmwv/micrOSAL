// SPDX-License-Identifier: Apache-2.0
/// @file test_work_queue.cpp
/// @brief Tests for osal::work_queue.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>

// Stack for the work queue's internal worker thread.
alignas(16) static std::uint8_t wq_stack[65536];

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("work_queue: construction succeeds")
{
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 8, "test_wq"};
    CHECK(wq.valid());
}

// ---------------------------------------------------------------------------
// Submit & flush
// ---------------------------------------------------------------------------

TEST_CASE("work_queue: submit and flush single item")
{
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 8, "test_wq"};
    REQUIRE(wq.valid());

    static std::atomic<bool> executed{false};
    executed.store(false);

    auto cb = [](void*) { executed.store(true); };
    REQUIRE(wq.submit(cb).ok());
    REQUIRE(wq.flush(osal::milliseconds{2000}).ok());

    CHECK(executed.load());
}

TEST_CASE("work_queue: submit multiple items in order")
{
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 16, "test_wq"};
    REQUIRE(wq.valid());

    static std::atomic<int> counter{0};
    static int              order[4] = {};
    counter.store(0);
    for (int& value : order)
    {
        value = 0;
    }

    auto cb = [](void*)
    {
        const int idx = counter.fetch_add(1);
        if (idx < 4)
        {
            order[idx] = idx + 1;
        }
    };

    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(wq.submit(cb).ok());
    }
    REQUIRE(wq.flush(osal::milliseconds{2000}).ok());

    CHECK(counter.load() == 4);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
    CHECK(order[2] == 3);
    CHECK(order[3] == 4);
}

// ---------------------------------------------------------------------------
// Callback argument
// ---------------------------------------------------------------------------

TEST_CASE("work_queue: callback receives arg")
{
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 8, "test_wq"};
    REQUIRE(wq.valid());

    static std::atomic<std::uint32_t> value{0U};
    value.store(0U);

    auto cb = [](void* arg) { static_cast<std::atomic<std::uint32_t>*>(arg)->store(0xBEEFU); };

    REQUIRE(wq.submit(cb, &value).ok());
    REQUIRE(wq.flush(osal::milliseconds{2000}).ok());

    CHECK(value.load() == 0xBEEFU);
}

// ---------------------------------------------------------------------------
// Pending count
// ---------------------------------------------------------------------------

TEST_CASE("work_queue: pending count decreases after flush")
{
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 16, "test_wq"};
    REQUIRE(wq.valid());

    static osal::semaphore gate{osal::semaphore_type::binary, 0U};
    REQUIRE(gate.valid());

    // Submit a blocker that holds the worker thread, then submit more.
    auto blocker = [](void*) { gate.take_for(osal::milliseconds{2000}); };
    auto noop    = [](void*) {};

    REQUIRE(wq.submit(blocker).ok());
    osal::thread::sleep_for(osal::milliseconds{20});  // Let worker pick up blocker.

    REQUIRE(wq.submit(noop).ok());
    REQUIRE(wq.submit(noop).ok());
    CHECK(wq.pending() >= 2);

    // Release the blocker.
    gate.give();
    REQUIRE(wq.flush(osal::milliseconds{2000}).ok());
    CHECK(wq.pending() == 0);
}

// ---------------------------------------------------------------------------
// Cancel all
// ---------------------------------------------------------------------------

TEST_CASE("work_queue: cancel_all discards pending items")
{
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 16, "test_wq"};
    REQUIRE(wq.valid());

    static osal::semaphore gate{osal::semaphore_type::binary, 0U};
    REQUIRE(gate.valid());

    static std::atomic<int> exec_count{0};
    exec_count.store(0);

    auto blocker = [](void*) { gate.take_for(osal::milliseconds{2000}); };
    auto counter = [](void*) { exec_count.fetch_add(1); };

    // Worker picks up blocker, then we enqueue items and cancel them.
    REQUIRE(wq.submit(blocker).ok());
    osal::thread::sleep_for(osal::milliseconds{20});

    for (int i = 0; i < 5; ++i)
    {
        REQUIRE(wq.submit(counter).ok());
    }
    REQUIRE(wq.cancel_all().ok());

    gate.give();
    REQUIRE(wq.flush(osal::milliseconds{2000}).ok());

    // The 5 counter items should NOT have executed (they were cancelled).
    CHECK(exec_count.load() == 0);
}

// ---------------------------------------------------------------------------
// Overflow when queue is full
// ---------------------------------------------------------------------------

TEST_CASE("work_queue: submit returns overflow when full")
{
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 4, "test_wq"};
    REQUIRE(wq.valid());

    static osal::semaphore gate{osal::semaphore_type::binary, 0U};
    REQUIRE(gate.valid());

    auto blocker = [](void*) { gate.take_for(osal::milliseconds{2000}); };
    auto noop    = [](void*) {};

    // Fill the queue: 1 in the worker (blocker) + depth items in the ring.
    REQUIRE(wq.submit(blocker).ok());
    osal::thread::sleep_for(osal::milliseconds{20});

    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(wq.submit(noop).ok());
    }

    // Next submit should fail with overflow.
    auto r = wq.submit(noop);
    CHECK(r.code() == osal::error_code::overflow);

    // Clean up: cancel pending items so flush sentinel has room, then drain.
    (void)wq.cancel_all();
    gate.give();
    REQUIRE(wq.flush(osal::milliseconds{2000}).ok());
}

// ---------------------------------------------------------------------------
// submit_from_isr (not supported on Linux/POSIX)
// ---------------------------------------------------------------------------

TEST_CASE("work_queue: submit_from_isr returns not_supported")
{
    // Linux and POSIX do not support ISR-safe submit.
    if constexpr (osal::active_capabilities::has_native_work_queue)
    {
        MESSAGE("Skipped — backend has native work queue with ISR support");
        return;
    }

    osal::work_queue wq{wq_stack, sizeof(wq_stack), 8, "test_wq"};
    REQUIRE(wq.valid());

    auto cb = [](void*) {};
    CHECK(wq.submit_from_isr(cb).code() == osal::error_code::not_supported);
}

// ---------------------------------------------------------------------------
// Config-based construction (FLASH placement)
// ---------------------------------------------------------------------------

alignas(16) static std::uint8_t wq_cfg_stack[65536];

TEST_CASE("work_queue: config construction")
{
    const osal::work_queue_config cfg{wq_cfg_stack, sizeof(wq_cfg_stack), 8, "cfg_wq"};
    osal::work_queue              wq{cfg};
    CHECK(wq.valid());

    static volatile bool exec = false;
    exec                      = false;
    auto cb                   = [](void*) { exec = true; };
    REQUIRE(wq.submit(cb).ok());
    REQUIRE(wq.flush(osal::milliseconds{2000}).ok());
    CHECK(exec);
}
