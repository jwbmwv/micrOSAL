// SPDX-License-Identifier: Apache-2.0
/// @file test_rwlock.cpp
/// @brief Tests for osal::rwlock (multiple-reader / single-writer lock).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>

// Shared thread stacks.
alignas(16) static std::uint8_t t_stack_a[65536];
alignas(16) static std::uint8_t t_stack_b[65536];
alignas(16) static std::uint8_t t_stack_c[65536];

static osal::thread_config make_cfg(void (*entry)(void*), void* arg,
                                    void* stack, std::size_t sz,
                                    const char* name)
{
    osal::thread_config cfg{};
    cfg.entry       = entry;
    cfg.arg         = arg;
    cfg.stack       = stack;
    cfg.stack_bytes = sz;
    cfg.name        = name;
    return cfg;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: construction succeeds")
{
    osal::rwlock rw;
    CHECK(rw.valid());
}

// ---------------------------------------------------------------------------
// Basic read lock / unlock
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: read_lock and read_unlock")
{
    osal::rwlock rw;
    REQUIRE(rw.valid());
    CHECK(rw.read_lock().ok());
    CHECK(rw.read_unlock().ok());
}

// ---------------------------------------------------------------------------
// Basic write lock / unlock
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: write_lock and write_unlock")
{
    osal::rwlock rw;
    REQUIRE(rw.valid());
    CHECK(rw.write_lock().ok());
    CHECK(rw.write_unlock().ok());
}

// ---------------------------------------------------------------------------
// Multiple concurrent readers
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: multiple readers can hold lock concurrently")
{
    static osal::rwlock rw;
    static std::atomic<int> inside{0};
    static std::atomic<int> max_concurrent{0};
    static std::atomic<bool> overlap_detected{false};
    inside.store(0);
    max_concurrent.store(0);
    overlap_detected.store(false);

    auto reader = [](void*) {
        rw.read_lock();
        const int n = inside.fetch_add(1) + 1;
        int cur = max_concurrent.load();
        while (cur < n && !max_concurrent.compare_exchange_weak(cur, n)) {}
        if (inside.load() > 1)
        {
            overlap_detected.store(true);
        }
        osal::thread::sleep_for(osal::milliseconds{30});
        inside.fetch_sub(1);
        rw.read_unlock();
    };

    osal::thread ta, tb;
    REQUIRE(ta.create(make_cfg(reader, nullptr, t_stack_a, sizeof(t_stack_a), "reader_a")).ok());
    REQUIRE(tb.create(make_cfg(reader, nullptr, t_stack_b, sizeof(t_stack_b), "reader_b")).ok());
    REQUIRE(ta.join().ok());
    REQUIRE(tb.join().ok());

    // Both readers should have been inside simultaneously.
    CHECK(overlap_detected.load());
}

// ---------------------------------------------------------------------------
// Writer excludes readers
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: writer blocks readers")
{
    static osal::rwlock rw;
    static volatile bool reader_saw_data = false;
    static volatile int shared_value     = 0;
    reader_saw_data = false;
    shared_value    = 0;

    struct ctx_t
    {
        volatile int* value;
        volatile bool* saw;
    };

    // Writer holds the lock, sets value, releases.
    auto writer = [](void* arg) {
        auto* c = static_cast<ctx_t*>(arg);
        rw.write_lock();
        *c->value = 99;
        osal::thread::sleep_for(osal::milliseconds{50});
        rw.write_unlock();
    };

    // Reader waits for write lock to be released then reads.
    auto reader = [](void* arg) {
        auto* c = static_cast<ctx_t*>(arg);
        // Let writer get in first.
        osal::thread::sleep_for(osal::milliseconds{10});
        rw.read_lock();
        *c->saw = (*c->value == 99);
        rw.read_unlock();
    };

    static ctx_t ctx{&shared_value, &reader_saw_data};

    osal::thread tw, tr;
    REQUIRE(tw.create(make_cfg(writer, &ctx, t_stack_a, sizeof(t_stack_a), "writer")).ok());
    REQUIRE(tr.create(make_cfg(reader, &ctx, t_stack_b, sizeof(t_stack_b), "reader")).ok());
    REQUIRE(tw.join().ok());
    REQUIRE(tr.join().ok());

    CHECK(reader_saw_data);
}

// ---------------------------------------------------------------------------
// Timed read lock — succeeds on unlocked lock
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: read_lock_for succeeds on unlocked lock")
{
    osal::rwlock rw;
    REQUIRE(rw.valid());
    CHECK(rw.read_lock_for(osal::milliseconds{100}).ok());
    CHECK(rw.read_unlock().ok());
}

// ---------------------------------------------------------------------------
// Timed write lock — succeeds on unlocked lock
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: write_lock_for succeeds on unlocked lock")
{
    osal::rwlock rw;
    REQUIRE(rw.valid());
    CHECK(rw.write_lock_for(osal::milliseconds{100}).ok());
    CHECK(rw.write_unlock().ok());
}

// ---------------------------------------------------------------------------
// Timed write lock — times out when writer holds it
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: write_lock_for times out when another writer holds it")
{
    static osal::rwlock rw;
    static volatile bool writer_locked = false;

    auto holder = [](void*) {
        rw.write_lock();
        writer_locked = true;
        osal::thread::sleep_for(osal::milliseconds{200});
        rw.write_unlock();
    };

    osal::thread th;
    REQUIRE(th.create(make_cfg(holder, nullptr, t_stack_a, sizeof(t_stack_a), "holder")).ok());

    while (!writer_locked)
    {
        osal::thread::yield();
    }

    const auto result = rw.write_lock_for(osal::milliseconds{20});
    CHECK_FALSE(result.ok());

    REQUIRE(th.join().ok());
}

// ---------------------------------------------------------------------------
// RAII read_guard
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: read_guard RAII acquires and releases")
{
    osal::rwlock rw;
    REQUIRE(rw.valid());
    {
        osal::rwlock::read_guard rg{rw};
        CHECK(rw.valid());  // Still alive inside the guard.
    }
    // After guard destruction, write should succeed (not deadlock).
    CHECK(rw.write_lock_for(osal::milliseconds{100}).ok());
    rw.write_unlock();
}

// ---------------------------------------------------------------------------
// RAII write_guard
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: write_guard RAII acquires and releases")
{
    osal::rwlock rw;
    REQUIRE(rw.valid());
    {
        osal::rwlock::write_guard wg{rw};
        CHECK(rw.valid());
    }
    // After guard destruction, read should succeed.
    CHECK(rw.read_lock_for(osal::milliseconds{100}).ok());
    rw.read_unlock();
}

// ---------------------------------------------------------------------------
// Writer exclusivity — only one writer at a time
// ---------------------------------------------------------------------------

TEST_CASE("rwlock: writers are mutually exclusive")
{
    static osal::rwlock rw;
    static std::atomic<int> writers_inside{0};
    static std::atomic<bool> exclusivity_violated{false};
    writers_inside.store(0);
    exclusivity_violated.store(false);

    auto writer = [](void*) {
        for (int i = 0; i < 10; ++i)
        {
            rw.write_lock();
            const int n = writers_inside.fetch_add(1) + 1;
            if (n > 1)
            {
                exclusivity_violated.store(true);
            }
            osal::thread::sleep_for(osal::milliseconds{2});
            writers_inside.fetch_sub(1);
            rw.write_unlock();
        }
    };

    osal::thread ta, tb;
    REQUIRE(ta.create(make_cfg(writer, nullptr, t_stack_a, sizeof(t_stack_a), "writer_a")).ok());
    REQUIRE(tb.create(make_cfg(writer, nullptr, t_stack_b, sizeof(t_stack_b), "writer_b")).ok());
    REQUIRE(ta.join().ok());
    REQUIRE(tb.join().ok());

    CHECK_FALSE(exclusivity_violated.load());
}
