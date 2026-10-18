// SPDX-License-Identifier: Apache-2.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>

#include <atomic>

#if defined(OSAL_BACKEND_THREADX) || defined(OSAL_BACKEND_PX5) || defined(OSAL_BACKEND_MICRIUM) ||          \
    defined(OSAL_BACKEND_CHIBIOS) || defined(OSAL_BACKEND_EMBOS) || defined(OSAL_BACKEND_VXWORKS) ||        \
    defined(OSAL_BACKEND_CMSIS_RTOS) || defined(OSAL_BACKEND_CMSIS_RTOS2) || defined(OSAL_BACKEND_NUTTX) || \
    defined(OSAL_BACKEND_QNX)
TEST_CASE("native contract: raw thread creation rejects invalid input and preserves a live handle")
{
    osal::active_traits::thread_handle_t handle{};
    alignas(16) unsigned char            stack[1024]{};
    const auto                           entry = [](void*) {};
    CHECK(osal_thread_create(nullptr, entry, nullptr, osal::PRIORITY_NORMAL, osal::AFFINITY_ANY, stack, sizeof(stack),
                             "native-test") == osal::error_code::invalid_argument);
    CHECK(osal_thread_create(&handle, nullptr, nullptr, osal::PRIORITY_NORMAL, osal::AFFINITY_ANY, stack, sizeof(stack),
                             "native-test") == osal::error_code::invalid_argument);
    CHECK(handle.native == nullptr);
    int owner{};
    handle.native = &owner;
    CHECK(osal_thread_create(&handle, entry, nullptr, osal::PRIORITY_NORMAL, osal::AFFINITY_ANY, stack, sizeof(stack),
                             "native-test") == osal::error_code::already_exists);
    CHECK(handle.native == &owner);
}

TEST_CASE("native contract: invalid timer creation has no ownership side effects")
{
    osal::active_traits::timer_handle_t handle{};
    const auto                          callback = [](void*) {};
    CHECK(osal_timer_create(nullptr, "native-test", callback, nullptr, 1U, false) ==
          osal::error_code::invalid_argument);
    CHECK(osal_timer_create(&handle, "native-test", nullptr, nullptr, 1U, false) == osal::error_code::invalid_argument);
    CHECK(osal_timer_create(&handle, "native-test", callback, nullptr, 0U, false) ==
          osal::error_code::invalid_argument);
    CHECK(osal_timer_create(&handle, "native-test", callback, nullptr, osal::WAIT_FOREVER, false) ==
          osal::error_code::invalid_argument);
    CHECK(handle.native == nullptr);
    int owner{};
    handle.native = &owner;
    CHECK(osal_timer_create(&handle, "native-test", callback, nullptr, 1U, false) == osal::error_code::already_exists);
    CHECK(handle.native == &owner);
}
#endif

#ifdef OSAL_NATIVE_TEST_CMSIS2_SDK
#include <cmsis_os2.h>

namespace
{
struct fake_timer
{
    osTimerFunc_t callback{};
    void*         argument{};
    osTimerType_t type{};
    bool          running{};
    uint32_t      ticks{};
};
fake_timer timers[8];
unsigned   timer_count{}, starts{}, deletes{};
osStatus_t detach_result = osOK, join_result = osOK, delete_result = osOK, start_result = osOK;
int        fake_thread;
}  // namespace

extern "C" osThreadId_t osThreadNew(osThreadFunc_t, void*, const osThreadAttr_t*)
{
    return &fake_thread;
}
extern "C" osStatus_t osThreadDetach(osThreadId_t)
{
    return detach_result;
}
extern "C" osStatus_t osThreadJoin(osThreadId_t)
{
    return join_result;
}
extern "C" osTimerId_t osTimerNew(osTimerFunc_t callback, osTimerType_t type, void* arg, const osTimerAttr_t*)
{
    auto& timer = timers[timer_count++];
    timer       = {callback, arg, type, false, 0};
    return &timer;
}
extern "C" osStatus_t osTimerDelete(osTimerId_t)
{
    ++deletes;
    return delete_result;
}
extern "C" osStatus_t osTimerStart(osTimerId_t id, uint32_t ticks)
{
    ++starts;
    if (start_result == osOK)
    {
        auto* timer    = static_cast<fake_timer*>(id);
        timer->running = true;
        timer->ticks   = ticks;
    }
    return start_result;
}
extern "C" osStatus_t osTimerStop(osTimerId_t id)
{
    static_cast<fake_timer*>(id)->running = false;
    return osOK;
}
extern "C" uint32_t osTimerIsRunning(osTimerId_t id)
{
    return static_cast<fake_timer*>(id)->running ? 1U : 0U;
}

TEST_CASE("CMSIS2 failed join and detach preserve ownership")
{
    osal::active_traits::thread_handle_t handle{};
    REQUIRE(osal_thread_create(
                &handle, [](void*) {}, nullptr, osal::PRIORITY_NORMAL, osal::AFFINITY_ANY, nullptr, 1024, "native-test")
                .ok());
    void* original = handle.native;
    detach_result  = osErrorResource;
    CHECK_FALSE(osal_thread_detach(&handle).ok());
    CHECK(handle.native == original);
    CHECK(osal_thread_join(&handle, osal::NO_WAIT) == osal::error_code::not_supported);
    CHECK(handle.native == original);
    join_result = osErrorResource;
    CHECK_FALSE(osal_thread_join(&handle, osal::WAIT_FOREVER).ok());
    CHECK(handle.native == original);
    detach_result = osOK;
    CHECK(osal_thread_detach(&handle).ok());
    CHECK(handle.native == nullptr);
}

TEST_CASE("CMSIS2 stopped period change never arms and preserves native mode")
{
    for (bool periodic : {false, true})
    {
        osal::active_traits::timer_handle_t handle{};
        REQUIRE(osal_timer_create(&handle, "native-test", [](void*) {}, nullptr, 10, periodic).ok());
        const unsigned before = starts;
        REQUIRE(osal_timer_set_period(&handle, 20).ok());
        CHECK(starts == before);
        CHECK_FALSE(osal_timer_is_active(&handle));
        CHECK(timers[timer_count - 1].type == (periodic ? osTimerPeriodic : osTimerOnce));
        CHECK(osal_timer_set_period(&handle, 0) == osal::error_code::invalid_argument);
        delete_result  = osErrorResource;
        void* original = handle.native;
        CHECK_FALSE(osal_timer_destroy(&handle).ok());
        CHECK(handle.native == original);
        delete_result = osOK;
        CHECK(osal_timer_destroy(&handle).ok());
        CHECK(handle.native == nullptr);
    }
}

TEST_CASE("CMSIS2 started timer context is never recycled without a callback barrier")
{
    static std::atomic<unsigned>        calls{0};
    osal::active_traits::timer_handle_t handle{};
    REQUIRE(osal_timer_create(
                &handle, "native-test", [](void* value) { static_cast<std::atomic<unsigned>*>(value)->fetch_add(1); },
                &calls, 10, false)
                .ok());
    REQUIRE(osal_timer_start(&handle).ok());
    REQUIRE(osal_timer_stop(&handle).ok());
    const unsigned before   = deletes;
    void*          original = handle.native;
    CHECK(osal_timer_destroy(&handle) == osal::error_code::not_supported);
    CHECK(handle.native == original);
    CHECK(deletes == before);
    // Model a native callback already dispatched before stop; its context remains valid.
    auto& timer = timers[timer_count - 1];
    timer.callback(timer.argument);
    CHECK(calls.load() == 1U);
}

TEST_CASE("CMSIS2 active period change starts a fresh interval without changing mode")
{
    for (bool periodic : {false, true})
    {
        osal::active_traits::timer_handle_t handle{};
        REQUIRE(osal_timer_create(&handle, "native-test", [](void*) {}, nullptr, 10, periodic).ok());
        REQUIRE(osal_timer_start(&handle).ok());
        const unsigned before = starts;
        REQUIRE(osal_timer_set_period(&handle, 80).ok());
        CHECK(starts == before + 1U);
        const auto& timer = timers[timer_count - 1U];
        CHECK(timer.ticks == 80U);
        CHECK(timer.type == (periodic ? osTimerPeriodic : osTimerOnce));
        CHECK(timer.running);
        REQUIRE(osal_timer_stop(&handle).ok());
        CHECK(osal_timer_destroy(&handle) == osal::error_code::not_supported);
    }
}
#else
TEST_CASE("native contract: semaphore task and ISR give share the ceiling")
{
    for (const unsigned maximum : {1U, 3U})
    {
        osal::active_traits::semaphore_handle_t sem{};
        CHECK(osal_semaphore_create(&sem, maximum + 1U, maximum) == osal::error_code::invalid_argument);
        REQUIRE(osal_semaphore_create(&sem, 0U, maximum).ok());
        for (unsigned i = 0; i < maximum; ++i)
        {
            REQUIRE(osal_semaphore_give(&sem).ok());
        }
        CHECK(osal_semaphore_give_isr(&sem) == osal::error_code::overflow);
        REQUIRE(osal_semaphore_try_take(&sem).ok());
        REQUIRE(osal_semaphore_give_isr(&sem).ok());
        CHECK(osal_semaphore_give(&sem) == osal::error_code::overflow);
        CHECK(osal_semaphore_destroy(&sem).ok());
    }
}

TEST_CASE("native contract: changing a stopped timer does not arm it or change one-shot mode")
{
    static std::atomic<unsigned> calls{0};
    calls.store(0);
    const auto tick_us = osal_clock_tick_period_us();
    REQUIRE(tick_us != 0U);
    const auto                          ticks = static_cast<osal::tick_t>((10000U + tick_us - 1U) / tick_us);
    osal::active_traits::timer_handle_t timer{};
    REQUIRE(osal_timer_create(
                &timer, "native-test", [](void* value) { static_cast<std::atomic<unsigned>*>(value)->fetch_add(1); },
                &calls, ticks, false)
                .ok());
    REQUIRE(osal_timer_set_period(&timer, ticks * 2U).ok());
    CHECK_FALSE(osal_timer_is_active(&timer));
    osal_thread_sleep_ms(40U);
    CHECK(calls.load() == 0U);
    REQUIRE(osal_timer_start(&timer).ok());
    for (unsigned i = 0; i < 200U && calls.load() == 0U; ++i)
    {
        osal_thread_sleep_ms(1U);
    }
    REQUIRE(calls.load() == 1U);
    osal_thread_sleep_ms(60U);
    CHECK(calls.load() == 1U);
    CHECK_FALSE(osal_timer_is_active(&timer));
    CHECK(osal_timer_destroy(&timer).ok());
}
#endif
