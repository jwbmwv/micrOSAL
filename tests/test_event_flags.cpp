// SPDX-License-Identifier: Apache-2.0
/// @file test_event_flags.cpp
/// @brief Tests for osal::event_flags.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>

alignas(16) static std::uint8_t ef_stack[65536];

TEST_CASE("event_flags: construction succeeds")
{
    osal::event_flags ef;
    CHECK(ef.valid());
}

TEST_CASE("event_flags: starts with all bits clear")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());
    CHECK(ef.get() == 0U);
}

TEST_CASE("event_flags: set and get")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0x05).ok());  // Set bits 0 and 2.
    CHECK((ef.get() & 0x05) == 0x05);
}

TEST_CASE("event_flags: clear")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0xFF).ok());
    REQUIRE(ef.clear(0x0F).ok());
    CHECK((ef.get() & 0x0F) == 0U);
    CHECK((ef.get() & 0xF0) == 0xF0);
}

TEST_CASE("event_flags: wait_any immediate")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0x02).ok());

    osal::event_bits_t actual = 0;
    auto               r      = ef.wait_any(0x02, &actual, false, osal::milliseconds{0});
    CHECK(r.ok());
    CHECK((actual & 0x02) != 0U);
}

TEST_CASE("event_flags: wait_any timeout when no bits set")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    auto r = ef.wait_any(0x01, nullptr, false, osal::milliseconds{20});
    CHECK_FALSE(r.ok());
}

TEST_CASE("event_flags: wait_all immediate")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0x03).ok());

    osal::event_bits_t actual = 0;
    auto               r      = ef.wait_all(0x03, &actual, false, osal::milliseconds{0});
    CHECK(r.ok());
    CHECK((actual & 0x03) == 0x03);
}

TEST_CASE("event_flags: wait_all fails if only partial bits set")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0x01).ok());
    auto r = ef.wait_all(0x03, nullptr, false, osal::milliseconds{20});
    CHECK_FALSE(r.ok());
}

TEST_CASE("event_flags: clear_on_exit")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    REQUIRE(ef.set(0x04).ok());

    osal::event_bits_t actual = 0;
    auto               r      = ef.wait_any(0x04, &actual, /*clear_on_exit=*/true, osal::milliseconds{100});
    CHECK(r.ok());

    // Bit should have been auto-cleared.
    CHECK((ef.get() & 0x04) == 0U);
}

TEST_CASE("event_flags: set_isr reflects backend support")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    const auto r = ef.set_isr(0x20);

    if constexpr (osal::active_capabilities::has_isr_event_flags)
    {
        CHECK(r.ok());
        CHECK((ef.get() & 0x20U) != 0U);
    }
    else
    {
        CHECK(r.code() == osal::error_code::not_supported);
        CHECK((ef.get() & 0x20U) == 0U);
    }
}

TEST_CASE("event_flags: cross-thread signalling")
{
    static osal::event_flags ef;
    REQUIRE(ef.valid());

    // Clear any leftover bits.
    (void)ef.clear(0xFFFFFFFFU);

    auto setter = [](void*)
    {
        osal::thread::sleep_for(osal::milliseconds{20});
        (void)ef.set(0x10);
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    t;
    osal::thread_config             cfg{};
    cfg.entry       = setter;
    cfg.arg         = nullptr;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "ef_set";
    REQUIRE(t.create(cfg).ok());

    osal::event_bits_t actual = 0;
    auto               r      = ef.wait_any(0x10, &actual, true, osal::milliseconds{2000});
    CHECK(r.ok());
    CHECK((actual & 0x10) != 0U);

    REQUIRE(t.join().ok());
}

TEST_CASE("event_flags: repeated timeout then signal cycles")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());

    static osal::event_flags* active_ef = nullptr;
    active_ef                           = &ef;

    constexpr osal::event_bits_t kBit        = 0x40U;
    constexpr int                kIterations = 128;

    auto setter = [](void*)
    {
        osal::thread::sleep_for(osal::milliseconds{1});
        (void)active_ef->set(kBit);
    };

    for (int iter = 0; iter < kIterations; ++iter)
    {
        CAPTURE(iter);

        REQUIRE(ef.clear(0xFFFFFFFFU).ok());
        CHECK(ef.wait_any(kBit, nullptr, false, osal::milliseconds{1}).code() == osal::error_code::timeout);

        osal::thread        t;
        osal::thread_config cfg{};
        cfg.entry       = setter;
        cfg.arg         = nullptr;
        cfg.stack       = ef_stack;
        cfg.stack_bytes = sizeof(ef_stack);
        cfg.name        = "ef_race";
        REQUIRE(t.create(cfg).ok());

        osal::event_bits_t actual = 0U;
        const auto         r      = ef.wait_any(kBit, &actual, true, osal::milliseconds{2000});

        REQUIRE(t.join().ok());
        CHECK(r.ok());
        CHECK((actual & kBit) != 0U);
        CHECK((ef.get() & kBit) == 0U);
    }
}

TEST_CASE("event_flags: only matching waiters wake while others remain blocked")
{
    osal::event_flags ef;
    REQUIRE(ef.valid());
    REQUIRE(ef.clear(0xFFFFFFFFU).ok());

    static std::atomic<int>          started{0};
    static std::atomic<unsigned int> woke_mask{0U};
    started.store(0, std::memory_order_relaxed);
    woke_mask.store(0U, std::memory_order_relaxed);

    struct waiter_ctx
    {
        osal::event_flags*         ef;
        osal::event_bits_t         bit;
        unsigned int               result_mask;
        std::atomic<int>*          started;
        std::atomic<unsigned int>* woke_mask;
    };

    auto waiter = [](void* arg)
    {
        auto* ctx = static_cast<waiter_ctx*>(arg);
        (void)ctx->started->fetch_add(1, std::memory_order_release);

        osal::event_bits_t actual = 0U;
        if (ctx->ef->wait_any(ctx->bit, &actual, true, osal::milliseconds{2000}).ok())
        {
            (void)ctx->woke_mask->fetch_or(ctx->result_mask, std::memory_order_acq_rel);
        }
    };

    alignas(16) static std::uint8_t stack_a[65536];
    alignas(16) static std::uint8_t stack_b[65536];
    alignas(16) static std::uint8_t stack_c[65536];
    waiter_ctx                      ctx_a{&ef, 0x01U, 0x01U, &started, &woke_mask};
    waiter_ctx                      ctx_b{&ef, 0x02U, 0x02U, &started, &woke_mask};
    waiter_ctx                      ctx_c{&ef, 0x04U, 0x04U, &started, &woke_mask};

    osal::thread        ta;
    osal::thread        tb;
    osal::thread        tc;
    osal::thread_config cfg_a{};
    osal::thread_config cfg_b{};
    osal::thread_config cfg_c{};
    cfg_a.entry       = waiter;
    cfg_a.arg         = &ctx_a;
    cfg_a.stack       = stack_a;
    cfg_a.stack_bytes = sizeof(stack_a);
    cfg_a.name        = "ef_wait_a";
    cfg_b.entry       = waiter;
    cfg_b.arg         = &ctx_b;
    cfg_b.stack       = stack_b;
    cfg_b.stack_bytes = sizeof(stack_b);
    cfg_b.name        = "ef_wait_b";
    cfg_c.entry       = waiter;
    cfg_c.arg         = &ctx_c;
    cfg_c.stack       = stack_c;
    cfg_c.stack_bytes = sizeof(stack_c);
    cfg_c.name        = "ef_wait_c";

    REQUIRE(ta.create(cfg_a).ok());
    REQUIRE(tb.create(cfg_b).ok());
    REQUIRE(tc.create(cfg_c).ok());

    for (int i = 0; i < 200 && started.load(std::memory_order_acquire) != 3; ++i)
    {
        osal::thread::sleep_for(osal::milliseconds{1});
    }
    REQUIRE(started.load(std::memory_order_acquire) == 3);
    osal::thread::sleep_for(osal::milliseconds{20});

    REQUIRE(ef.set(0x02U).ok());

    for (int i = 0; i < 200 && woke_mask.load(std::memory_order_acquire) != 0x02U; ++i)
    {
        osal::thread::sleep_for(osal::milliseconds{1});
    }
    CHECK(woke_mask.load(std::memory_order_acquire) == 0x02U);

    REQUIRE(ef.set(0x05U).ok());
    REQUIRE(ta.join().ok());
    REQUIRE(tb.join().ok());
    REQUIRE(tc.join().ok());
    CHECK(woke_mask.load(std::memory_order_acquire) == 0x07U);
}
