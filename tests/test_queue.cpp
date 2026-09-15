// SPDX-License-Identifier: Apache-2.0
/// @file test_queue.cpp
/// @brief Tests for osal::queue<T, N>.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>
#include <utility>

#if defined(OSAL_TEST_QUEUE_FAULT_INJECTION)
namespace
{
thread_local osal::error_code queue_create_failure{osal::error_code::ok};
thread_local osal::error_code queue_receive_failure{osal::error_code::ok};
}  // namespace

extern "C" osal::result osal_test_real_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer,
                                                    std::size_t item_size, std::size_t capacity) noexcept
    asm("__real_osal_queue_create");
extern "C" osal::result osal_test_real_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                                     osal::tick_t timeout) noexcept asm("__real_osal_queue_receive");

extern "C" osal::result osal_test_wrap_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer,
                                                    std::size_t item_size, std::size_t capacity) noexcept
    asm("__wrap_osal_queue_create");
extern "C" osal::result osal_test_wrap_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                                     osal::tick_t timeout) noexcept asm("__wrap_osal_queue_receive");

extern "C" osal::result osal_test_wrap_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer,
                                                    std::size_t item_size, std::size_t capacity) noexcept
{
    if (queue_create_failure != osal::error_code::ok)
    {
        return std::exchange(queue_create_failure, osal::error_code::ok);
    }
    return osal_test_real_queue_create(handle, buffer, item_size, capacity);
}

extern "C" osal::result osal_test_wrap_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                                     osal::tick_t timeout) noexcept
{
    if (queue_receive_failure != osal::error_code::ok)
    {
        return std::exchange(queue_receive_failure, osal::error_code::ok);
    }
    return osal_test_real_queue_receive(handle, item, timeout);
}
#endif

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
namespace
{

template<typename Queue>
concept expected_receivable_queue = requires(Queue& queue) {
    queue.receive_expected();
    queue.try_receive_expected();
};

}  // namespace
#endif

TEST_CASE("queue: construction succeeds")
{
    osal::queue<std::uint32_t, 4> q;
    CHECK(q.valid());
}

TEST_CASE("queue: starts empty")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());
    CHECK(q.empty());
    CHECK_FALSE(q.full());
    CHECK(q.count() == 0);
    CHECK(q.free_slots() == 4);
}

TEST_CASE("queue: send and receive single item")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    REQUIRE(q.send(42U).ok());
    CHECK(q.count() == 1);

    std::uint32_t val = 0;
    REQUIRE(q.receive(val).ok());
    CHECK(val == 42U);
    CHECK(q.empty());
}

TEST_CASE("queue: FIFO ordering")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    REQUIRE(q.send(1U).ok());
    REQUIRE(q.send(2U).ok());
    REQUIRE(q.send(3U).ok());

    std::uint32_t val = 0;
    REQUIRE(q.receive(val).ok());
    CHECK(val == 1U);
    REQUIRE(q.receive(val).ok());
    CHECK(val == 2U);
    REQUIRE(q.receive(val).ok());
    CHECK(val == 3U);
}

TEST_CASE("queue: full detection")
{
    osal::queue<std::uint32_t, 2> q;
    REQUIRE(q.valid());

    REQUIRE(q.try_send(10U));
    REQUIRE(q.try_send(20U));
    CHECK(q.full());
    CHECK_FALSE(q.try_send(30U));
}

TEST_CASE("queue: try_receive on empty returns false")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    std::uint32_t val = 0;
    CHECK_FALSE(q.try_receive(val));
}

TEST_CASE("queue: peek does not remove item")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    REQUIRE(q.send(99U).ok());

    std::uint32_t val = 0;
    CHECK(q.peek(val));
    CHECK(val == 99U);
    CHECK(q.count() == 1);  // Still there.
}

TEST_CASE("queue: struct type")
{
    struct msg_t
    {
        std::uint16_t id;
        std::uint32_t payload;
    };

    osal::queue<msg_t, 8> q;
    REQUIRE(q.valid());

    msg_t out{42, 0xDEADBEEF};
    REQUIRE(q.send(out).ok());

    msg_t in{};
    REQUIRE(q.receive(in).ok());
    CHECK(in.id == 42);
    CHECK(in.payload == 0xDEADBEEF);
}

TEST_CASE("queue: cross-thread send/receive")
{
    static osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    auto producer = [](void*)
    {
        for (std::uint32_t i = 1; i <= 3; ++i)
        {
            (void)q.send(i);
        }
    };

    alignas(16) static std::uint8_t stack[65536];
    osal::thread                    t;
    osal::thread_config             cfg{};
    cfg.entry       = producer;
    cfg.arg         = nullptr;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "q_prod";
    REQUIRE(t.create(cfg).ok());

    std::uint32_t val = 0;
    for (std::uint32_t i = 1; i <= 3; ++i)
    {
        REQUIRE(q.receive(val).ok());
        CHECK(val == i);
    }

    REQUIRE(t.join().ok());
}

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
TEST_CASE("queue: receive_expected and try_receive_expected")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    auto exp_empty = q.try_receive_expected();
    REQUIRE_FALSE(exp_empty.has_value());
    CHECK(exp_empty.error() == osal::error_code::would_block);

    REQUIRE(q.send(100U).ok());
    REQUIRE(q.send(200U).ok());

    auto exp_try = q.try_receive_expected();
    REQUIRE(exp_try.has_value());
    CHECK(*exp_try == 100U);

    auto exp_recv = q.receive_expected();
    REQUIRE(exp_recv.has_value());
    CHECK(*exp_recv == 200U);
    CHECK(q.empty());
}

TEST_CASE("queue: expected receive requires nonthrowing value construction")
{
    struct no_default_message
    {
        no_default_message() = delete;
        std::uint32_t value;
    };
    struct throwing_default_message
    {
        throwing_default_message() noexcept(false);
        std::uint32_t value;
    };

    static_assert(osal::queue_element<no_default_message>);
    static_assert(osal::queue_element<throwing_default_message>);
    static_assert(expected_receivable_queue<osal::queue<std::uint32_t, 2>>);
    static_assert(!expected_receivable_queue<osal::queue<no_default_message, 2>>);
    static_assert(!expected_receivable_queue<osal::queue<throwing_default_message, 2>>);
    static_assert(requires(osal::queue<no_default_message, 2>& queue, no_default_message& item) {
        queue.receive(item);
        queue.try_receive(item);
    });

    osal::queue<std::uint32_t, 2> queue;
    REQUIRE(queue.valid());
    CHECK(noexcept(queue.receive_expected()));
    CHECK(noexcept(queue.try_receive_expected()));
}

#if defined(OSAL_TEST_QUEUE_FAULT_INJECTION)
TEST_CASE("queue: expected receive reports failed initialization")
{
    queue_create_failure = osal::error_code::out_of_resources;
    osal::queue<std::uint32_t, 2> queue;
    REQUIRE_FALSE(queue.valid());

    const auto nonblocking = queue.try_receive_expected();
    REQUIRE_FALSE(nonblocking.has_value());
    CHECK(nonblocking.error() == osal::error_code::not_initialized);
    const auto blocking = queue.receive_expected();
    REQUIRE_FALSE(blocking.has_value());
    CHECK(blocking.error() == osal::error_code::not_initialized);
}

TEST_CASE("queue: expected receive preserves backend failures")
{
    osal::queue<std::uint32_t, 2> queue;
    REQUIRE(queue.valid());
    REQUIRE(queue.try_send(42U));

    for (const auto error :
         {osal::error_code::not_initialized, osal::error_code::isr_invalid, osal::error_code::unknown})
    {
        queue_receive_failure  = error;
        const auto nonblocking = queue.try_receive_expected();
        REQUIRE_FALSE(nonblocking.has_value());
        CHECK(nonblocking.error() == error);

        queue_receive_failure = error;
        const auto blocking   = queue.receive_expected();
        REQUIRE_FALSE(blocking.has_value());
        CHECK(blocking.error() == error);
        CHECK(queue.count() == 1U);
    }

    const auto recovered = queue.try_receive_expected();
    REQUIRE(recovered.has_value());
    CHECK(*recovered == 42U);
}
#endif
#endif

TEST_CASE("queue: snapshots are safe during concurrent mutation")
{
    osal::queue<std::uint32_t, 4> q;
    REQUIRE(q.valid());

    struct producer_state
    {
        osal::queue<std::uint32_t, 4>* queue;
        std::atomic<bool>              complete{false};
    } state{&q};

    auto producer = [](void* arg)
    {
        auto* const producer = static_cast<producer_state*>(arg);
        for (std::uint32_t value = 0U; value < 1'000U; ++value)
        {
            while (!producer->queue->try_send(value))
            {
                osal::thread::yield();
            }
        }
        producer->complete.store(true);
    };

    alignas(16) std::uint8_t stack[65536];
    osal::thread             t;
    osal::thread_config      cfg{};
    cfg.entry       = producer;
    cfg.arg         = &state;
    cfg.stack       = stack;
    cfg.stack_bytes = sizeof(stack);
    cfg.name        = "q_snapshot";
    REQUIRE(t.create(cfg).ok());

    std::uint32_t value = 0U;
    while (!state.complete.load() || !q.empty())
    {
        CHECK(q.count() <= q.capacity);
        CHECK(q.free_slots() <= q.capacity);
        (void)q.try_receive(value);
        osal::thread::yield();
    }

    REQUIRE(t.join().ok());
}
