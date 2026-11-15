// SPDX-License-Identifier: Apache-2.0
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <tx_api.h>

#include "../../portable_feature_checks.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace
{
TX_THREAD runner;
alignas(16) std::array<std::byte, 65536> runner_stack{};
alignas(16) std::array<std::byte, 65536> worker_stack{};
alignas(16) std::array<std::byte, 65536> closer_stack{};
int    test_argc = 0;
char** test_argv = nullptr;

void run_tests(ULONG)
{
    doctest::Context context{test_argc, test_argv};
    std::exit(context.run());
}

void join_completed(osal::thread& worker, osal::semaphore& completed)
{
    const auto status = completed.checked_take(osal::milliseconds{2000});
    CHECK(status.ok());
    if (!status.ok())
    {
        std::fprintf(stderr, "ThreadX worker did not complete; refusing to reclaim its live callback context\n");
        std::_Exit(2);
    }
    // ThreadX join terminates its task; do not use it as proof of cooperative completion.
    const auto joined = worker.join();
    CHECK(joined.ok());
    if (!joined.ok())
    {
        std::fprintf(stderr, "ThreadX worker cleanup failed\n");
        std::_Exit(2);
    }
}
}  // namespace

extern "C" void tx_application_define(void*)
{
    const auto status = tx_thread_create(&runner, const_cast<CHAR*>("tests"), run_tests, 0U, runner_stack.data(),
                                         runner_stack.size(), 16U, 16U, TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS)
    {
        std::fprintf(stderr, "ThreadX test runner creation failed: %u\n", status);
        std::exit(2);
    }
}

int main(int argc, char** argv)
{
    test_argc = argc;
    test_argv = argv;
    tx_kernel_enter();
    return 2;
}

TEST_CASE("ThreadX SDK: bounded queue copies exact bytes without consuming unsupported peek")
{
    osal::queue<std::array<std::uint8_t, 3>, 2> queue;
    REQUIRE(queue.valid());
    const std::array<std::uint8_t, 3> message{0x12U, 0x34U, 0x56U};
    REQUIRE(queue.try_send(message));
    REQUIRE(queue.try_send(message));
    CHECK_FALSE(queue.try_send(message));
    struct guarded
    {
        std::array<std::uint8_t, 3> value{};
        std::uint8_t                guard = 0xA5U;
    } received;
    CHECK_FALSE(queue.peek(received.value));
    CHECK(queue.count() == 2U);
    CHECK(queue.try_receive(received.value));
    CHECK(received.value == message);
    CHECK(received.guard == 0xA5U);
    CHECK(queue.count() == 1U);
    CHECK(queue.close().ok());
}

TEST_CASE("ThreadX SDK: bounded semaphore and mutex lifecycle")
{
    osal::semaphore semaphore{osal::semaphore_type::binary, 0U};
    REQUIRE(semaphore.valid());
    CHECK_FALSE(semaphore.checked_take(osal::milliseconds{0}).ok());
    CHECK(semaphore.checked_give().ok());
    CHECK_FALSE(semaphore.checked_give().ok());
    CHECK(semaphore.checked_take(osal::milliseconds{0}).ok());
    CHECK(semaphore.close().ok());
    CHECK(semaphore.close().ok());

    osal::mutex mutex;
    REQUIRE(mutex.valid());
    CHECK(mutex.checked_lock().ok());
    CHECK(mutex.checked_unlock().ok());
    CHECK(mutex.close().ok());
}

TEST_CASE("ThreadX SDK: native thread receives its full pointer context and pool slots are reusable")
{
    struct context
    {
        osal::semaphore       finished{osal::semaphore_type::binary, 0U};
        std::atomic<unsigned> calls{0U};
    } ctx;
    REQUIRE(ctx.finished.valid());
    for (unsigned i = 0U; i < 16U; ++i)
    {
        osal::thread        worker;
        osal::thread_config config{};
        config.entry = +[](void* arg)
        {
            auto& state = *static_cast<context*>(arg);
            state.calls.fetch_add(1U);
            (void)state.finished.checked_give();
        };
        config.arg         = &ctx;
        config.stack       = worker_stack.data();
        config.stack_bytes = worker_stack.size();
        config.priority    = osal::PRIORITY_NORMAL;
        REQUIRE(worker.create(config).ok());
        REQUIRE(ctx.finished.checked_take(osal::milliseconds{1000}).ok());
        CHECK(worker.join().ok());
    }
    CHECK(ctx.calls.load() == 16U);
}

TEST_CASE("ThreadX SDK: stop callbacks execute and unregister under the native scheduler")
{
    osal::stop_source source;
    REQUIRE(source.valid());
    unsigned calls = 0U;
    {
        osal::stop_callback removed{source.get_token(), [](void* arg) noexcept { ++*static_cast<unsigned*>(arg); },
                                    &calls};
        REQUIRE(removed.status().ok());
        CHECK(removed.close(osal::milliseconds{0}).ok());
    }
    osal::stop_callback callback{source.get_token(), [](void* arg) noexcept { ++*static_cast<unsigned*>(arg); },
                                 &calls};
    REQUIRE(callback.status().ok());
    CHECK(source.request_stop());
    CHECK(calls == 1U);
    CHECK(callback.close().ok());
}

TEST_CASE("ThreadX SDK: cross-task readiness and cancellation wake native waiters")
{
    bool cancel = false;
    SUBCASE("readiness") {}
    SUBCASE("cancellation")
    {
        cancel = true;
    }
    for (unsigned repeat = 0U; repeat < 10U; ++repeat)
    {
        osal::wait_signal signal;
        osal::stop_source source;
        osal::semaphore   entered{osal::semaphore_type::binary, 0U};
        REQUIRE(signal.status().ok());
        REQUIRE(source.valid());
        REQUIRE(entered.valid());
        struct context
        {
            osal_test::wake_context wake;
            osal::semaphore         completed{osal::semaphore_type::binary, 0U};
        } ctx{{signal, entered, cancel ? source.get_token() : osal::stop_token{}}};
        REQUIRE(ctx.completed.valid());
        osal::thread        worker;
        osal::thread_config config{};
        config.entry = +[](void* arg)
        {
            auto& state = *static_cast<context*>(arg);
            osal_test::wait_worker(&state.wake);
            (void)state.completed.checked_give();
        };
        config.arg         = &ctx;
        config.stack       = worker_stack.data();
        config.stack_bytes = worker_stack.size();
        REQUIRE(worker.create(config).ok());
        CHECK(entered.checked_take(osal::milliseconds{1000}).ok());
        if (cancel)
        {
            bool first = false;
            CHECK(source.request_stop(first).ok());
            CHECK(first);
        }
        else
        {
            ctx.wake.ready.store(true, std::memory_order_release);
            CHECK(signal.notify().ok());
        }
        join_completed(worker, ctx.completed);
        CHECK(ctx.wake.announced.load() == osal::error_code::ok);
        CHECK(ctx.wake.status.load() == (cancel ? osal::error_code::cancelled : osal::error_code::ok));
        CHECK(signal.status().ok());
        CHECK(source.status().ok());
    }
}

TEST_CASE("ThreadX SDK: callback close times out and releases source metadata while waiting")
{
    osal::stop_source source;
    REQUIRE(source.valid());
    struct context
    {
        osal::stop_source&            source;
        osal::stop_callback*          callback = nullptr;
        osal::semaphore               entered{osal::semaphore_type::binary, 0U};
        osal::semaphore               release{osal::semaphore_type::binary, 0U};
        osal::semaphore               requested{osal::semaphore_type::binary, 0U};
        osal::semaphore               closing{osal::semaphore_type::binary, 0U};
        osal::semaphore               closed{osal::semaphore_type::binary, 0U};
        std::atomic<osal::error_code> announced{osal::error_code::not_initialized};
        std::atomic<osal::error_code> released{osal::error_code::not_initialized};
        std::atomic<osal::error_code> requested_status{osal::error_code::not_initialized};
        std::atomic<osal::error_code> close_status{osal::error_code::not_initialized};
        std::atomic<bool>             first{false};
    } ctx{source};
    REQUIRE(ctx.entered.valid());
    REQUIRE(ctx.release.valid());
    REQUIRE(ctx.requested.valid());
    REQUIRE(ctx.closing.valid());
    REQUIRE(ctx.closed.valid());
    osal::stop_callback callback{source.get_token(),
                                 [](void* arg) noexcept
                                 {
                                     auto& state = *static_cast<context*>(arg);
                                     state.announced.store(state.entered.checked_give().code());
                                     // Deliberately pause the executor to exercise concurrent unregister, not
                                     // production callback policy.
                                     state.released.store(state.release.checked_take(osal::milliseconds{1000}).code());
                                 },
                                 &ctx};
    REQUIRE(callback.status().ok());
    ctx.callback = &callback;
    osal::thread        requester;
    osal::thread_config config{};
    config.entry = +[](void* arg)
    {
        auto& state = *static_cast<context*>(arg);
        bool  first = false;
        state.requested_status.store(state.source.request_stop(first).code());
        state.first.store(first);
        (void)state.requested.checked_give();
    };
    config.arg         = &ctx;
    config.stack       = worker_stack.data();
    config.stack_bytes = worker_stack.size();
    REQUIRE(requester.create(config).ok());
    CHECK(ctx.entered.checked_take(osal::milliseconds{1000}).ok());
    CHECK(callback.close(osal::milliseconds{0}) == osal::error_code::timeout);

    osal::thread closer;
    config.entry = +[](void* arg)
    {
        auto& state = *static_cast<context*>(arg);
        (void)state.closing.checked_give();
        state.close_status.store(state.callback->close().code());
        (void)state.closed.checked_give();
    };
    config.stack       = closer_stack.data();
    config.stack_bytes = closer_stack.size();
    const auto created = closer.create(config);
    CHECK(created.ok());
    if (created.ok())
    {
        CHECK(ctx.closing.checked_take(osal::milliseconds{1000}).ok());
        CHECK(ctx.close_status.load() == osal::error_code::not_initialized);
        bool first = true;
        CHECK(source.request_stop(first).ok());
        CHECK_FALSE(first);
    }
    CHECK(ctx.release.checked_give().ok());
    join_completed(requester, ctx.requested);
    if (created.ok())
    {
        join_completed(closer, ctx.closed);
        CHECK(ctx.close_status.load() == osal::error_code::ok);
    }
    CHECK(callback.close(osal::milliseconds{0}).ok());
    CHECK(ctx.announced.load() == osal::error_code::ok);
    CHECK(ctx.released.load() == osal::error_code::ok);
    CHECK(ctx.requested_status.load() == osal::error_code::ok);
    CHECK(ctx.first.load());
    CHECK(source.status().ok());
}

TEST_CASE("ThreadX SDK: finite semaphore and contended mutex waits expire in native ticks")
{
    const osal::milliseconds timeout{30};
    osal::semaphore          semaphore{osal::semaphore_type::binary, 0U};
    REQUIRE(semaphore.valid());
    const auto started = osal_clock_ticks();
    CHECK(semaphore.checked_take(timeout) == osal::error_code::timeout);
    CHECK(static_cast<osal::tick_t>(osal_clock_ticks() - started) >= osal::clock_utils::ms_to_ticks(timeout));
    CHECK(semaphore.checked_give().ok());
    CHECK(semaphore.checked_take(osal::milliseconds{0}).ok());

    struct context
    {
        osal::mutex                   mutex;
        osal::semaphore               completed{osal::semaphore_type::binary, 0U};
        std::atomic<osal::error_code> status{osal::error_code::not_initialized};
        std::atomic<osal::tick_t>     elapsed{0U};
    } ctx;
    REQUIRE(ctx.mutex.valid());
    REQUIRE(ctx.completed.valid());
    REQUIRE(ctx.mutex.checked_lock().ok());
    osal::thread        worker;
    osal::thread_config config{};
    config.entry = +[](void* arg)
    {
        auto&      state  = *static_cast<context*>(arg);
        const auto before = osal_clock_ticks();
        const auto status = state.mutex.checked_lock(osal::milliseconds{30});
        state.elapsed.store(static_cast<osal::tick_t>(osal_clock_ticks() - before));
        state.status.store(status.code());
        if (status.ok())
        {
            (void)state.mutex.checked_unlock();
        }
        (void)state.completed.checked_give();
    };
    config.arg         = &ctx;
    config.stack       = worker_stack.data();
    config.stack_bytes = worker_stack.size();
    const auto created = worker.create(config);
    CHECK(created.ok());
    if (created.ok())
    {
        join_completed(worker, ctx.completed);
        CHECK(ctx.status.load() == osal::error_code::timeout);
        CHECK(ctx.elapsed.load() >= osal::clock_utils::ms_to_ticks(timeout));
    }
    CHECK(ctx.mutex.checked_unlock().ok());
    CHECK(ctx.mutex.checked_lock(osal::milliseconds{0}).ok());
    CHECK(ctx.mutex.checked_unlock().ok());
}

TEST_CASE("ThreadX SDK: semaphore exhaustion preserves live objects and released slots recover")
{
    std::array<std::optional<osal::semaphore>, 32> pool;
    std::size_t                                    created = 0U;
    for (; created < pool.size(); ++created)
    {
        auto& semaphore = pool[created].emplace(osal::semaphore_type::binary, 0U);
        if (!semaphore.valid())
        {
            CHECK(semaphore.initialization_status() == osal::error_code::out_of_resources);
            break;
        }
    }
    REQUIRE(created > 0U);
    REQUIRE(created < pool.size());
    {
        osal::stop_source exhausted;
        CHECK(exhausted.initialization_status() == osal::error_code::out_of_resources);
        CHECK_FALSE(exhausted.valid());
    }
    auto& first = pool[0];
    if (!first.has_value())
    {
        FAIL("The first semaphore slot must remain constructed after exhaustion");
        return;
    }
    CHECK(first->checked_give().ok());
    CHECK(first->checked_take(osal::milliseconds{0}).ok());
    first.reset();
    auto& replacement = first.emplace(osal::semaphore_type::binary, 1U);
    CHECK(replacement.valid());
    CHECK(replacement.checked_take(osal::milliseconds{0}).ok());
}

TEST_CASE("ThreadX SDK: timer reclamation policy retains a started timer")
{
    static_assert(!osal::timer::can_reclaim_after_start);
    osal::timer unstarted{+[](void*) {}, nullptr, osal::milliseconds{10}};
    REQUIRE(unstarted.valid());
    CHECK(unstarted.close().ok());

    // The backend deliberately retains started slots; keep native callback storage process-lived.
    static osal::active_traits::timer_handle_t retained{};
    static std::atomic<unsigned>               calls{0U};
    REQUIRE(osal_timer_create(&retained, "retained", +[](void*) { calls.fetch_add(1U); }, nullptr, 1U, false).ok());
    REQUIRE(osal_timer_start(&retained).ok());
    osal::thread::sleep_for(osal::milliseconds{100});
    CHECK(calls.load() == 1U);
    CHECK(osal_timer_stop(&retained).ok());
    CHECK(osal_timer_destroy(&retained) == osal::error_code::not_supported);
    CHECK(retained.native != nullptr);
}
