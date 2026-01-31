// SPDX-License-Identifier: Apache-2.0
/// @file test_integration.cpp
/// @brief Integration-level tests spanning multiple OSAL primitives.
///
/// Covers cross-thread event_flags, timer reset, timed operations under
/// contention, backend metadata, and thread re-creation.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <cstring>

// ---------------------------------------------------------------------------
// Backend metadata
// ---------------------------------------------------------------------------

TEST_CASE("backend: name() returns a non-empty string")
{
    const char* name = osal::backend_name();
    REQUIRE(name != nullptr);
    CHECK(std::strlen(name) > 0);
}

// ---------------------------------------------------------------------------
// Cross-thread event_flags — thread A waits, thread B sets
// ---------------------------------------------------------------------------

TEST_CASE("integration: cross-thread event_flags wait_all")
{
    static osal::event_flags ef;
    REQUIRE(ef.valid());
    (void)ef.clear(0xFFFFFFFFU);

    // Thread sets bits 0x01 and 0x02 with a small delay between.
    auto setter = [](void*) {
        osal::thread::sleep_for(osal::milliseconds{20});
        (void)ef.set(0x01);
        osal::thread::sleep_for(osal::milliseconds{20});
        (void)ef.set(0x02);
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread t;
    osal::thread_config cfg{};
    cfg.entry = setter;
    cfg.arg   = nullptr;
    cfg.stack = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name  = "ef_all";
    REQUIRE(t.create(cfg).ok());

    osal::event_bits_t actual = 0;
    auto r = ef.wait_all(0x03, &actual, true, osal::milliseconds{2000});
    CHECK(r.ok());
    CHECK((actual & 0x03) == 0x03);

    REQUIRE(t.join().ok());
}

// ---------------------------------------------------------------------------
// Timer reset restarts the period
// ---------------------------------------------------------------------------

TEST_CASE("integration: timer reset restarts countdown")
{
    if constexpr (!osal::active_capabilities::has_timer)
    {
        MESSAGE("Skipped — backend does not support timers");
        return;
    }

    static volatile std::uint32_t count = 0;
    auto cb = [](void*) { ++count; };

    count = 0;
    // Long period so it won't fire during reset window.
    osal::timer tmr{cb, nullptr, osal::milliseconds{300}, osal::timer_mode::one_shot};
    REQUIRE(tmr.valid());

    REQUIRE(tmr.start().ok());
    // Wait 100 ms, then reset — the 300 ms countdown should restart.
    osal::thread::sleep_for(osal::milliseconds{100});
    REQUIRE(tmr.reset().ok());

    // At this point 100 ms have elapsed total.  The reset should
    // push the fire time to ~400 ms total.  Wait only 100 ms more;
    // the timer should NOT have fired yet.
    osal::thread::sleep_for(osal::milliseconds{100});
    CHECK(count == 0);

    // Now wait enough for it to fire.
    osal::thread::sleep_for(osal::milliseconds{350});
    REQUIRE(tmr.stop().ok());
    CHECK(count == 1);
}

// ---------------------------------------------------------------------------
// Mutex try_lock_for times out under contention
// ---------------------------------------------------------------------------

TEST_CASE("integration: mutex try_lock_for timeout under contention")
{
    static osal::mutex mtx;
    REQUIRE(mtx.valid());

    static volatile bool holder_ready = false;
    static volatile bool holder_release = false;

    auto holder = [](void*) {
        mtx.lock();
        holder_ready = true;
        while (!holder_release)
        {
            osal::thread::yield();
        }
        mtx.unlock();
    };

    holder_ready   = false;
    holder_release = false;

    alignas(16) static std::uint8_t stack[65536];
    osal::thread t;
    osal::thread_config cfg{};
    cfg.entry = holder;
    cfg.arg   = nullptr;
    cfg.stack = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name  = "holder";
    REQUIRE(t.create(cfg).ok());

    while (!holder_ready)
    {
        osal::thread::yield();
    }

    // Attempt to lock with a short timeout — should fail.
    auto before = osal::monotonic_clock::now();
    bool got = mtx.try_lock_for(osal::milliseconds{50});
    auto after  = osal::monotonic_clock::now();

    CHECK_FALSE(got);
    auto elapsed = std::chrono::duration_cast<osal::milliseconds>(after - before);
    CHECK(elapsed.count() >= 30);

    holder_release = true;
    REQUIRE(t.join().ok());

    // After release the mutex should be available.
    CHECK(mtx.try_lock());
    mtx.unlock();
}

// ---------------------------------------------------------------------------
// Queue receive_for times out on empty queue
// ---------------------------------------------------------------------------

TEST_CASE("integration: queue receive_for timeout on empty")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());
    REQUIRE(q.empty());

    auto before = osal::monotonic_clock::now();
    std::uint32_t val = 0;
    bool got = q.receive_for(val, osal::milliseconds{50});
    auto after  = osal::monotonic_clock::now();

    CHECK_FALSE(got);
    auto elapsed = std::chrono::duration_cast<osal::milliseconds>(after - before);
    CHECK(elapsed.count() >= 30);
}

// ---------------------------------------------------------------------------
// Thread re-creation: create → join → create → join on same object
// ---------------------------------------------------------------------------

TEST_CASE("integration: thread re-creation")
{
    static volatile int run_count = 0;
    run_count = 0;

    auto entry = [](void*) {
        ++run_count;
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread t;

    for (int i = 0; i < 3; ++i)
    {
        osal::thread_config cfg{};
        cfg.entry = entry;
        cfg.arg   = nullptr;
        cfg.stack = stack;
        cfg.stack_bytes = sizeof(stack);
        cfg.name  = "reuse";
        REQUIRE(t.create(cfg).ok());
        REQUIRE(t.join().ok());
        CHECK_FALSE(t.valid());
    }

    CHECK(run_count == 3);
}
