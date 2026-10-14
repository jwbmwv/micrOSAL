// SPDX-License-Identifier: Apache-2.0
/// @file test_checked_lifecycle.cpp
/// @brief Checked initialization, ownership retention, and explicit shutdown.
/// @details OSAL_TEST_TIMER_RECLAIM_COMPILE_ONLY validates backend policy without SDK headers.
#if defined(OSAL_TEST_TIMER_RECLAIM_COMPILE_ONLY)
#include <osal/timer.hpp>
#else
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <atomic>
#endif

#if defined(OSAL_BACKEND_THREADX) || defined(OSAL_BACKEND_PX5) || defined(OSAL_BACKEND_MICRIUM) ||   \
    defined(OSAL_BACKEND_CHIBIOS) || defined(OSAL_BACKEND_EMBOS) || defined(OSAL_BACKEND_VXWORKS) || \
    defined(OSAL_BACKEND_NUTTX) || defined(OSAL_BACKEND_CMSIS_RTOS) || defined(OSAL_BACKEND_CMSIS_RTOS2)
static_assert(!osal::timer::can_reclaim_after_start);
#elif defined(OSAL_BACKEND_ZEPHYR)
#if defined(CONFIG_SMP) && CONFIG_SMP
static_assert(!osal::timer::can_reclaim_after_start);
#else
static_assert(osal::timer::can_reclaim_after_start);
#endif
#else
static_assert(osal::timer::can_reclaim_after_start);
#endif

#if !defined(OSAL_TEST_TIMER_RECLAIM_COMPILE_ONLY)
namespace
{
alignas(16) std::uint8_t worker_stack[65536];
alignas(16) std::uint8_t thread_stack[65536];

void no_work(void*) {}
#if defined(OSAL_BACKEND_LINUX) || defined(OSAL_BACKEND_POSIX)
void count_expiry(void* arg)
{
    static_cast<std::atomic<unsigned>*>(arg)->fetch_add(1U);
}
#endif
}  // namespace

#if defined(OSAL_TEST_CHECKED_LIFECYCLE_FAULT_INJECTION)
namespace
{
bool fail_semaphore_destroy{};
bool fail_timer_destroy{};
bool fail_queue_destroy{};
bool fail_work_queue_destroy{};
bool fail_queue_create{};
}  // namespace

extern "C" osal::result checked_real_semaphore_destroy(osal::active_traits::semaphore_handle_t*) noexcept
    asm("__real_osal_semaphore_destroy");
extern "C" osal::result checked_real_timer_destroy(osal::active_traits::timer_handle_t*) noexcept
    asm("__real_osal_timer_destroy");
extern "C" osal::result checked_real_queue_destroy(osal::active_traits::queue_handle_t*) noexcept
    asm("__real_osal_queue_destroy");
extern "C" osal::result checked_real_work_queue_destroy(osal::active_traits::work_queue_handle_t*) noexcept
    asm("__real_osal_work_queue_destroy");
extern "C" osal::result checked_real_queue_create(osal::active_traits::queue_handle_t*, void*, std::size_t,
                                                  std::size_t) noexcept asm("__real_osal_queue_create");
extern "C" osal::result checked_wrap_semaphore_destroy(osal::active_traits::semaphore_handle_t*) noexcept
    asm("__wrap_osal_semaphore_destroy");
extern "C" osal::result checked_wrap_timer_destroy(osal::active_traits::timer_handle_t*) noexcept
    asm("__wrap_osal_timer_destroy");
extern "C" osal::result checked_wrap_queue_destroy(osal::active_traits::queue_handle_t*) noexcept
    asm("__wrap_osal_queue_destroy");
extern "C" osal::result checked_wrap_work_queue_destroy(osal::active_traits::work_queue_handle_t*) noexcept
    asm("__wrap_osal_work_queue_destroy");
extern "C" osal::result checked_wrap_queue_create(osal::active_traits::queue_handle_t*, void*, std::size_t,
                                                  std::size_t) noexcept asm("__wrap_osal_queue_create");

extern "C" osal::result checked_wrap_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
{
    return fail_semaphore_destroy ? osal::result{osal::error_code::unknown} : checked_real_semaphore_destroy(handle);
}
extern "C" osal::result checked_wrap_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
{
    return fail_timer_destroy ? osal::result{osal::error_code::not_supported} : checked_real_timer_destroy(handle);
}
extern "C" osal::result checked_wrap_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
{
    return fail_queue_destroy ? osal::result{osal::error_code::unknown} : checked_real_queue_destroy(handle);
}
extern "C" osal::result checked_wrap_work_queue_destroy(osal::active_traits::work_queue_handle_t* handle) noexcept
{
    return fail_work_queue_destroy ? osal::result{osal::error_code::unknown} : checked_real_work_queue_destroy(handle);
}
extern "C" osal::result checked_wrap_queue_create(osal::active_traits::queue_handle_t* handle, void* storage,
                                                  std::size_t size, std::size_t depth) noexcept
{
    return fail_queue_create ? osal::result{osal::error_code::out_of_resources}
                             : checked_real_queue_create(handle, storage, size, depth);
}
#endif

TEST_CASE("checked lifecycle: initialization errors survive idempotent close")
{
    osal::semaphore semaphore{osal::semaphore_type::counting, 0U, 0U};
    CHECK_FALSE(semaphore.valid());
    CHECK(semaphore.initialization_status() == osal::error_code::invalid_argument);
    CHECK(semaphore.close().ok());
    CHECK(semaphore.initialization_status() == osal::error_code::invalid_argument);
    CHECK(semaphore.checked_give() == osal::error_code::not_initialized);
    CHECK(semaphore.checked_take(osal::milliseconds{0}) == osal::error_code::not_initialized);
    const osal::semaphore_config bad_semaphore_config{osal::semaphore_type::binary, 2U, 1U};
    osal::semaphore              bad_semaphore{bad_semaphore_config};
    CHECK(bad_semaphore.initialization_status() == osal::error_code::invalid_argument);

    osal::thread thread;
    CHECK(thread.initialization_status() == osal::error_code::not_initialized);
    CHECK(thread.create({}) == osal::error_code::invalid_argument);
    CHECK(thread.initialization_status() == osal::error_code::invalid_argument);
    CHECK_FALSE(thread.valid());

    osal::work_queue work{worker_stack, sizeof(worker_stack), 0U};
    CHECK_FALSE(work.valid());
    CHECK(work.initialization_status() == osal::error_code::invalid_argument);
    CHECK(work.shutdown().ok());
    CHECK(work.initialization_status() == osal::error_code::invalid_argument);

    if constexpr (osal::timer::is_supported)
    {
        osal::timer timer{nullptr, nullptr, osal::milliseconds{1}};
        CHECK_FALSE(timer.valid());
        CHECK(timer.initialization_status() == osal::error_code::invalid_argument);
        const osal::timer_config bad_timer_config{no_work, nullptr, osal::milliseconds{0}};
        osal::timer              bad_timer{bad_timer_config};
        CHECK(bad_timer.initialization_status() == osal::error_code::invalid_argument);
        CHECK(timer.close().ok());
        CHECK(timer.initialization_status() == osal::error_code::invalid_argument);
    }
    else
    {
        osal::timer timer{no_work, nullptr, osal::milliseconds{1}};
        CHECK(timer.initialization_status() == osal::error_code::not_supported);
        CHECK(timer.close().ok());
    }
}

TEST_CASE("checked lifecycle: successful close is idempotent and retains initialization status")
{
    osal::semaphore semaphore{osal::semaphore_type::binary, 0U};
    REQUIRE(semaphore.initialization_status().ok());
    CHECK(semaphore.close().ok());
    CHECK(semaphore.close().ok());
    CHECK_FALSE(semaphore.valid());
    CHECK(semaphore.initialization_status().ok());

    osal::queue<int, 1> queue;
    REQUIRE(queue.initialization_status().ok());
    CHECK(queue.close().ok());
    CHECK(queue.close().ok());
    CHECK_FALSE(queue.valid());
    CHECK(queue.initialization_status().ok());
    CHECK(queue.send(1) == osal::error_code::not_initialized);

    osal::work_queue work{worker_stack, sizeof(worker_stack), 1U};
    REQUIRE(work.initialization_status().ok());
    CHECK(work.shutdown().ok());
    CHECK(work.close().ok());
    CHECK_FALSE(work.valid());
    CHECK(work.initialization_status().ok());
    CHECK(work.submit(no_work) == osal::error_code::not_initialized);

    if constexpr (osal::timer::is_supported)
    {
        osal::timer timer{no_work, nullptr, osal::milliseconds{1}};
        REQUIRE(timer.initialization_status().ok());
        CHECK(timer.close().ok());
        CHECK(timer.close().ok());
        CHECK_FALSE(timer.valid());
        CHECK(timer.initialization_status().ok());
        CHECK(timer.start() == osal::error_code::not_initialized);
    }
}

TEST_CASE("checked lifecycle: timer constructors reject invalid callbacks and periods before backend calls")
{
    for (const auto period : {osal::milliseconds{0}, osal::milliseconds{-1}})
    {
        osal::timer direct{no_work, nullptr, period};
        CHECK_FALSE(direct.valid());
        CHECK(direct.initialization_status() == osal::error_code::invalid_argument);
        CHECK(direct.close().ok());
        CHECK(direct.initialization_status() == osal::error_code::invalid_argument);

        const osal::timer_config config{no_work, nullptr, period};
        osal::timer              configured{config};
        CHECK_FALSE(configured.valid());
        CHECK(configured.initialization_status() == osal::error_code::invalid_argument);
    }

    osal::timer direct_null{nullptr, nullptr, osal::milliseconds{10}};
    CHECK_FALSE(direct_null.valid());
    CHECK(direct_null.initialization_status() == osal::error_code::invalid_argument);
    const osal::timer_config null_config{nullptr, nullptr, osal::milliseconds{10}};
    osal::timer              configured_null{null_config};
    CHECK_FALSE(configured_null.valid());
    CHECK(configured_null.initialization_status() == osal::error_code::invalid_argument);

    osal::timer timer{no_work, nullptr, osal::milliseconds{10}};
    CHECK(timer.set_period(osal::milliseconds{0}) == osal::error_code::invalid_argument);
    CHECK(timer.set_period(osal::milliseconds{-1}) == osal::error_code::invalid_argument);
    CHECK_FALSE(timer.is_active());
    if constexpr (osal::timer::is_supported)
    {
        CHECK(timer.valid());
        CHECK(timer.initialization_status().ok());
    }
    else
    {
        CHECK(timer.initialization_status() == osal::error_code::not_supported);
    }
}

TEST_CASE("checked lifecycle: semaphore constructors reject unknown semaphore types")
{
    constexpr auto  invalid_type = static_cast<osal::semaphore_type>(255U);
    osal::semaphore direct{invalid_type, 0U, 2U};
    CHECK_FALSE(direct.valid());
    CHECK(direct.initialization_status() == osal::error_code::invalid_argument);
    CHECK(direct.close().ok());
    const osal::semaphore_config config{invalid_type, 0U, 2U};
    osal::semaphore              configured{config};
    CHECK_FALSE(configured.valid());
    CHECK(configured.initialization_status() == osal::error_code::invalid_argument);
}

TEST_CASE("checked lifecycle: finite timeout conversion never becomes an infinite wait")
{
    CHECK(osal::clock_utils::ms_to_ticks(osal::milliseconds{0}) == osal::NO_WAIT);
    CHECK(osal::clock_utils::ms_to_ticks(osal::milliseconds::max()) < osal::WAIT_FOREVER);
    osal::semaphore semaphore{osal::semaphore_type::binary, 1U};
    REQUIRE(semaphore.valid());
    CHECK(semaphore.checked_take(osal::milliseconds{0}).ok());
    CHECK(semaphore.checked_give().ok());
    if constexpr (osal::timed_semaphore_backend<osal::active_backend>)
    {
        CHECK(semaphore.checked_take(osal::milliseconds::max()).ok());
    }
    else
    {
        CHECK(semaphore.checked_take(osal::milliseconds{1}) == osal::error_code::not_supported);
    }

    if constexpr (!osal::active_capabilities::has_timed_mutex)
    {
        osal::mutex mutex;
        REQUIRE(mutex.valid());
        CHECK(mutex.checked_lock(osal::milliseconds{1}) == osal::error_code::not_supported);
    }
}

TEST_CASE("checked lifecycle: semaphore exposes overflow, nonblocking failure, and timeout")
{
    osal::semaphore semaphore{osal::semaphore_type::binary, 1U};
    REQUIRE(semaphore.valid());
    CHECK(semaphore.checked_give() == osal::error_code::overflow);
    CHECK(semaphore.checked_take(osal::milliseconds{-1}).ok());
    const auto empty = semaphore.checked_take(osal::milliseconds{0});
    CHECK((empty == osal::error_code::would_block || empty == osal::error_code::timeout));
    if constexpr (osal::timed_semaphore_backend<osal::active_backend>)
    {
        CHECK(semaphore.checked_take(osal::milliseconds{2}) == osal::error_code::timeout);
    }
}

TEST_CASE("checked lifecycle: second thread create cannot replace a live handle")
{
    if constexpr (!osal::thread::is_supported)
    {
        return;
    }
    osal::thread        thread;
    osal::thread_config config{};
    config.entry       = no_work;
    config.stack       = thread_stack;
    config.stack_bytes = sizeof(thread_stack);
    REQUIRE(thread.create(config).ok());
    CHECK(thread.create({}) == osal::error_code::already_exists);
    CHECK(thread.valid());
    CHECK(thread.initialization_status().ok());
    const auto joined = thread.join();
    if (joined == osal::error_code::not_supported)
    {
        CHECK(thread.detach().ok());
    }
    else
    {
        CHECK(joined.ok());
    }
}

#if defined(OSAL_BACKEND_LINUX) || defined(OSAL_BACKEND_POSIX)
TEST_CASE("checked lifecycle: changing inactive timer period does not arm it")
{
    std::atomic<unsigned> expiries{0U};
    osal::timer           timer{count_expiry, &expiries, osal::milliseconds{10}};
    REQUIRE(timer.valid());
    CHECK_FALSE(timer.is_active());
    REQUIRE(timer.set_period(osal::milliseconds{2}).ok());
    CHECK_FALSE(timer.is_active());
    osal::thread::sleep_for(osal::milliseconds{15});
    CHECK(expiries.load() == 0U);
    REQUIRE(timer.start().ok());
    osal::thread::sleep_for(osal::milliseconds{30});
    CHECK(expiries.load() == 1U);
    CHECK_FALSE(timer.is_active());
    REQUIRE(timer.stop().ok());
    REQUIRE(timer.set_period(osal::milliseconds{3}).ok());
    CHECK_FALSE(timer.is_active());
    CHECK(timer.close().ok());
}
#endif

#if defined(OSAL_TEST_CHECKED_LIFECYCLE_FAULT_INJECTION)
TEST_CASE("checked lifecycle: rejected destruction retains ownership and live diagnostics")
{
    osal::semaphore semaphore{osal::semaphore_type::binary, 1U};
    REQUIRE(semaphore.valid());
    auto before            = osal::diagnostics(osal::diagnostic_resource::semaphore).snapshot();
    fail_semaphore_destroy = true;
    CHECK(semaphore.close() == osal::error_code::unknown);
    CHECK(semaphore.valid());
    CHECK(semaphore.checked_take(osal::milliseconds{0}).ok());
    CHECK(osal::diagnostics(osal::diagnostic_resource::semaphore).snapshot().in_use == before.in_use);
    fail_semaphore_destroy = false;
    CHECK(semaphore.close().ok());

    osal::queue<int, 1> queue;
    REQUIRE(queue.valid());
    fail_queue_destroy = true;
    CHECK(queue.close() == osal::error_code::unknown);
    CHECK(queue.valid());
    CHECK(queue.try_send(1));
    fail_queue_destroy = false;
    CHECK(queue.close().ok());

    osal::work_queue work{worker_stack, sizeof(worker_stack), 1U};
    REQUIRE(work.valid());
    fail_work_queue_destroy = true;
    CHECK(work.shutdown() == osal::error_code::unknown);
    CHECK(work.valid());
    fail_work_queue_destroy = false;
    CHECK(work.submit(no_work).ok());
    CHECK(work.shutdown().ok());

    osal::timer timer{no_work, nullptr, osal::milliseconds{10}};
    REQUIRE(timer.valid());
    fail_timer_destroy = true;
    CHECK(timer.close() == osal::error_code::not_supported);
    CHECK(timer.valid());
    CHECK(timer.initialization_status().ok());
    fail_timer_destroy = false;
    CHECK(timer.close().ok());
}

TEST_CASE("checked lifecycle: backend creation failure is retained")
{
    fail_queue_create = true;
    osal::queue<int, 1> queue;
    fail_queue_create = false;
    CHECK_FALSE(queue.valid());
    CHECK(queue.initialization_status() == osal::error_code::out_of_resources);
    CHECK(queue.close().ok());
    CHECK(queue.initialization_status() == osal::error_code::out_of_resources);
}
#endif
#endif
