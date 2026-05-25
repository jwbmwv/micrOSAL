// SPDX-License-Identifier: Apache-2.0
/// @file test_work_queue.cpp
/// @brief Tests for osal::work_queue.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>

#if defined(OSAL_BACKEND_BAREMETAL) || defined(OSAL_BACKEND_CHIBIOS) || defined(OSAL_BACKEND_CMSIS_RTOS) || \
    defined(OSAL_BACKEND_CMSIS_RTOS2) || defined(OSAL_BACKEND_EMBOS) || defined(OSAL_BACKEND_FREERTOS) ||   \
    defined(OSAL_BACKEND_MICRIUM) || defined(OSAL_BACKEND_NUTTX) || defined(OSAL_BACKEND_PX5) ||            \
    defined(OSAL_BACKEND_QNX) || defined(OSAL_BACKEND_THREADX) || defined(OSAL_BACKEND_VXWORKS)
#define OSAL_TEST_SHARED_EMULATED_WORK_QUEUE 1
#else
#define OSAL_TEST_SHARED_EMULATED_WORK_QUEUE 0
#endif

// Stack for the work queue's internal worker thread.
alignas(16) static std::uint8_t wq_stack[65536];
alignas(16) static std::uint8_t wq_flush_stack1[65536];
alignas(16) static std::uint8_t wq_flush_stack2[65536];

static osal::thread_config make_wq_cfg(void (*entry)(void*), void* arg, void* stack, std::size_t stack_sz,
                                       const char* name)
{
    osal::thread_config cfg{};
    cfg.entry       = entry;
    cfg.arg         = arg;
    cfg.stack       = stack;
    cfg.stack_bytes = stack_sz;
    cfg.name        = name;
    return cfg;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("work_queue: construction succeeds")
{
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 8, "test_wq"};
    CHECK(wq.valid());
}

TEST_CASE("work_queue: zero-depth construction fails")
{
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 0, "test_wq"};
    CHECK_FALSE(wq.valid());
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

TEST_CASE("work_queue: concurrent flush callers wait for in-flight work")
{
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 8, "test_wq"};
    REQUIRE(wq.valid());

    static osal::semaphore gate{osal::semaphore_type::binary, 0U};
    REQUIRE(gate.valid());
    while (gate.try_take())
    {
    }

    static std::atomic<osal::work_queue*> active_wq{nullptr};
    active_wq.store(&wq, std::memory_order_release);

    static std::atomic<int> flush1_code{-1};
    static std::atomic<int> flush2_code{-1};

    flush1_code.store(-1);
    flush2_code.store(-1);

    auto blocker   = [](void*) { gate.take_for(osal::milliseconds{2000}); };
    auto flush1_fn = [](void*)
    {
        auto* queue = active_wq.load(std::memory_order_acquire);
        if (queue != nullptr)
        {
            flush1_code.store(static_cast<int>(queue->flush(osal::milliseconds{2000}).code()));
        }
    };
    auto flush2_fn = [](void*)
    {
        auto* queue = active_wq.load(std::memory_order_acquire);
        if (queue != nullptr)
        {
            flush2_code.store(static_cast<int>(queue->flush(osal::milliseconds{2000}).code()));
        }
    };

    REQUIRE(wq.submit(blocker).ok());
    osal::thread::sleep_for(osal::milliseconds{20});

    osal::thread flush1_thread;
    REQUIRE(flush1_thread.create(make_wq_cfg(flush1_fn, nullptr, wq_flush_stack1, sizeof(wq_flush_stack1), "wq_flush1"))
                .ok());

    osal::thread flush2_thread;
    REQUIRE(flush2_thread.create(make_wq_cfg(flush2_fn, nullptr, wq_flush_stack2, sizeof(wq_flush_stack2), "wq_flush2"))
                .ok());

    gate.give();

    REQUIRE(flush1_thread.join().ok());
    REQUIRE(flush2_thread.join().ok());
    active_wq.store(nullptr, std::memory_order_release);

    CHECK(flush1_code.load() == static_cast<int>(osal::error_code::ok));
    CHECK(flush2_code.load() == static_cast<int>(osal::error_code::ok));
}

TEST_CASE("work_queue: flush succeeds while queue is full")
{
#if !OSAL_TEST_SHARED_EMULATED_WORK_QUEUE
    MESSAGE("Skipped — backend does not use the shared emulated work queue");
#else
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 4, "test_wq"};
    REQUIRE(wq.valid());

    static osal::semaphore gate{osal::semaphore_type::binary, 0U};
    REQUIRE(gate.valid());

    static std::atomic<int> exec_count{0};
    exec_count.store(0, std::memory_order_relaxed);

    auto blocker  = [](void*) { gate.take_for(osal::milliseconds{2000}); };
    auto counter  = [](void*) { (void)exec_count.fetch_add(1, std::memory_order_relaxed); };
    auto releaser = [](void*)
    {
        osal::thread::sleep_for(osal::milliseconds{20});
        gate.give();
    };

    REQUIRE(wq.submit(blocker).ok());
    osal::thread::sleep_for(osal::milliseconds{20});
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(wq.submit(counter).ok());
    }

    alignas(16) static std::uint8_t release_stack[65536];
    osal::thread                    release_thread;
    REQUIRE(
        release_thread.create(make_wq_cfg(releaser, nullptr, release_stack, sizeof(release_stack), "wq_release")).ok());

    CHECK(wq.pending() == 4U);
    const auto flush_result = wq.flush(osal::milliseconds{2000});
    CAPTURE(static_cast<int>(flush_result.code()));
    REQUIRE(flush_result.ok());
    REQUIRE(release_thread.join().ok());
    CHECK(exec_count.load(std::memory_order_relaxed) == 4);
    CHECK(wq.pending() == 0U);
#endif
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

TEST_CASE("work_queue: cancel_all does not satisfy an earlier flush")
{
#if !OSAL_TEST_SHARED_EMULATED_WORK_QUEUE
    MESSAGE("Skipped — backend does not use the shared emulated work queue");
#else
    osal::work_queue wq{wq_stack, sizeof(wq_stack), 8, "test_wq"};
    REQUIRE(wq.valid());

    static std::atomic<osal::work_queue*> active_wq{nullptr};
    active_wq.store(&wq, std::memory_order_release);

    static osal::semaphore gate{osal::semaphore_type::binary, 0U};
    REQUIRE(gate.valid());
    while (gate.try_take())
    {
    }

    static std::atomic<int> flush_code{-1};
    static std::atomic<int> exec_count{0};
    flush_code.store(-1, std::memory_order_relaxed);
    exec_count.store(0, std::memory_order_relaxed);

    auto blocker = [](void*) { gate.take_for(osal::milliseconds{2000}); };
    auto counter = [](void*) { (void)exec_count.fetch_add(1, std::memory_order_relaxed); };
    auto flusher = [](void*)
    {
        auto* queue = active_wq.load(std::memory_order_acquire);
        if (queue != nullptr)
        {
            flush_code.store(static_cast<int>(queue->flush(osal::milliseconds{50}).code()),
                             std::memory_order_release);
        }
    };

    REQUIRE(wq.submit(blocker).ok());
    osal::thread::sleep_for(osal::milliseconds{20});
    REQUIRE(wq.submit(counter).ok());
    REQUIRE(wq.submit(counter).ok());

    alignas(16) static std::uint8_t flush_stack[65536];
    osal::thread                    flush_thread;
    REQUIRE(
        flush_thread.create(make_wq_cfg(flusher, nullptr, flush_stack, sizeof(flush_stack), "wq_timeout_flush")).ok());

    osal::thread::sleep_for(osal::milliseconds{10});
    REQUIRE(wq.cancel_all().ok());
    REQUIRE(flush_thread.join().ok());

    CHECK(flush_code.load(std::memory_order_acquire) == static_cast<int>(osal::error_code::timeout));
    CHECK(exec_count.load(std::memory_order_relaxed) == 0);

    gate.give();
    REQUIRE(wq.flush(osal::milliseconds{2000}).ok());
    active_wq.store(nullptr, std::memory_order_release);
#endif
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

    // Clean up: cancel pending items, then drain the in-flight blocker.
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

    static std::atomic_bool exec{false};
    exec.store(false, std::memory_order_relaxed);
    auto cb = [](void*) { exec.store(true, std::memory_order_release); };
    REQUIRE(wq.submit(cb).ok());
    REQUIRE(wq.flush(osal::milliseconds{2000}).ok());
    CHECK(exec.load(std::memory_order_acquire));
}
