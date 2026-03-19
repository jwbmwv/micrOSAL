// SPDX-License-Identifier: Apache-2.0
/// @file test_thread_local_data.cpp
/// @brief Tests for osal::thread_local_data.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("thread_local_data: construction succeeds")
{
    osal::thread_local_data tls;
    CHECK(tls.valid());
}

TEST_CASE("thread_local_data: initial get returns nullptr")
{
    osal::thread_local_data tls;
    REQUIRE(tls.valid());
    CHECK(tls.get() == nullptr);
}

// ---------------------------------------------------------------------------
// set / get on creating thread
// ---------------------------------------------------------------------------

TEST_CASE("thread_local_data: set and get on same thread")
{
    osal::thread_local_data tls;
    REQUIRE(tls.valid());

    static int value = 42;
    CHECK(tls.set(&value).ok());
    CHECK(tls.get() == &value);
}

TEST_CASE("thread_local_data: set nullptr is retrievable")
{
    osal::thread_local_data tls;
    REQUIRE(tls.valid());

    static int v = 1;
    REQUIRE(tls.set(&v).ok());
    CHECK(tls.set(nullptr).ok());
    CHECK(tls.get() == nullptr);
}

// ---------------------------------------------------------------------------
// Per-thread isolation — each thread sees its own value
// ---------------------------------------------------------------------------

TEST_CASE("thread_local_data: each thread sees its own value")
{
    static osal::thread_local_data tls;
    REQUIRE(tls.valid());

    static int               val_a = 100;
    static int               val_b = 200;
    static std::atomic<bool> a_ready{false};
    static std::atomic<bool> b_ready{false};
    static std::atomic<bool> a_ok{false};
    static std::atomic<bool> b_ok{false};
    a_ready.store(false);
    b_ready.store(false);
    a_ok.store(false);
    b_ok.store(false);

    struct ctx_t
    {
        int*               val;
        std::atomic<bool>* ready;
        std::atomic<bool>* ok;
    };
    static ctx_t ctx_a{&val_a, &a_ready, &a_ok};
    static ctx_t ctx_b{&val_b, &b_ready, &b_ok};

    auto worker = [](void* arg)
    {
        auto* c = static_cast<ctx_t*>(arg);
        tls.set(c->val);
        c->ready->store(true, std::memory_order_release);
        // Busy-wait until both threads have set their values.
        while (!a_ready.load(std::memory_order_acquire) || !b_ready.load(std::memory_order_acquire))
        {
            osal::thread::yield();
        }
        // Each thread should see only its own value.
        c->ok->store(tls.get() == c->val, std::memory_order_release);
    };

    alignas(16) static std::uint8_t stack_a[65536];
    alignas(16) static std::uint8_t stack_b[65536];

    osal::thread_config cfg{};
    osal::thread        ta, tb;

    cfg.entry       = worker;
    cfg.arg         = &ctx_a;
    cfg.stack       = stack_a;
    cfg.stack_bytes = sizeof(stack_a);
    cfg.name        = "tls_a";
    REQUIRE(ta.create(cfg).ok());

    cfg.entry       = worker;
    cfg.arg         = &ctx_b;
    cfg.stack       = stack_b;
    cfg.stack_bytes = sizeof(stack_b);
    cfg.name        = "tls_b";
    REQUIRE(tb.create(cfg).ok());

    REQUIRE(ta.join().ok());
    REQUIRE(tb.join().ok());

    CHECK(a_ok.load());
    CHECK(b_ok.load());
}

// ---------------------------------------------------------------------------
// Creating thread does not see child thread's value
// ---------------------------------------------------------------------------

TEST_CASE("thread_local_data: parent does not see child value")
{
    static osal::thread_local_data tls;
    REQUIRE(tls.valid());

    static int               child_val = 999;
    static std::atomic<bool> child_set{false};
    child_set.store(false);

    auto child = [](void*)
    {
        tls.set(&child_val);
        child_set.store(true, std::memory_order_release);
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread_config             cfg{};
    cfg.entry       = child;
    cfg.arg         = nullptr;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "child";

    // Parent's value before thread creation.
    static int parent_val = 1;
    REQUIRE(tls.set(&parent_val).ok());

    osal::thread t;
    REQUIRE(t.create(cfg).ok());
    REQUIRE(t.join().ok());

    // Parent should still see its own value, not the child's.
    CHECK(tls.get() == &parent_val);
}

// ---------------------------------------------------------------------------
// Multiple independent TLS keys
// ---------------------------------------------------------------------------

TEST_CASE("thread_local_data: multiple keys are independent")
{
    osal::thread_local_data key1;
    osal::thread_local_data key2;
    REQUIRE(key1.valid());
    REQUIRE(key2.valid());

    static int v1 = 11;
    static int v2 = 22;
    CHECK(key1.set(&v1).ok());
    CHECK(key2.set(&v2).ok());

    CHECK(key1.get() == &v1);
    CHECK(key2.get() == &v2);

    // Overwrite key1 — key2 unaffected.
    static int v3 = 33;
    CHECK(key1.set(&v3).ok());
    CHECK(key1.get() == &v3);
    CHECK(key2.get() == &v2);
}

// ---------------------------------------------------------------------------
// Key reuse after destruction (generation counter guard)
// ---------------------------------------------------------------------------

TEST_CASE("thread_local_data: destroyed key returns nullptr on get")
{
    osal::thread_local_data* tls = new osal::thread_local_data{};
    REQUIRE(tls->valid());

    static int v = 5;
    CHECK(tls->set(&v).ok());
    CHECK(tls->get() == &v);

    // Destroy the key — this increments the generation counter.
    delete tls;

    // A new key at the same slot should NOT see the old value.
    osal::thread_local_data tls2;
    REQUIRE(tls2.valid());
    CHECK(tls2.get() == nullptr);
}

// ---------------------------------------------------------------------------
// Exhaust all keys — 17th key should fail
// ---------------------------------------------------------------------------

TEST_CASE("thread_local_data: exhausting all keys returns invalid")
{
    constexpr std::size_t                 kMax = OSAL_THREAD_LOCAL_MAX_KEYS;
    std::vector<osal::thread_local_data*> keys;
    keys.reserve(kMax);

    std::size_t valid_count = 0;
    // Allocate beyond the limit.
    for (std::size_t i = 0; i <= kMax; ++i)
    {
        auto* k = new osal::thread_local_data{};
        keys.push_back(k);
        if (k->valid())
        {
            ++valid_count;
        }
    }

    // At most kMax can be valid.
    CHECK(valid_count <= kMax);

    for (auto* k : keys)
        delete k;
}
