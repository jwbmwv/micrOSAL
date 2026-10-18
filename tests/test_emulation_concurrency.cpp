// SPDX-License-Identifier: Apache-2.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>
#include <chrono>
#include <thread>

// Exercise the static emulations even when the hosted backend uses native objects.
#define osal_condvar_create test_condvar_create
#define osal_condvar_destroy test_condvar_destroy
#define osal_condvar_wait test_condvar_wait
#define osal_condvar_notify_one test_condvar_notify_one
#define osal_condvar_notify_all test_condvar_notify_all
#include "../src/common/emulated_condvar.inl"
#undef osal_condvar_create
#undef osal_condvar_destroy
#undef osal_condvar_wait
#undef osal_condvar_notify_one
#undef osal_condvar_notify_all

#define osal_work_queue_create test_work_queue_create
#define osal_work_queue_destroy test_work_queue_destroy
#define osal_work_queue_submit test_work_queue_submit
#define osal_work_queue_submit_from_isr test_work_queue_submit_from_isr
#define osal_work_queue_flush test_work_queue_flush
#define osal_work_queue_cancel_all test_work_queue_cancel_all
#define osal_work_queue_pending test_work_queue_pending
#include "../src/common/emulated_work_queue.inl"
#undef osal_work_queue_create
#undef osal_work_queue_destroy
#undef osal_work_queue_submit
#undef osal_work_queue_submit_from_isr
#undef osal_work_queue_flush
#undef osal_work_queue_cancel_all
#undef osal_work_queue_pending

using namespace std::chrono_literals;

TEST_CASE("emulated condvar: consecutive notify_one selects distinct registered waiters")
{
    osal::active_traits::condvar_handle_t cv{};
    osal::active_traits::mutex_handle_t   mutex{};
    REQUIRE(test_condvar_create(&cv).ok());
    REQUIRE(osal_mutex_create(&mutex, false).ok());
    std::atomic<unsigned> awakened{0};
    const auto            wait = [&]
    {
        osal_mutex_lock(&mutex, osal::WAIT_FOREVER);
        if (test_condvar_wait(&cv, &mutex, 2000).ok())
        {
            awakened.fetch_add(1);
        }
        osal_mutex_unlock(&mutex);
    };
    std::thread first(wait);
    std::thread second(wait);
    auto*       object     = static_cast<emulated_condvar_obj*>(cv.native);
    const auto  deadline   = std::chrono::steady_clock::now() + 1s;
    bool        registered = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        osal_mutex_lock(&object->guard, osal::WAIT_FOREVER);
        registered = object->n_waiters == 2;
        osal_mutex_unlock(&object->guard);
        if (registered)
        {
            break;
        }
        std::this_thread::yield();
    }
    CHECK(registered);
    osal_mutex_lock(&mutex, osal::WAIT_FOREVER);
    CHECK(test_condvar_notify_one(&cv).ok());
    CHECK(test_condvar_notify_one(&cv).ok());
    osal_mutex_unlock(&mutex);
    first.join();
    second.join();
    CHECK(awakened.load() == 2U);
    CHECK(test_condvar_destroy(&cv).ok());
    CHECK(osal_mutex_destroy(&mutex).ok());
}

TEST_CASE("emulated work queue: concurrent cancellation cannot consume new submission wakeups")
{
    osal::active_traits::work_queue_handle_t queue{};
    REQUIRE(test_work_queue_create(&queue, nullptr, 0, 16, "emu-cancel").ok());
    std::atomic<unsigned> executed{0};
    const auto            work = [](void* arg) { static_cast<std::atomic<unsigned>*>(arg)->fetch_add(1); };
    std::thread           canceller(
        [&]
        {
            for (unsigned i = 0; i < 2000; ++i)
            {
                test_work_queue_cancel_all(&queue);
                std::this_thread::yield();
            }
        });
    for (unsigned i = 0; i < 2000; ++i)
    {
        (void)test_work_queue_submit(&queue, work, &executed);
        std::this_thread::yield();
    }
    canceller.join();
    CHECK(test_work_queue_cancel_all(&queue).ok());
    std::atomic<unsigned> sentinel{0};
    REQUIRE(test_work_queue_submit(&queue, work, &sentinel).ok());
    CHECK(test_work_queue_flush(&queue, 2000).ok());
    CHECK(sentinel.load() == 1U);
    CHECK(test_work_queue_destroy(&queue).ok());
}
