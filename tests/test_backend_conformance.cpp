// SPDX-License-Identifier: Apache-2.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include "../src/common/backend_timeout_adapter.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("backend conformance: bounded semaphore rejects excess tokens")
{
    for (const unsigned maximum : {1U, 3U})
    {
        osal::active_traits::semaphore_handle_t sem{};
        REQUIRE(osal_semaphore_create(&sem, 0U, maximum).ok());
        for (unsigned i = 0; i < maximum; ++i)
        {
            CHECK(osal_semaphore_give(&sem).ok());
        }
        CHECK(osal_semaphore_give(&sem) == osal::error_code::overflow);
        CHECK(osal_semaphore_give_isr(&sem) == osal::error_code::overflow);
        for (unsigned i = 0; i < maximum; ++i)
        {
            CHECK(osal_semaphore_try_take(&sem).ok());
        }
        CHECK_FALSE(osal_semaphore_try_take(&sem).ok());
        CHECK(osal_semaphore_give(&sem).ok());
        CHECK(osal_semaphore_try_take(&sem).ok());
        CHECK(osal_semaphore_destroy(&sem).ok());
    }
}

TEST_CASE("backend conformance: large destination receives tiny message in task and ISR")
{
    osal::active_traits::message_buffer_handle_t handle{};
    std::array<std::uint8_t, 32>                 ring{};
    std::array<std::uint8_t, 65536>              destination{};
    const std::uint8_t                           message = 0x63;
    REQUIRE(osal_message_buffer_create(&handle, ring.data(), ring.size()).ok());
    REQUIRE(osal_message_buffer_send(&handle, &message, 1, osal::NO_WAIT).ok());
    CHECK(osal_message_buffer_receive(&handle, destination.data(), destination.size(), osal::NO_WAIT) == 1U);
    CHECK(destination[0] == message);
    CHECK(destination[1] == 0U);
    REQUIRE(osal_message_buffer_send(&handle, &message, 1, osal::NO_WAIT).ok());
    CHECK(osal_message_buffer_receive_isr(&handle, destination.data(), destination.size()) == 1U);
    CHECK(destination[0] == message);
    CHECK(destination[1] == 0U);
    CHECK(osal_message_buffer_destroy(&handle).ok());
}

TEST_CASE("backend conformance: concurrent gives cannot exceed the semaphore cap")
{
    osal::active_traits::semaphore_handle_t sem{};
    REQUIRE(osal_semaphore_create(&sem, 0U, 3U).ok());
    std::atomic<unsigned>      accepted{0};
    std::array<std::thread, 8> givers;
    for (auto& giver : givers)
    {
        giver = std::thread(
            [&]
            {
                for (unsigned i = 0; i < 100; ++i)
                {
                    if (osal_semaphore_give(&sem).ok())
                    {
                        accepted.fetch_add(1);
                    }
                }
            });
    }
    for (auto& giver : givers)
    {
        giver.join();
    }
    CHECK(accepted.load() == 3U);
    unsigned taken = 0;
    while (osal_semaphore_try_take(&sem).ok())
    {
        ++taken;
    }
    CHECK(taken == 3U);
    CHECK(osal_semaphore_destroy(&sem).ok());
}

TEST_CASE("backend conformance: queue preserves single byte payload")
{
    osal::active_traits::queue_handle_t queue{};
    std::array<std::uint8_t, 2>         storage{};
    std::uint8_t                        input = 42;
    std::array<std::uint8_t, 3>         output{0, 0xA5, 0x5A};
    REQUIRE(osal_queue_create(&queue, storage.data(), 1, 2).ok());
    REQUIRE(osal_queue_send(&queue, &input, osal::NO_WAIT).ok());
    REQUIRE(osal_queue_receive(&queue, output.data(), osal::NO_WAIT).ok());
    CHECK(output[0] == input);
    CHECK(output[1] == 0xA5);
    CHECK(output[2] == 0x5A);
    CHECK(osal_queue_destroy(&queue).ok());
}

TEST_CASE("backend conformance: rwlock finite waits last at least the requested interval")
{
    osal::active_traits::rwlock_handle_t lock{};
    REQUIRE(osal_rwlock_create(&lock).ok());
    REQUIRE(osal_rwlock_write_lock(&lock, osal::WAIT_FOREVER).ok());
    for (const bool reader : {true, false})
    {
        osal::result                        result{};
        std::chrono::steady_clock::duration elapsed{};
        std::thread                         waiter(
            [&]
            {
                const auto start = std::chrono::steady_clock::now();
                result = reader ? osal_rwlock_read_lock(&lock, 30) : osal_rwlock_write_lock(&lock, 30);
                elapsed = std::chrono::steady_clock::now() - start;
                if (result.ok())
                {
                    if (reader)
                    {
                        osal_rwlock_read_unlock(&lock);
                    }
                    else
                    {
                        osal_rwlock_write_unlock(&lock);
                    }
                }
            });
        waiter.join();
        CHECK(result == osal::error_code::timeout);
        CHECK(elapsed >= 30ms);
    }
    CHECK(osal_rwlock_write_unlock(&lock).ok());
    CHECK(osal_rwlock_destroy(&lock).ok());
}

TEST_CASE("backend conformance: finite join uses the native deadline clock")
{
    osal::active_traits::thread_handle_t thread{};
    REQUIRE(osal_thread_create(
                &thread, [](void*) { std::this_thread::sleep_for(120ms); }, nullptr, osal::PRIORITY_NORMAL,
                osal::AFFINITY_ANY, nullptr, 0, "join-test")
                .ok());
    const auto poll_start = std::chrono::steady_clock::now();
    CHECK_FALSE(osal_thread_join(&thread, osal::NO_WAIT).ok());
    CHECK(std::chrono::steady_clock::now() - poll_start < 100ms);
    const auto start  = std::chrono::steady_clock::now();
    const auto result = osal_thread_join(&thread, 30);
    CHECK(result == osal::error_code::timeout);
    CHECK(std::chrono::steady_clock::now() - start >= 30ms);
    REQUIRE(thread.native != nullptr);
    CHECK(osal_thread_join(&thread, osal::WAIT_FOREVER).ok());
}

TEST_CASE("backend conformance: empty semaphore and queue timed waits do not expire early")
{
    osal::active_traits::semaphore_handle_t sem{};
    REQUIRE(osal_semaphore_create(&sem, 0U, 1U).ok());
    auto start = std::chrono::steady_clock::now();
    CHECK(osal_semaphore_take(&sem, 30) == osal::error_code::timeout);
    CHECK(std::chrono::steady_clock::now() - start >= 30ms);
    CHECK(osal_semaphore_destroy(&sem).ok());

    osal::active_traits::queue_handle_t queue{};
    std::uint8_t                        storage{};
    std::uint8_t                        output{};
    REQUIRE(osal_queue_create(&queue, &storage, 1U, 1U).ok());
    start = std::chrono::steady_clock::now();
    CHECK(osal_queue_receive(&queue, &output, 30) == osal::error_code::timeout);
    CHECK(std::chrono::steady_clock::now() - start >= 30ms);
    CHECK(osal_queue_destroy(&queue).ok());
}

TEST_CASE("backend conformance: timespec conversion divides before multiplying")
{
    constexpr auto timeout = static_cast<osal::tick_t>(std::numeric_limits<osal::tick_t>::max() - 1U);
    timespec       before{};
    clock_gettime(CLOCK_MONOTONIC, &before);
    const auto deadline = osal::detail::backend_timeout_adapter::to_abs_timespec(CLOCK_MONOTONIC, timeout);
    const auto seconds  = static_cast<std::uint64_t>(timeout) / 1000U;
    const auto room     = static_cast<std::uint64_t>(std::numeric_limits<time_t>::max() - before.tv_sec);
    if (seconds < room)
    {
        CHECK(static_cast<std::uint64_t>(deadline.tv_sec - before.tv_sec) >= seconds);
    }
    else
    {
        CHECK(deadline.tv_sec == std::numeric_limits<time_t>::max());
    }
    CHECK(deadline.tv_nsec >= 0);
    CHECK(deadline.tv_nsec < 1'000'000'000L);
}
