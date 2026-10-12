// SPDX-License-Identifier: Apache-2.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/bus/osal_signal.hpp>
#include <osal/delayable_work.hpp>
#include <osal/osal_c.h>

#include <atomic>
#include <thread>

namespace
{
std::atomic<std::int64_t>            now_ms{0};
std::atomic<bool>                    clock_stub{false};
bool                                 fail_create    = false;
bool                                 fail_stop      = false;
bool                                 fail_submit    = false;
osal_timer_callback_t                saved_callback = nullptr;
void*                                saved_argument = nullptr;
osal::active_traits::timer_handle_t* stub_timer     = nullptr;
std::atomic<bool>                    observe_receive{false};
std::atomic<bool>                    receive_entered{false};
std::atomic<bool>                    release_receive{false};
}  // namespace

extern "C" std::int64_t osal_test_real_clock_monotonic_ms() noexcept asm("__real_osal_clock_monotonic_ms");
extern "C" std::int64_t osal_test_wrap_clock_monotonic_ms() noexcept asm("__wrap_osal_clock_monotonic_ms");
extern "C" std::int64_t osal_test_wrap_clock_monotonic_ms() noexcept
{
    return clock_stub.load() ? now_ms.load() : osal_test_real_clock_monotonic_ms();
}

extern "C" osal::result osal_test_real_queue_create(osal::active_traits::queue_handle_t*, void*, std::size_t,
                                                    std::size_t) noexcept asm("__real_osal_queue_create");
extern "C" osal::result osal_test_wrap_queue_create(osal::active_traits::queue_handle_t*, void*, std::size_t,
                                                    std::size_t) noexcept asm("__wrap_osal_queue_create");
extern "C" osal::result osal_test_wrap_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer,
                                                    std::size_t size, std::size_t capacity) noexcept
{
    return fail_create ? osal::result{osal::error_code::out_of_resources}
                       : osal_test_real_queue_create(handle, buffer, size, capacity);
}

extern "C" osal::result osal_test_real_queue_receive(osal::active_traits::queue_handle_t*, void*, osal::tick_t) noexcept
    asm("__real_osal_queue_receive");
extern "C" osal::result osal_test_wrap_queue_receive(osal::active_traits::queue_handle_t*, void*, osal::tick_t) noexcept
    asm("__wrap_osal_queue_receive");
extern "C" osal::result osal_test_wrap_queue_receive(osal::active_traits::queue_handle_t* handle, void* destination,
                                                     osal::tick_t timeout) noexcept
{
    if (observe_receive.load())
    {
        receive_entered.store(true);
        while (!release_receive.load())
        {
            std::this_thread::yield();
        }
    }
    return osal_test_real_queue_receive(handle, destination, timeout);
}

extern "C" osal::result osal_test_real_timer_create(osal::active_traits::timer_handle_t*, const char*,
                                                    osal_timer_callback_t, void*, osal::tick_t, bool) noexcept
    asm("__real_osal_timer_create");
extern "C" osal::result osal_test_wrap_timer_create(osal::active_traits::timer_handle_t*, const char*,
                                                    osal_timer_callback_t, void*, osal::tick_t, bool) noexcept
    asm("__wrap_osal_timer_create");
extern "C" osal::result osal_test_wrap_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                                    osal_timer_callback_t callback, void* argument, osal::tick_t ticks,
                                                    bool reload) noexcept
{
    saved_callback = callback;
    saved_argument = argument;
    stub_timer     = handle;
    return osal_test_real_timer_create(handle, name, callback, argument, ticks, reload);
}

extern "C" osal::result osal_test_real_timer_start(osal::active_traits::timer_handle_t*) noexcept
    asm("__real_osal_timer_start");
extern "C" osal::result osal_test_wrap_timer_start(osal::active_traits::timer_handle_t*) noexcept
    asm("__wrap_osal_timer_start");
extern "C" osal::result osal_test_wrap_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
{
    return handle == stub_timer ? osal::ok() : osal_test_real_timer_start(handle);
}
extern "C" osal::result osal_test_real_timer_stop(osal::active_traits::timer_handle_t*) noexcept
    asm("__real_osal_timer_stop");
extern "C" osal::result osal_test_wrap_timer_stop(osal::active_traits::timer_handle_t*) noexcept
    asm("__wrap_osal_timer_stop");
extern "C" osal::result osal_test_wrap_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
{
    if (handle != stub_timer)
    {
        return osal_test_real_timer_stop(handle);
    }
    return fail_stop ? osal::result{osal::error_code::permission_denied} : osal::ok();
}
extern "C" osal::result osal_test_real_timer_set_period(osal::active_traits::timer_handle_t*, osal::tick_t) noexcept
    asm("__real_osal_timer_set_period");
extern "C" osal::result osal_test_wrap_timer_set_period(osal::active_traits::timer_handle_t*, osal::tick_t) noexcept
    asm("__wrap_osal_timer_set_period");
extern "C" osal::result osal_test_wrap_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                                        osal::tick_t                         ticks) noexcept
{
    return handle == stub_timer ? osal::ok() : osal_test_real_timer_set_period(handle, ticks);
}
extern "C" osal::result osal_test_real_work_queue_submit(osal::active_traits::work_queue_handle_t*, osal_work_func_t,
                                                         void*) noexcept asm("__real_osal_work_queue_submit");
extern "C" osal::result osal_test_wrap_work_queue_submit(osal::active_traits::work_queue_handle_t*, osal_work_func_t,
                                                         void*) noexcept asm("__wrap_osal_work_queue_submit");
extern "C" osal::result osal_test_wrap_work_queue_submit(osal::active_traits::work_queue_handle_t* handle,
                                                         osal_work_func_t callback, void* argument) noexcept
{
    return fail_submit ? osal::result{osal::error_code::overflow}
                       : osal_test_real_work_queue_submit(handle, callback, argument);
}

TEST_CASE("pubsub: failed backing queue cannot produce a successful subscription")
{
    fail_create = true;
    osal::osal_signal<int, 2, 2> topic;
    fail_create            = false;
    osal::subscriber_id id = osal::invalid_subscriber_id;
    CHECK_FALSE(topic.valid());
    CHECK_FALSE(topic.subscribe(id));
    CHECK(id == osal::invalid_subscriber_id);
    CHECK(topic.subscriber_count() == 0U);
    CHECK_FALSE(topic.publish(1));
}

TEST_CASE("delayable_work: stop failure, stale expiry and dispatch error retain their contracts")
{
    osal::work_queue queue{nullptr, 0U};
    REQUIRE(queue.valid());
    std::atomic<unsigned> calls{0U};
    osal::delayable_work  work{queue, +[](void* arg) { static_cast<std::atomic<unsigned>*>(arg)->fetch_add(1U); },
                              &calls};
    REQUIRE(work.valid());
    now_ms.store(1000);
    clock_stub.store(true);
    CHECK(work.schedule(osal::milliseconds{100}).ok());
    fail_stop = true;
    CHECK(work.reschedule(osal::milliseconds{200}) == osal::error_code::permission_denied);
    CHECK(work.scheduled());
    CHECK(work.cancel() == osal::error_code::permission_denied);
    CHECK(work.pending());
    fail_stop = false;
    CHECK(work.reschedule(osal::milliseconds{200}).ok());
    now_ms.store(1100);
    saved_callback(saved_argument);
    CHECK(work.scheduled());
    CHECK(calls.load() == 0U);
    now_ms.store(1200);
    fail_submit = true;
    saved_callback(saved_argument);
    fail_submit = false;
    CHECK(work.flush(osal::milliseconds{0}) == osal::error_code::overflow);
    CHECK_FALSE(work.pending());
    CHECK(work.flush(osal::milliseconds{0}).ok());
    clock_stub.store(false);
}

TEST_CASE("pubsub: a receiving slot cannot be unsubscribed and recycled")
{
    osal::osal_signal<int, 1, 2> topic;
    osal::subscriber_id          id = osal::invalid_subscriber_id;
    REQUIRE(topic.subscribe(id));
    receive_entered.store(false);
    release_receive.store(false);
    observe_receive.store(true);
    int         received = 0;
    bool        success  = false;
    std::thread receiver{[&] { success = topic.receive(id, received, osal::NO_WAIT); }};
    const auto  deadline = osal::monotonic_deadline::after(osal::milliseconds{1000});
    while (!receive_entered.load() && !deadline.expired())
    {
        std::this_thread::yield();
    }
    CHECK(receive_entered.load());
    CHECK_FALSE(topic.unsubscribe(id));
    CHECK(topic.subscriber_count() == 1U);
    CHECK(topic.publish(42));
    release_receive.store(true);
    receiver.join();
    observe_receive.store(false);
    CHECK(success);
    CHECK(received == 42);
    CHECK(topic.unsubscribe(id));
}

TEST_CASE("C delayable work: stop failures and old expiries cannot consume a replacement arm")
{
    osal_work_queue_handle queue{};
    REQUIRE(osal_c_work_queue_create(&queue, nullptr, 0U, 4U, "faults") == OSAL_OK);
    osal_delayable_work_handle work{};
    std::atomic<unsigned>      calls{0U};
    CHECK(osal_c_delayable_work_create(
              &work, &queue, +[](void* arg) { static_cast<std::atomic<unsigned>*>(arg)->fetch_add(1U); }, &calls,
              "faults") == OSAL_OK);
    now_ms.store(1000);
    clock_stub.store(true);
    CHECK(osal_c_delayable_work_schedule(&work, 100U) == OSAL_OK);
    fail_stop = true;
    CHECK(osal_c_delayable_work_reschedule(&work, 200U) == OSAL_PERMISSION_DENIED);
    CHECK(osal_c_delayable_work_cancel(&work) == OSAL_PERMISSION_DENIED);
    CHECK(osal_c_delayable_work_pending(&work) != 0);
    fail_stop = false;
    CHECK(osal_c_delayable_work_reschedule(&work, 200U) == OSAL_OK);
    now_ms.store(1100);
    saved_callback(saved_argument);
    CHECK(osal_c_delayable_work_scheduled(&work) != 0);
    CHECK(calls.load() == 0U);
    now_ms.store(1200);
    fail_submit = true;
    saved_callback(saved_argument);
    fail_submit = false;
    CHECK(osal_c_delayable_work_flush(&work, 0U) == OSAL_OVERFLOW);
    CHECK(osal_c_delayable_work_flush(&work, 0U) == OSAL_OK);
    clock_stub.store(false);
    CHECK(osal_c_delayable_work_destroy(&work) == OSAL_OK);
    CHECK(osal_c_work_queue_destroy(&queue) == OSAL_OK);
}
