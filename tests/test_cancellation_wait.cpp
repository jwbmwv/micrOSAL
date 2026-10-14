// SPDX-License-Identifier: Apache-2.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/object_wait_set.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

#ifdef OSAL_TEST_WAIT_SIGNAL_FAULTS
namespace
{
std::atomic<bool>     fail_create{false};
std::atomic<bool>     fail_take{false};
std::atomic<bool>     fail_give{false};
std::atomic<unsigned> forever_takes{0U};
}  // namespace
#ifdef OSAL_TEST_WAIT_SIGNAL_DESTROY_FAULTS
namespace
{
std::atomic<bool> fail_destroy{false};
}
#endif
// GNU ld --wrap requires these symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C"
{
#ifdef OSAL_TEST_WAIT_SIGNAL_DESTROY_FAULTS
    osal::result __real_osal_semaphore_destroy(osal::active_traits::semaphore_handle_t*) noexcept;
    osal::result __wrap_osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        const auto status = __real_osal_semaphore_destroy(handle);
        return status.ok() && fail_destroy.load() ? osal::result{osal::error_code::unknown} : status;
    }
#endif
    osal::result __real_osal_semaphore_create(osal::active_traits::semaphore_handle_t*, unsigned, unsigned) noexcept;
    osal::result __real_osal_semaphore_take(osal::active_traits::semaphore_handle_t*, osal::tick_t) noexcept;
    osal::result __real_osal_semaphore_give(osal::active_traits::semaphore_handle_t*) noexcept;
    osal::result __wrap_osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial,
                                              unsigned maximum) noexcept
    {
        return fail_create.load() ? osal::result{osal::error_code::out_of_resources}
                                  : __real_osal_semaphore_create(handle, initial, maximum);
    }
    osal::result __wrap_osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                            osal::tick_t                             ticks) noexcept
    {
        if (ticks == osal::WAIT_FOREVER)
        {
            forever_takes.fetch_add(1U);
        }
        return fail_take.load() ? osal::result{osal::error_code::unknown} : __real_osal_semaphore_take(handle, ticks);
    }
    osal::result __wrap_osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return fail_give.load() ? osal::result{osal::error_code::unknown} : __real_osal_semaphore_give(handle);
    }
}
// NOLINTEND(bugprone-reserved-identifier)
#endif

#ifdef OSAL_TEST_STOP_SOURCE_FAULTS
namespace
{
std::atomic<bool> fail_source_mutex_create{false};
std::atomic<bool> fail_source_condvar_create{false};
std::atomic<bool> fail_source_lock{false};
std::atomic<bool> fail_source_notify{false};
}  // namespace
// GNU ld --wrap requires these symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C"
{
    osal::result __real_osal_mutex_create_config(osal::active_traits::mutex_handle_t*,
                                                 const osal::mutex_config*) noexcept;
    osal::result __real_osal_condvar_create(osal::active_traits::condvar_handle_t*) noexcept;
    osal::result __real_osal_mutex_lock(osal::active_traits::mutex_handle_t*, osal::tick_t) noexcept;
    osal::result __real_osal_condvar_notify_all(osal::active_traits::condvar_handle_t*) noexcept;
    osal::result __wrap_osal_mutex_create_config(osal::active_traits::mutex_handle_t* handle,
                                                 const osal::mutex_config*            config) noexcept
    {
        return fail_source_mutex_create.load() ? osal::result{osal::error_code::out_of_resources}
                                               : __real_osal_mutex_create_config(handle, config);
    }
    osal::result __wrap_osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        return fail_source_condvar_create.load() ? osal::result{osal::error_code::unknown}
                                                 : __real_osal_condvar_create(handle);
    }
    osal::result __wrap_osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t ticks) noexcept
    {
        return fail_source_lock.load() ? osal::result{osal::error_code::permission_denied}
                                       : __real_osal_mutex_lock(handle, ticks);
    }
    osal::result __wrap_osal_condvar_notify_all(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        return fail_source_notify.load() ? osal::result{osal::error_code::unknown}
                                         : __real_osal_condvar_notify_all(handle);
    }
}
// NOLINTEND(bugprone-reserved-identifier)
#endif

TEST_CASE("stop: borrowed tokens and synchronous bounded callbacks")
{
    osal::stop_source source;
    REQUIRE(source.initialization_status().ok());
    REQUIRE(source.valid());
    const auto token = source.get_token();
    CHECK(token.stop_possible());
    CHECK_FALSE(token.stop_requested());
    CHECK_FALSE(osal::stop_token{}.stop_possible());
    CHECK(token.status().ok());
    CHECK(osal::stop_token{}.status().ok());
    std::atomic<unsigned> calls{0U};
    auto                  increment = [](void* p) noexcept { static_cast<std::atomic<unsigned>*>(p)->fetch_add(1U); };
    {
        osal::stop_callback removed{token, increment, &calls};
        REQUIRE(removed.status().ok());
    }
    osal::stop_callback callback{token, increment, &calls};
    REQUIRE(callback.status().ok());
    CHECK(source.request_stop());
    CHECK_FALSE(source.request_stop());
    CHECK(token.stop_requested());
    CHECK(calls.load() == 1U);
    osal::stop_callback late{token, increment, &calls};
    CHECK(late.status().ok());
    CHECK(calls.load() == 2U);
    osal::stop_callback invalid{token, nullptr, nullptr};
    CHECK(invalid.status() == osal::error_code::invalid_argument);
}

TEST_CASE("stop: checked request separates first and repeated requests")
{
    osal::stop_source source;
    REQUIRE(source.valid());
    bool first = false;
    CHECK(source.request_stop(first).ok());
    CHECK(first);
    first = true;
    CHECK(source.request_stop(first).ok());
    CHECK_FALSE(first);
    CHECK(source.status().ok());
}

TEST_CASE("stop: callback capacity fails explicitly")
{
    osal::stop_source source;
    using storage_type = std::aligned_storage_t<sizeof(osal::stop_callback), alignof(osal::stop_callback)>;
    std::array<storage_type, OSAL_STOP_SOURCE_MAX_CALLBACKS>         storage{};
    std::array<osal::stop_callback*, OSAL_STOP_SOURCE_MAX_CALLBACKS> callbacks{};
    for (std::size_t i = 0U; i < callbacks.size(); ++i)
    {
        callbacks[i] = new (&storage[i]) osal::stop_callback{source.get_token(), [](void*) noexcept {}, nullptr};
        CHECK(callbacks[i]->status().ok());
    }
    osal::stop_callback excess{source.get_token(), [](void*) noexcept {}, nullptr};
    CHECK(excess.status() == osal::error_code::overflow);
    if constexpr (osal::wait_signal::is_supported)
    {
        osal::wait_signal signal;
        REQUIRE(signal.valid());
        bool probed = false;
        CHECK(osal::wait_until_ready(
                  signal,
                  [&]() noexcept
                  {
                      probed = true;
                      return true;
                  },
                  osal::milliseconds{0}, source.get_token()) == osal::error_code::overflow);
        CHECK_FALSE(probed);
    }
    for (auto* callback : callbacks)
    {
        callback->~stop_callback();
    }
}

TEST_CASE("stop: unregister and request races protect callback lifetime")
{
    for (unsigned repeat = 0U; repeat < 100U; ++repeat)
    {
        osal::stop_source     source;
        std::atomic<bool>     go{false};
        std::atomic<unsigned> calls{0U};
        std::thread           requester{[&]
                              {
                                  while (!go.load())
                                  {
                                      std::this_thread::yield();
                                  }
                                  (void)source.request_stop();
                              }};
        {
            osal::stop_callback callback{source.get_token(), [](void* p) noexcept
                                         { static_cast<std::atomic<unsigned>*>(p)->fetch_add(1U); }, &calls};
            go.store(true);
        }
        const unsigned after_destruction = calls.load();
        requester.join();
        CHECK(calls.load() == after_destruction);
        CHECK(calls.load() <= 1U);
    }
}

TEST_CASE("stop: unregister waits for in-flight callback without holding source lock")
{
    osal::stop_source source;
    struct context
    {
        std::atomic<bool> entered{false};
        std::atomic<bool> resume{false};
    } ctx;
    alignas(osal::stop_callback) std::array<std::byte, sizeof(osal::stop_callback)> storage{};
    auto*       callback = new (storage.data()) osal::stop_callback{source.get_token(),
                                                              [](void* p) noexcept
                                                              {
                                                                  auto& state = *static_cast<context*>(p);
                                                                  state.entered.store(true);
                                                                  // Model a paused callback executor to test unregister
                                                                  // synchronization.
                                                                  while (!state.resume.load())
                                                                  {
                                                                      std::this_thread::yield();
                                                                  }
                                                              },
                                                              &ctx};
    std::thread requester{[&] { (void)source.request_stop(); }};
    while (!ctx.entered.load())
    {
        std::this_thread::yield();
    }
    std::atomic<bool> destroyed{false};
    CHECK(callback->close(osal::milliseconds{0}) == osal::error_code::timeout);
    CHECK(source.status().ok());
    std::thread unregister{[&]
                           {
                               callback->~stop_callback();
                               destroyed.store(true);
                           }};
    std::this_thread::sleep_for(10ms);
    CHECK_FALSE(destroyed.load());
    // This obtains the source lock while unregister awaits callback completion.
    CHECK_FALSE(source.request_stop());
    ctx.resume.store(true);
    requester.join();
    unregister.join();
    CHECK(destroyed.load());
}

TEST_CASE("stop: checked unregister is idempotent")
{
    osal::stop_source source;
    REQUIRE(source.valid());
    unsigned            calls = 0U;
    osal::stop_callback callback{source.get_token(), [](void* p) noexcept { ++*static_cast<unsigned*>(p); }, &calls};
    REQUIRE(callback.status().ok());
    CHECK(callback.close(osal::milliseconds{0}).ok());
    CHECK(callback.close().ok());
    CHECK(source.request_stop());
    CHECK(calls == 0U);
}

TEST_CASE("stop: distinct callback close waiters cannot steal completion wakes")
{
    for (unsigned repeat = 0U; repeat < 10U; ++repeat)
    {
        osal::stop_source source;
        REQUIRE(source.valid());
        struct context
        {
            std::atomic<bool> entered{false};
            std::atomic<bool> resume{false};
        } first, second;
        auto invoke = [](void* p) noexcept
        {
            auto& state = *static_cast<context*>(p);
            state.entered.store(true);
            while (!state.resume.load())
            {
                std::this_thread::yield();
            }
        };
        osal::stop_callback first_callback{source.get_token(), invoke, &first};
        osal::stop_callback second_callback{source.get_token(), invoke, &second};
        REQUIRE(first_callback.status().ok());
        REQUIRE(second_callback.status().ok());
        osal::result first_closed{osal::error_code::unknown};
        osal::result second_closed{osal::error_code::unknown};
        std::thread  requester{[&] { (void)source.request_stop(); }};
        while (!second.entered.load())
        {
            std::this_thread::yield();
        }
        constexpr osal::milliseconds timeout{std::is_same_v<osal::active_backend, osal::backend_baremetal> ? -1 : 2000};
        std::thread                  second_closer{[&] { second_closed = second_callback.close(timeout); }};
        std::this_thread::sleep_for(1ms);
        second.resume.store(true);
        while (!first.entered.load())
        {
            std::this_thread::yield();
        }
        std::thread first_closer{[&] { first_closed = first_callback.close(timeout); }};
        std::this_thread::sleep_for(1ms);
        first.resume.store(true);
        requester.join();
        first_closer.join();
        second_closer.join();
        CHECK(first_closed.ok());
        CHECK(second_closed.ok());
        CHECK(source.status().ok());
    }
}

#ifdef OSAL_TEST_STOP_SOURCE_FAULTS
TEST_CASE("stop: initialization and synchronization failures are explicit")
{
    fail_source_mutex_create.store(true);
    {
        osal::stop_source source;
        CHECK(source.initialization_status() == osal::error_code::out_of_resources);
        CHECK_FALSE(source.valid());
        CHECK(source.get_token().status() == osal::error_code::out_of_resources);
        bool first = true;
        CHECK(source.request_stop(first) == osal::error_code::out_of_resources);
        CHECK_FALSE(first);
        osal::stop_callback callback{source.get_token(), [](void*) noexcept {}, nullptr};
        CHECK(callback.status() == osal::error_code::out_of_resources);
        if constexpr (osal::wait_signal::is_supported)
        {
            osal::wait_signal signal;
            REQUIRE(signal.valid());
            bool probed = false;
            CHECK(osal::wait_until_ready(
                      signal,
                      [&]() noexcept
                      {
                          probed = true;
                          return true;
                      },
                      osal::milliseconds{0}, source.get_token()) == osal::error_code::out_of_resources);
            CHECK_FALSE(probed);
        }
    }
    fail_source_mutex_create.store(false);
    fail_source_condvar_create.store(true);
    {
        osal::stop_source source;
        CHECK(source.initialization_status() == osal::error_code::unknown);
        CHECK(source.status() == osal::error_code::unknown);
        osal::stop_callback callback{source.get_token(), [](void*) noexcept {}, nullptr};
        CHECK(callback.status() == osal::error_code::unknown);
    }
    fail_source_condvar_create.store(false);
    {
        osal::stop_source source;
        REQUIRE(source.valid());
        fail_source_lock.store(true);
        osal::stop_callback callback{source.get_token(), [](void*) noexcept {}, nullptr};
        CHECK(callback.status() == osal::error_code::permission_denied);
        fail_source_lock.store(false);
        CHECK(source.status() == osal::error_code::permission_denied);
        bool first = true;
        CHECK(source.request_stop(first) == osal::error_code::permission_denied);
        CHECK_FALSE(first);
    }
    {
        osal::stop_source source;
        REQUIRE(source.valid());
        osal::stop_callback callback{source.get_token(), [](void*) noexcept {}, nullptr};
        REQUIRE(callback.status().ok());
        fail_source_lock.store(true);
        CHECK(callback.close() == osal::error_code::permission_denied);
        fail_source_lock.store(false);
        CHECK(callback.close().ok());
    }
    {
        osal::stop_source source;
        REQUIRE(source.valid());
        osal::stop_callback callback{source.get_token(), [](void*) noexcept {}, nullptr};
        REQUIRE(callback.status().ok());
        fail_source_notify.store(true);
        bool first = false;
        CHECK(source.request_stop(first) == osal::error_code::unknown);
        CHECK(first);
        CHECK(source.stop_requested());
        CHECK(source.status() == osal::error_code::unknown);
        fail_source_notify.store(false);
        CHECK(callback.close().ok());
    }
}

TEST_CASE("stop: failed source-lock reacquisition completes unregister handoff")
{
    osal::stop_source source;
    REQUIRE(source.valid());
    struct context
    {
        std::atomic<bool> entered{false};
        std::atomic<bool> resume{false};
    } ctx;
    alignas(osal::stop_callback) std::array<std::byte, sizeof(osal::stop_callback)> storage{};
    auto* callback = new (storage.data()) osal::stop_callback{source.get_token(),
                                                              [](void* p) noexcept
                                                              {
                                                                  auto& state = *static_cast<context*>(p);
                                                                  state.entered.store(true);
                                                                  while (!state.resume.load())
                                                                  {
                                                                      std::this_thread::yield();
                                                                  }
                                                                  fail_source_lock.store(true);
                                                              },
                                                              &ctx};
    REQUIRE(callback->status().ok());
    std::thread requester{[&] { (void)source.request_stop(); }};
    while (!ctx.entered.load())
    {
        std::this_thread::yield();
    }
    osal::result close_result{osal::error_code::unknown};
    std::thread  unregister{[&] { close_result = callback->close(); }};
    std::this_thread::sleep_for(10ms);
    ctx.resume.store(true);
    requester.join();
    fail_source_lock.store(false);
    unregister.join();
    CHECK(close_result.ok());
    CHECK(source.status() == osal::error_code::permission_denied);
    callback->~stop_callback();
}
#endif

TEST_CASE("stop: registration racing request invokes callback exactly once")
{
    for (unsigned repeat = 0U; repeat < 100U; ++repeat)
    {
        osal::stop_source     source;
        std::atomic<unsigned> calls{0U};
        std::thread           requester{[&] { (void)source.request_stop(); }};
        osal::stop_callback   callback{source.get_token(), [](void* p) noexcept
                                     { static_cast<std::atomic<unsigned>*>(p)->fetch_add(1U); }, &calls};
        requester.join();
        CHECK(calls.load() == 1U);
    }
}

TEST_CASE("stop: callback may register another callback without source lock deadlock")
{
    osal::stop_source source;
    struct context
    {
        osal::stop_source& source;
        unsigned           calls = 0U;
    } ctx{source};
    osal::stop_callback callback{source.get_token(),
                                 [](void* p) noexcept
                                 {
                                     auto&               state = *static_cast<context*>(p);
                                     osal::stop_callback nested{state.source.get_token(), [](void* arg) noexcept
                                                                { ++static_cast<context*>(arg)->calls; }, p};
                                 },
                                 &ctx};
    CHECK(source.request_stop());
    CHECK(ctx.calls == 1U);
}

TEST_CASE("wait signal: unsupported backends report explicit capability failure")
{
    if constexpr (!osal::wait_signal::is_supported)
    {
        const auto        before = osal::diagnostics(osal::diagnostic_resource::semaphore).snapshot();
        osal::wait_signal signal;
        CHECK_FALSE(signal.valid());
        CHECK(signal.status() == osal::error_code::not_supported);
        CHECK(signal.notify() == osal::error_code::not_supported);
        CHECK(signal.notify_from_isr() == osal::error_code::not_supported);
        bool probed = false;
        CHECK(osal::wait_until_ready(signal,
                                     [&]() noexcept
                                     {
                                         probed = true;
                                         return true;
                                     }) == osal::error_code::not_supported);
        CHECK_FALSE(probed);
        osal::object_wait_set objects;
        std::size_t           count = 1U;
        CHECK(objects.wait(signal, nullptr, 0U, count) == osal::error_code::not_supported);
        CHECK(count == 0U);
        const auto after = osal::diagnostics(osal::diagnostic_resource::semaphore).snapshot();
        CHECK(after.in_use == before.in_use);
        CHECK(after.attempts == before.attempts);
    }
    else
    {
        osal::wait_signal::require_support();
        osal::wait_signal signal;
        CHECK(signal.valid());
    }
}

TEST_SUITE("native event-driven waits" * doctest::skip(!osal::wait_signal::is_supported))
{
    TEST_CASE("event wait: producer mutation wakes multiple-object readiness")
    {
        osal::wait_signal     signal;
        osal::queue<int, 2>   first;
        osal::queue<int, 2>   second;
        osal::object_wait_set objects;
        REQUIRE(signal.valid());
        REQUIRE(first.valid());
        REQUIRE(second.valid());
        REQUIRE(objects.add(first, 10).ok());
        REQUIRE(objects.add(second, 20).ok());
        std::atomic<bool> waiting{false};
        osal::result      status{osal::error_code::unknown};
        int               id      = 0;
        std::size_t       n_ready = 0U;
        std::thread       consumer{[&]
                             {
                                 waiting.store(true);
                                 status = objects.wait(signal, &id, 1U, n_ready, osal::milliseconds{2000});
                             }};
        while (!waiting.load())
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(10ms);
        CHECK(second.send(42).ok());
        CHECK(signal.notify().ok());
        consumer.join();
        CHECK(status.ok());
        CHECK(n_ready == 1U);
        CHECK(id == 20);
        CHECK_FALSE(second.empty());
        CHECK(first.send(7).ok());
        CHECK(signal.notify().ok());
        std::array<int, 1> truncated{};
        CHECK(objects.wait(signal, std::span<int>{truncated}, n_ready, osal::milliseconds{0}).ok());
        CHECK(n_ready == 1U);
        CHECK(truncated[0] == 10);
        CHECK(objects.wait(signal, nullptr, 0U, n_ready, osal::milliseconds{0}).ok());
        CHECK(n_ready == 0U);
        CHECK(objects.wait(truncated.data(), truncated.size(), n_ready, osal::milliseconds{0}).ok());
        CHECK(n_ready == 1U);
    }

    TEST_CASE("event wait: notification in probe-to-block window cannot be lost")
    {
        osal::wait_signal signal;
        REQUIRE(signal.valid());
        bool       ready  = false;
        unsigned   probes = 0U;
        const auto r      = osal::wait_until_ready(
            signal,
            [&]() noexcept
            {
                ++probes;
                if (!ready)
                {
                    ready = true;
                    (void)signal.notify();
                    return false;
                }
                return true;
            },
            osal::milliseconds{100});
        CHECK(r.ok());
        CHECK(probes == 2U);
    }

    TEST_CASE("event wait: cancellation wakes indefinite native semaphore wait")
    {
        osal::wait_signal     signal;
        osal::stop_source     source;
        osal::object_wait_set objects;
        REQUIRE(signal.valid());
        std::atomic<bool> entered{false};
        osal::result      status{osal::error_code::unknown};
#ifdef OSAL_TEST_WAIT_SIGNAL_FAULTS
        forever_takes.store(0U);
#endif
        std::thread consumer{[&]
                             {
                                 entered.store(true);
                                 std::size_t count = 0U;
                                 status            = objects.wait(signal, nullptr, 0U, count, osal::milliseconds{-1},
                                                                  source.get_token());
                             }};
        while (!entered.load())
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(20ms);
#ifdef OSAL_TEST_WAIT_SIGNAL_FAULTS
        CHECK(forever_takes.load() == 1U);
#endif
        const auto started = std::chrono::steady_clock::now();
        CHECK(source.request_stop());
        consumer.join();
        CHECK(status.code() == osal::error_code::cancelled);
        CHECK(std::chrono::steady_clock::now() - started < 500ms);
        CHECK(osal::wait_until_ready(
                  signal, []() noexcept { return true; }, osal::milliseconds{-1}, source.get_token()) ==
              osal::error_code::cancelled);
    }

    TEST_CASE("event wait: cancellation is registered before the first probe")
    {
        osal::wait_signal signal;
        osal::stop_source source;
        REQUIRE(signal.valid());
        unsigned   probes = 0U;
        const auto r      = osal::wait_until_ready(
            signal,
            [&]() noexcept
            {
                ++probes;
                (void)source.request_stop();
                return false;
            },
            osal::milliseconds{-1}, source.get_token());
        CHECK(r == osal::error_code::cancelled);
        CHECK(probes == 1U);
    }

    TEST_CASE("event wait: finite monotonic deadline survives repeated unrelated wakes")
    {
        osal::wait_signal signal;
        REQUIRE(signal.valid());
        std::atomic<bool> done{false};
        std::thread       producer{[&]
                             {
                                 while (!done.load())
                                 {
                                     (void)signal.notify();
                                     std::this_thread::sleep_for(1ms);
                                 }
                             }};
        const auto        started = std::chrono::steady_clock::now();
        const auto        r = osal::wait_until_ready(signal, []() noexcept { return false; }, osal::milliseconds{40});
        const auto        elapsed = std::chrono::steady_clock::now() - started;
        done.store(true);
        producer.join();
        CHECK(r == osal::error_code::timeout);
        CHECK(elapsed >= 35ms);
        CHECK(elapsed < 500ms);
        CHECK(osal::wait_until_ready(
                  signal, []() noexcept { return false; }, osal::milliseconds{0}) == osal::error_code::timeout);
    }

    TEST_CASE("event wait: exclusive ownership and coalescing are explicit")
    {
        osal::wait_signal signal;
        REQUIRE(signal.valid());
        CHECK(signal.begin_wait().ok());
        CHECK(osal::wait_until_ready(signal, []() noexcept { return true; }) == osal::error_code::already_exists);
        signal.end_wait();
        for (unsigned i = 0U; i < 100U; ++i)
        {
            CHECK(signal.notify().ok());
        }
        CHECK(signal.generation() == 100U);
        if constexpr (!osal::wait_signal::isr_notification_supported)
        {
            CHECK(signal.notify_from_isr() == osal::error_code::not_supported);
        }
    }

    TEST_CASE("event wait: native semaphore ownership is counted globally")
    {
        auto&      counters = osal::diagnostics(osal::diagnostic_resource::semaphore);
        const auto before   = counters.snapshot();
        {
            osal::wait_signal first;
            osal::wait_signal second;
            REQUIRE(first.valid());
            REQUIRE(second.valid());
            const auto created = counters.snapshot();
            CHECK(created.in_use == before.in_use + 2U);
            CHECK(created.attempts == before.attempts + 2U);
            CHECK(created.successes == before.successes + 2U);
            CHECK(created.high_water >= created.in_use);
            CHECK(first.notify().ok());
            if constexpr (!osal::wait_signal::isr_notification_supported)
            {
                CHECK(first.notify_from_isr() == osal::error_code::not_supported);
            }
            CHECK(counters.snapshot().attempts == created.attempts);
        }
        const auto destroyed = counters.snapshot();
        CHECK(destroyed.in_use == before.in_use);
        CHECK(destroyed.attempts == before.attempts + 4U);
        CHECK(destroyed.successes == before.successes + 4U);
    }

#ifdef OSAL_TEST_WAIT_SIGNAL_FAULTS
    TEST_CASE("event wait: finite deadline bounds backend notification failure")
    {
        osal::wait_signal signal;
        REQUIRE(signal.valid());
        std::atomic<bool> entered{false};
        osal::result      status{osal::error_code::ok};
        std::thread       consumer{[&]
                             {
                                 status = osal::wait_until_ready(
                                     signal,
                                     [&]() noexcept
                                     {
                                         entered.store(true);
                                         return false;
                                     },
                                     osal::milliseconds{80});
                             }};
        while (!entered.load())
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(10ms);
        fail_give.store(true);
        CHECK(signal.notify() == osal::error_code::unknown);
        fail_give.store(false);
        consumer.join();
        CHECK(status == osal::error_code::unknown);
        CHECK(signal.status() == osal::error_code::unknown);
    }

#ifdef OSAL_TEST_WAIT_SIGNAL_DESTROY_FAULTS
    TEST_CASE("event wait: failed native destroy does not release diagnostics")
    {
        auto&      counters = osal::diagnostics(osal::diagnostic_resource::semaphore);
        const auto before   = counters.snapshot();
        {
            osal::wait_signal signal;
            REQUIRE(signal.valid());
            fail_destroy.store(true);
        }
        fail_destroy.store(false);
        const auto failed = counters.snapshot();
        CHECK(failed.in_use == before.in_use + 1U);
        CHECK(failed.attempts == before.attempts + 2U);
        CHECK(failed.failures == before.failures + 1U);
        // The mock reported failure after native cleanup; balance the fixture gauge.
        counters.release();
    }
#endif
    TEST_CASE("event wait: semaphore failures are returned without fallback")
    {
        auto&      counters = osal::diagnostics(osal::diagnostic_resource::semaphore);
        const auto before   = counters.snapshot();
        fail_create.store(true);
        {
            osal::wait_signal signal;
            CHECK_FALSE(signal.valid());
            CHECK(signal.status() == osal::error_code::out_of_resources);
            CHECK(signal.notify() == osal::error_code::out_of_resources);
            CHECK(osal::wait_until_ready(signal, []() noexcept { return true; }) == osal::error_code::out_of_resources);
        }
        fail_create.store(false);
        const auto failed = counters.snapshot();
        CHECK(failed.in_use == before.in_use);
        CHECK(failed.attempts == before.attempts + 1U);
        CHECK(failed.failures == before.failures + 1U);
        osal::wait_signal signal;
        REQUIRE(signal.valid());
        fail_take.store(true);
        CHECK(osal::wait_until_ready(signal, []() noexcept { return false; }) == osal::error_code::unknown);
        fail_take.store(false);
        fail_give.store(true);
        CHECK(signal.notify() == osal::error_code::unknown);
        fail_give.store(false);
        CHECK(signal.status() == osal::error_code::unknown);
        CHECK(osal::wait_until_ready(signal, []() noexcept { return true; }) == osal::error_code::unknown);
    }
#endif
}  // TEST_SUITE
