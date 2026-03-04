// SPDX-License-Identifier: Apache-2.0
/// @file test_mutex.cpp
/// @brief Tests for osal::mutex (normal and recursive).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

// ---------------------------------------------------------------------------
// Normal mutex
// ---------------------------------------------------------------------------

TEST_CASE("mutex: construction succeeds")
{
    osal::mutex m;
    CHECK(m.valid());
}

TEST_CASE("mutex: lock and unlock")
{
    osal::mutex m;
    REQUIRE(m.valid());
    m.lock();
    m.unlock();
}

TEST_CASE("mutex: try_lock succeeds on unlocked mutex")
{
    osal::mutex m;
    REQUIRE(m.valid());
    CHECK(m.try_lock());
    m.unlock();
}

TEST_CASE("mutex: try_lock fails when already locked (from another thread)")
{
    osal::mutex m;
    REQUIRE(m.valid());

    volatile bool locked_by_child = false;
    volatile bool child_done = false;

    struct ctx_t
    {
        osal::mutex* mtx;
        volatile bool* locked;
        volatile bool* done;
    } ctx{&m, &locked_by_child, &child_done};

    // Lock in a child thread, signal, then wait.
    auto entry = [](void* arg) {
        auto* c = static_cast<ctx_t*>(arg);
        c->mtx->lock();
        *c->locked = true;
        // Hold the lock for a bit.
        osal::thread::sleep_for(osal::milliseconds{100});
        c->mtx->unlock();
        *c->done = true;
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread t;
    osal::thread_config cfg{};
    cfg.entry = entry;
    cfg.arg = &ctx;
    cfg.stack = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name = "lock_test";
    REQUIRE(t.create(cfg).ok());

    // Wait for child to acquire.
    while (!locked_by_child)
    {
        osal::thread::yield();
    }

    // try_lock should fail since child holds it.
    CHECK_FALSE(m.try_lock());

    REQUIRE(t.join().ok());
    CHECK(child_done);

    // Now it should be unlocked.
    CHECK(m.try_lock());
    m.unlock();
}

TEST_CASE("mutex: try_lock_for succeeds within timeout")
{
    osal::mutex m;
    REQUIRE(m.valid());
    CHECK(m.try_lock_for(osal::milliseconds{100}));
    m.unlock();
}

TEST_CASE("mutex: lock_guard RAII")
{
    osal::mutex m;
    REQUIRE(m.valid());
    {
        osal::mutex::lock_guard guard{m};
        // Mutex should be locked here — can't easily test from same thread,
        // but at minimum the guard constructors/destructors don't crash.
    }
    // After guard is destroyed, mutex should be unlocked.
    CHECK(m.try_lock());
    m.unlock();
}

// ---------------------------------------------------------------------------
// Recursive mutex
// ---------------------------------------------------------------------------

TEST_CASE("recursive mutex: construction succeeds")
{
    osal::mutex m{osal::mutex_type::recursive};
    CHECK(m.valid());
}

TEST_CASE("recursive mutex: double lock from same thread")
{
    if constexpr (!osal::active_capabilities::has_recursive_mutex)
    {
        MESSAGE("Skipped — backend does not support recursive mutex");
        return;
    }

    osal::mutex m{osal::mutex_type::recursive};
    REQUIRE(m.valid());
    m.lock();
    // Second lock from same thread should not deadlock.
    CHECK(m.try_lock());
    m.unlock();
    m.unlock();
}

// ---------------------------------------------------------------------------
// Config-based construction (FLASH placement)
// ---------------------------------------------------------------------------

TEST_CASE("mutex: config construction — normal")
{
    const osal::mutex_config cfg{osal::mutex_type::normal};
    osal::mutex m{cfg};
    CHECK(m.valid());
    CHECK(m.try_lock());
    m.unlock();
}

TEST_CASE("mutex: config construction — recursive")
{
    if constexpr (!osal::active_capabilities::has_recursive_mutex)
    {
        MESSAGE("Skipped — backend does not support recursive mutex");
        return;
    }

    const osal::mutex_config cfg{osal::mutex_type::recursive};
    osal::mutex m{cfg};
    REQUIRE(m.valid());
    m.lock();
    CHECK(m.try_lock());
    m.unlock();
    m.unlock();
}

TEST_CASE("mutex: constexpr config compiles")
{
    // Verify the config struct is constexpr-constructible (FLASH-eligible).
    constexpr osal::mutex_config cfg{};
    osal::mutex m{cfg};
    CHECK(m.valid());
}
