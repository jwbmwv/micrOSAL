// SPDX-License-Identifier: Apache-2.0
/// @file test_thread.cpp
/// @brief Tests for osal::thread.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <cstring>

static_assert(osal::thread::supports_timed_join == osal::active_capabilities::has_timed_join);
static_assert(osal::thread::supports_affinity == osal::active_capabilities::has_thread_affinity);
static_assert(osal::thread::supports_dynamic_priority == osal::active_capabilities::has_dynamic_thread_priority);
static_assert(osal::thread::supports_task_notification == osal::active_capabilities::has_task_notification);
static_assert(osal::thread::supports_suspend_resume == osal::active_capabilities::has_thread_suspend_resume);

TEST_CASE("thread: default construction is not valid")
{
    osal::thread t;
    CHECK_FALSE(t.valid());
}

TEST_CASE("thread: create and join")
{
    volatile bool executed = false;

    struct ctx_t
    {
        volatile bool* flag;
    } ctx{&executed};

    auto entry = [](void* arg)
    {
        auto* c  = static_cast<ctx_t*>(arg);
        *c->flag = true;
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    t;
    osal::thread_config             cfg{};
    cfg.entry       = entry;
    cfg.arg         = &ctx;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "basic";
    REQUIRE(t.create(cfg).ok());
    CHECK(t.valid());

    REQUIRE(t.join().ok());
    CHECK(executed);
    CHECK_FALSE(t.valid());  // After join, handle is cleared.
}

TEST_CASE("thread: detach")
{
    osal::semaphore done{osal::semaphore_type::binary, 0U};
    REQUIRE(done.valid());

    struct ctx_t
    {
        osal::semaphore* sem;
    } ctx{&done};

    auto entry = [](void* arg)
    {
        auto* c = static_cast<ctx_t*>(arg);
        osal::thread::sleep_for(osal::milliseconds{10});
        c->sem->give();
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    t;
    osal::thread_config             cfg{};
    cfg.entry       = entry;
    cfg.arg         = &ctx;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "detach";
    REQUIRE(t.create(cfg).ok());
    REQUIRE(t.detach().ok());
    CHECK_FALSE(t.valid());

    // Wait for the detached thread to finish.
    CHECK(done.take_for(osal::milliseconds{2000}));
}

TEST_CASE("thread: yield does not crash")
{
    osal::thread::yield();
    CHECK(true);
}

TEST_CASE("thread: sleep_for")
{
    const auto t1 = osal::monotonic_clock::now();
    osal::thread::sleep_for(osal::milliseconds{30});
    const auto t2      = osal::monotonic_clock::now();
    const auto elapsed = std::chrono::duration_cast<osal::milliseconds>(t2 - t1);
    CHECK(elapsed.count() >= 20);
}

TEST_CASE("thread: set_priority")
{
    volatile bool ran = false;
    struct ctx_t
    {
        volatile bool* flag;
    } ctx{&ran};

    auto entry = [](void* arg)
    {
        auto* c  = static_cast<ctx_t*>(arg);
        *c->flag = true;
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    t;
    osal::thread_config             cfg{};
    cfg.entry       = entry;
    cfg.arg         = &ctx;
    cfg.priority    = osal::PRIORITY_NORMAL;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "prio";
    REQUIRE(t.create(cfg).ok());

    // set_priority should at least not crash.
    (void)t.set_priority(osal::PRIORITY_NORMAL + 1);

    REQUIRE(t.join().ok());
    CHECK(ran);
}

TEST_CASE("thread: suspend/resume")
{
    volatile bool keep_running = true;

    struct ctx_t
    {
        volatile bool* running;
    } ctx{&keep_running};

    auto entry = [](void* arg)
    {
        auto* c = static_cast<ctx_t*>(arg);
        while (*c->running)
        {
            osal::thread::yield();
        }
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    t;
    osal::thread_config             cfg{};
    cfg.entry       = entry;
    cfg.arg         = &ctx;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "suspend_resume";
    REQUIRE(t.create(cfg).ok());

    const osal::result s = t.suspend();
    if constexpr (osal::active_capabilities::has_thread_suspend_resume)
    {
        CHECK(s.ok());
        CHECK(t.resume().ok());
    }
    else
    {
        CHECK(s == osal::error_code::not_supported);
    }

    keep_running = false;
    REQUIRE(t.join().ok());
}

TEST_CASE("thread: join_for with timed join")
{
    if constexpr (!osal::active_capabilities::has_timed_join)
    {
        MESSAGE("Skipped — backend does not support timed join");
        return;
    }

    auto entry = [](void*) { osal::thread::sleep_for(osal::milliseconds{10}); };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    t;
    osal::thread_config             cfg{};
    cfg.entry       = entry;
    cfg.arg         = nullptr;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "timed_join";
    REQUIRE(t.create(cfg).ok());

    // Long timeout — should succeed.
    auto r = t.join_for(osal::milliseconds{5000});
    CHECK(r.ok());
}

// ---------------------------------------------------------------------------
// sleep_until
// ---------------------------------------------------------------------------

TEST_CASE("thread: sleep_until wakes at/after deadline")
{
    const auto before   = osal::monotonic_clock::now();
    const auto deadline = before + osal::milliseconds{30};
    osal::thread::sleep_until(deadline);
    const auto after = osal::monotonic_clock::now();
    CHECK(after >= deadline);
}

TEST_CASE("thread: sleep_until in the past returns immediately")
{
    const auto past   = osal::monotonic_clock::now() - osal::milliseconds{100};
    const auto before = osal::monotonic_clock::now();
    osal::thread::sleep_until(past);
    const auto after = osal::monotonic_clock::now();
    // Should not sleep at all — delta should be tiny (well under 50 ms).
    CHECK((after - before) < osal::milliseconds{50});
}

// ---------------------------------------------------------------------------
// osal::this_thread
// ---------------------------------------------------------------------------

TEST_CASE("this_thread: yield does not crash")
{
    osal::this_thread::yield();
}

TEST_CASE("this_thread: sleep_for sleeps at least the requested duration")
{
    const auto before = osal::monotonic_clock::now();
    osal::this_thread::sleep_for(osal::milliseconds{20});
    const auto after = osal::monotonic_clock::now();
    CHECK((after - before) >= osal::milliseconds{20});
}

TEST_CASE("this_thread: sleep_until wakes at/after deadline")
{
    const auto deadline = osal::monotonic_clock::now() + osal::milliseconds{20};
    osal::this_thread::sleep_until(deadline);
    CHECK(osal::monotonic_clock::now() >= deadline);
}

TEST_CASE("thread_local_data: set/get in current thread")
{
    osal::thread_local_data tls;
    REQUIRE(tls.valid());

    int value = 42;
    CHECK(tls.get() == nullptr);
    REQUIRE(tls.set(&value).ok());
    CHECK(tls.get() == &value);
}

TEST_CASE("thread_local_data: values are isolated per thread")
{
    osal::thread_local_data tls;
    REQUIRE(tls.valid());

    int main_value = 1;
    REQUIRE(tls.set(&main_value).ok());

    osal::semaphore done{osal::semaphore_type::binary, 0U};
    REQUIRE(done.valid());

    struct ctx_t
    {
        osal::thread_local_data* tls;
        osal::semaphore*         done;
        int                      worker_value;
        void*                    seen_before;
        void*                    seen_after;
    } ctx{&tls, &done, 2, nullptr, nullptr};

    auto entry = [](void* arg)
    {
        auto* c        = static_cast<ctx_t*>(arg);
        c->seen_before = c->tls->get();
        (void)c->tls->set(&c->worker_value);
        c->seen_after = c->tls->get();
        (void)c->done->give();
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    t;
    osal::thread_config             cfg{};
    cfg.entry       = entry;
    cfg.arg         = &ctx;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "tls_worker";
    REQUIRE(t.create(cfg).ok());

    CHECK(done.take_for(osal::milliseconds{2000}));
    REQUIRE(t.join().ok());

    CHECK(ctx.seen_before == nullptr);
    CHECK(ctx.seen_after == &ctx.worker_value);
    CHECK(tls.get() == &main_value);
}

// ---------------------------------------------------------------------------
// Task notification — capability-gated
// ---------------------------------------------------------------------------

TEST_CASE("thread: set_priority respects has_dynamic_thread_priority")
{
    if constexpr (!osal::active_capabilities::has_dynamic_thread_priority)
    {
        osal::thread t;
        // Can't create a valid thread without a running RTOS here, but
        // set_priority on a default-constructed thread must not crash.
        const osal::result r = t.set_priority(osal::PRIORITY_NORMAL);
        CHECK(r == osal::error_code::not_supported);
    }
    // When has_dynamic_thread_priority == true the main set_priority test
    // already exercises the happy path.
}

TEST_CASE("thread: task notify returns not_supported on stub backends")
{
    if constexpr (!osal::active_capabilities::has_task_notification)
    {
        // Default-constructed (invalid) thread — the impl must return
        // not_supported without crashing regardless of handle state.
        osal::thread t;
        CHECK(t.notify() == osal::error_code::not_supported);
        CHECK(t.notify_isr() == osal::error_code::not_supported);
        CHECK(osal::thread::wait_for_notification(osal::milliseconds{1}) == osal::error_code::not_supported);
    }
}

TEST_CASE("thread: task notify round-trip (native backends)")
{
    if constexpr (osal::active_capabilities::has_task_notification)
    {
        // Spin up a worker that waits for a notification, then verify that
        // the notifying side can send a value and the worker receives it.
        static std::atomic<std::uint32_t> received{0};
        static osal::semaphore            ready{osal::semaphore_type::binary, 0U};
        static osal::semaphore            done{osal::semaphore_type::binary, 0U};
        REQUIRE(ready.valid());
        REQUIRE(done.valid());

        struct ctx_t
        {
        };
        static ctx_t ctx{};

        auto worker = [](void*)
        {
            ready.give();  // signal that we are about to wait
            std::uint32_t val = 0U;
            osal::thread::wait_for_notification(osal::milliseconds{2000}, &val);
            received.store(val);
            done.give();
        };

        alignas(16) static std::uint8_t stack[65536];
        osal::thread                    t;
        osal::thread_config             cfg{};
        cfg.entry       = worker;
        cfg.arg         = &ctx;
        cfg.stack       = stack;
        cfg.stack_bytes = sizeof(stack);
        cfg.name        = "notify_worker";
        REQUIRE(t.create(cfg).ok());

        CHECK(ready.take_for(osal::milliseconds{2000}));
        CHECK(t.notify(0xDEADBEEFU).ok());
        CHECK(done.take_for(osal::milliseconds{2000}));
        CHECK(received.load() == 0xDEADBEEFU);
        REQUIRE(t.join().ok());
    }
}
