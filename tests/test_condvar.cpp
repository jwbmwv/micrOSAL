// SPDX-License-Identifier: Apache-2.0
/// @file test_condvar.cpp
/// @brief Tests for osal::condvar.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>

// Stacks for helper threads.
alignas(16) static std::uint8_t t_stack[65536];
alignas(16) static std::uint8_t t_stack2[65536];

// Helper to create a thread_config quickly.
static osal::thread_config make_cfg(void (*entry)(void*), void* arg, void* stack, std::size_t stack_sz,
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

TEST_CASE("condvar: construction succeeds")
{
    osal::condvar cv;
    CHECK(cv.valid());
}

// ---------------------------------------------------------------------------
// notify_one wakes one waiter
// ---------------------------------------------------------------------------

TEST_CASE("condvar: notify_one wakes one waiting thread")
{
    static osal::mutex   mtx;
    static osal::condvar cv;
    REQUIRE(mtx.valid());
    REQUIRE(cv.valid());

    static volatile bool data_ready = false;
    static volatile bool consumed   = false;
    data_ready                      = false;
    consumed                        = false;

    // Consumer thread: wait for the flag.
    auto consumer = [](void*)
    {
        mtx.lock();
        while (!data_ready)
        {
            cv.wait(mtx);
        }
        consumed = true;
        mtx.unlock();
    };

    osal::thread t;
    REQUIRE(t.create(make_cfg(consumer, nullptr, t_stack, sizeof(t_stack), "cv_cons")).ok());

    // Let consumer reach the wait.
    osal::thread::sleep_for(osal::milliseconds{30});

    // Produce.
    mtx.lock();
    data_ready = true;
    mtx.unlock();
    cv.notify_one();

    t.join();
    CHECK(consumed);
}

// ---------------------------------------------------------------------------
// notify_all wakes all waiters
// ---------------------------------------------------------------------------

TEST_CASE("condvar: notify_all wakes all waiting threads")
{
    static osal::mutex   mtx;
    static osal::condvar cv;
    REQUIRE(mtx.valid());
    REQUIRE(cv.valid());

    static volatile bool    go = false;
    static std::atomic<int> woke_up{0};
    go = false;
    woke_up.store(0);

    auto waiter = [](void*)
    {
        mtx.lock();
        while (!go)
        {
            cv.wait(mtx);
        }
        woke_up.fetch_add(1);
        mtx.unlock();
    };

    osal::thread t1, t2;
    REQUIRE(t1.create(make_cfg(waiter, nullptr, t_stack, sizeof(t_stack), "cv_w1")).ok());
    REQUIRE(t2.create(make_cfg(waiter, nullptr, t_stack2, sizeof(t_stack2), "cv_w2")).ok());

    osal::thread::sleep_for(osal::milliseconds{30});

    mtx.lock();
    go = true;
    mtx.unlock();
    cv.notify_all();

    t1.join();
    t2.join();
    CHECK(woke_up.load() == 2);
}

// ---------------------------------------------------------------------------
// wait_for returns false on timeout
// ---------------------------------------------------------------------------

TEST_CASE("condvar: wait_for times out")
{
    static osal::mutex   mtx;
    static osal::condvar cv;
    REQUIRE(mtx.valid());
    REQUIRE(cv.valid());

    mtx.lock();
    bool ok = cv.wait_for(mtx, osal::milliseconds{20});
    mtx.unlock();

    CHECK_FALSE(ok);
}

// ---------------------------------------------------------------------------
// wait_for returns true when notified before timeout
// ---------------------------------------------------------------------------

TEST_CASE("condvar: wait_for succeeds on notify before timeout")
{
    static osal::mutex   mtx;
    static osal::condvar cv;
    REQUIRE(mtx.valid());
    REQUIRE(cv.valid());

    static std::atomic<bool> result_ok{false};
    result_ok.store(false);

    auto waiter_fn = [](void*)
    {
        mtx.lock();
        result_ok.store(cv.wait_for(mtx, osal::milliseconds{2000}));
        mtx.unlock();
    };

    osal::thread t;
    REQUIRE(t.create(make_cfg(waiter_fn, nullptr, t_stack, sizeof(t_stack), "cv_wf")).ok());

    osal::thread::sleep_for(osal::milliseconds{30});
    cv.notify_one();

    t.join();
    CHECK(result_ok.load());
}

// ---------------------------------------------------------------------------
// Multiple signal/wait cycles
// ---------------------------------------------------------------------------

TEST_CASE("condvar: multiple cycles")
{
    static osal::mutex   mtx;
    static osal::condvar cv;
    REQUIRE(mtx.valid());
    REQUIRE(cv.valid());

    static std::atomic<int> step{0};
    step.store(0);

    auto worker = [](void*)
    {
        for (int i = 0; i < 3; ++i)
        {
            mtx.lock();
            while (step.load() != i * 2 + 1)
            {
                cv.wait(mtx);
            }
            step.fetch_add(1);
            mtx.unlock();
            cv.notify_one();
        }
    };

    osal::thread t;
    REQUIRE(t.create(make_cfg(worker, nullptr, t_stack, sizeof(t_stack), "cv_cyc")).ok());

    for (int i = 0; i < 3; ++i)
    {
        mtx.lock();
        step.store(i * 2 + 1);
        mtx.unlock();
        cv.notify_one();

        // Wait for worker to advance step.
        for (int j = 0; j < 200; ++j)
        {
            osal::thread::sleep_for(osal::milliseconds{5});
            if (step.load() == i * 2 + 2)
            {
                break;
            }
        }
        CHECK(step.load() == i * 2 + 2);
    }

    t.join();
}

// ---------------------------------------------------------------------------
// wait(mutex, predicate) — spurious-wakeup safe
// ---------------------------------------------------------------------------

TEST_CASE("condvar: wait with predicate blocks until predicate is true")
{
    static osal::mutex   mtx;
    static osal::condvar cv;
    REQUIRE(mtx.valid());
    REQUIRE(cv.valid());

    static volatile bool ready = false;
    ready                      = false;

    auto setter = [](void*)
    {
        osal::thread::sleep_for(osal::milliseconds{40});
        {
            osal::mutex::lock_guard lg{mtx};
            ready = true;
        }
        cv.notify_one();
    };

    osal::thread t;
    REQUIRE(t.create(make_cfg(setter, nullptr, t_stack, sizeof(t_stack), "pred_wait")).ok());

    {
        osal::mutex::lock_guard lg{mtx};
        cv.wait(mtx, [] { return ready == true; });
    }
    CHECK(ready);

    t.join();
}

// ---------------------------------------------------------------------------
// wait_for(mutex, timeout, predicate) — times out when predicate stays false
// ---------------------------------------------------------------------------

TEST_CASE("condvar: wait_for with predicate times out when predicate stays false")
{
    static osal::mutex   mtx;
    static osal::condvar cv;
    REQUIRE(mtx.valid());
    REQUIRE(cv.valid());

    mtx.lock();
    bool ok = cv.wait_for(mtx, osal::milliseconds{30}, [] { return false; });
    mtx.unlock();

    CHECK_FALSE(ok);
}

// ---------------------------------------------------------------------------
// wait_for(mutex, timeout, predicate) — succeeds when predicate becomes true
// ---------------------------------------------------------------------------

TEST_CASE("condvar: wait_for with predicate succeeds before timeout")
{
    static osal::mutex   mtx;
    static osal::condvar cv;
    REQUIRE(mtx.valid());
    REQUIRE(cv.valid());

    static volatile bool flag = false;
    flag                      = false;

    auto setter = [](void*)
    {
        osal::thread::sleep_for(osal::milliseconds{20});
        {
            osal::mutex::lock_guard lg{mtx};
            flag = true;
        }
        cv.notify_one();
    };

    osal::thread t;
    REQUIRE(t.create(make_cfg(setter, nullptr, t_stack, sizeof(t_stack), "pred_wf")).ok());

    bool ok;
    {
        mtx.lock();
        ok = cv.wait_for(mtx, osal::milliseconds{2000}, [] { return flag == true; });
        mtx.unlock();
    }
    CHECK(ok);

    t.join();
}

// ---------------------------------------------------------------------------
// wait_until — times out at deadline
// ---------------------------------------------------------------------------

TEST_CASE("condvar: wait_until times out at deadline")
{
    static osal::mutex   mtx;
    static osal::condvar cv;
    REQUIRE(mtx.valid());
    REQUIRE(cv.valid());

    const auto deadline = osal::monotonic_clock::now() + osal::milliseconds{30};
    mtx.lock();
    bool ok = cv.wait_until(mtx, deadline);
    mtx.unlock();

    CHECK_FALSE(ok);
    CHECK(osal::monotonic_clock::now() >= deadline);
}

// ---------------------------------------------------------------------------
// wait_until(mutex, deadline, predicate) — succeeds when pred becomes true
// ---------------------------------------------------------------------------

TEST_CASE("condvar: wait_until with predicate succeeds before deadline")
{
    static osal::mutex   mtx;
    static osal::condvar cv;
    REQUIRE(mtx.valid());
    REQUIRE(cv.valid());

    static volatile bool ready2 = false;
    ready2                      = false;

    auto setter = [](void*)
    {
        osal::thread::sleep_for(osal::milliseconds{20});
        {
            osal::mutex::lock_guard lg{mtx};
            ready2 = true;
        }
        cv.notify_one();
    };

    osal::thread t;
    REQUIRE(t.create(make_cfg(setter, nullptr, t_stack, sizeof(t_stack), "wuntil_pred")).ok());

    const auto deadline = osal::monotonic_clock::now() + osal::milliseconds{2000};
    bool       ok;
    {
        mtx.lock();
        ok = cv.wait_until(mtx, deadline, [] { return ready2 == true; });
        mtx.unlock();
    }
    CHECK(ok);

    t.join();
}
