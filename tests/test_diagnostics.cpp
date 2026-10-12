// SPDX-License-Identifier: Apache-2.0
/// @file test_diagnostics.cpp
/// @brief Resource high water, failure/drop observations, and work backlog.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/diagnostics.hpp>
#include <osal/osal.hpp>

namespace
{
alignas(16) std::uint8_t worker_stack[65536];

struct blocked_work
{
    osal::semaphore entered{osal::semaphore_type::binary, 0U};
    osal::semaphore release{osal::semaphore_type::binary, 0U};
};

void block_worker(void* arg)
{
    auto& state = *static_cast<blocked_work*>(arg);
    state.entered.give();
    (void)state.release.checked_take(osal::milliseconds{2000});
}
void no_work(void*) {}
}  // namespace

TEST_CASE("diagnostics: independent counters track resource and operation observations")
{
    osal::diagnostic_counters counters;
    counters.set_capacity(2U);
    counters.attempt(osal::ok());
    counters.attempt(osal::error_code::out_of_resources);
    counters.acquire();
    counters.acquire();
    counters.release();
    counters.contended();
    counters.dropped();
    counters.set_backlog(3U);
    const auto snapshot = counters.snapshot();
    CHECK(snapshot.capacity == 2U);
    CHECK(snapshot.in_use == 1U);
    CHECK(snapshot.high_water == 3U);
    CHECK(snapshot.attempts == 2U);
    CHECK(snapshot.successes == 1U);
    CHECK(snapshot.failures == 1U);
    CHECK(snapshot.contention == 1U);
    CHECK(snapshot.drops == 1U);
    CHECK(snapshot.backlog == 3U);
    counters.release();
    counters.release();
    CHECK(counters.snapshot().in_use == 0U);
    CHECK(counters.snapshot().high_water == 3U);
}

TEST_CASE("diagnostics: live resources and high water follow initialization and close")
{
    auto&      counters = osal::diagnostics(osal::diagnostic_resource::semaphore);
    const auto before   = counters.snapshot();
    {
        osal::semaphore first{osal::semaphore_type::binary, 0U};
        osal::semaphore second{osal::semaphore_type::binary, 0U};
        REQUIRE(first.valid());
        REQUIRE(second.valid());
        CHECK(counters.snapshot().in_use == before.in_use + 2U);
        CHECK(counters.snapshot().high_water >= before.in_use + 2U);
        osal::semaphore invalid{osal::semaphore_type::counting, 0U, 0U};
        CHECK_FALSE(invalid.valid());
        CHECK(counters.snapshot().failures == before.failures + 1U);
        CHECK(counters.snapshot().in_use == before.in_use + 2U);
        CHECK(first.close().ok());
        CHECK(counters.snapshot().in_use == before.in_use + 1U);
        const auto closed = counters.snapshot();
        CHECK(first.close().ok());
        CHECK(counters.snapshot().attempts == closed.attempts);
    }
    CHECK(counters.snapshot().in_use == before.in_use);
}

TEST_CASE("diagnostics: queue failures, drops, and backlog are observed without ISR instrumentation")
{
    auto&               counters = osal::diagnostics(osal::diagnostic_resource::queue);
    const auto          before   = counters.snapshot();
    osal::queue<int, 1> queue;
    REQUIRE(queue.valid());
    REQUIRE(queue.try_send(1));
    CHECK_FALSE(queue.try_send(2));
    auto stats = queue.statistics();
    CHECK(stats.capacity == 1U);
    CHECK(stats.backlog == 1U);
    CHECK(stats.high_water == 1U);
    CHECK(stats.attempts == 2U);
    CHECK(stats.failures == 1U);
    CHECK(stats.drops == 1U);
    CHECK(stats.contention == 1U);
    CHECK(counters.snapshot().drops == before.drops + 1U);
    CHECK(counters.snapshot().failures == before.failures + 1U);
    int value{};
    REQUIRE(queue.try_receive(value));
    CHECK(value == 1);
    CHECK(queue.statistics().backlog == 0U);
    CHECK_FALSE(queue.try_receive(value));
    CHECK(queue.statistics().failures == 2U);
    CHECK(queue.statistics().drops == 1U);

    if constexpr (osal::active_capabilities::has_isr_queue)
    {
        stats             = queue.statistics();
        const auto global = counters.snapshot();
        CHECK(queue.send_isr(3));
        CHECK_FALSE(queue.send_isr(4));
        CHECK(queue.receive_isr(value));
        CHECK(queue.statistics().attempts == stats.attempts);
        CHECK(queue.statistics().drops == stats.drops);
        CHECK(counters.snapshot().attempts == global.attempts);
        CHECK(counters.snapshot().drops == global.drops);
    }
    CHECK(queue.close().ok());
    CHECK(counters.snapshot().in_use == before.in_use);
}

TEST_CASE("diagnostics: work queue observes backlog, contention, and rejected submissions")
{
    auto&            counters = osal::diagnostics(osal::diagnostic_resource::work_queue);
    const auto       before   = counters.snapshot();
    blocked_work     state;
    osal::work_queue work{worker_stack, sizeof(worker_stack), 1U};
    REQUIRE(work.valid());
    REQUIRE(work.submit(block_worker, &state).ok());
    REQUIRE(state.entered.checked_take(osal::milliseconds{1000}).ok());
    CHECK(work.submit(no_work).ok());
    CHECK(work.pending() == 1U);
    CHECK(work.statistics().backlog == 1U);
    CHECK(work.statistics().high_water >= 1U);
    CHECK(work.submit(no_work) == osal::error_code::overflow);
    CHECK(work.statistics().drops == 1U);
    CHECK(work.statistics().failures == 1U);
    CHECK(work.statistics().contention == 1U);
    CHECK(counters.snapshot().drops == before.drops + 1U);
    const auto blocked_flush = work.flush(osal::milliseconds{0});
    CHECK((blocked_flush == osal::error_code::timeout || blocked_flush == osal::error_code::overflow ||
           blocked_flush == osal::error_code::would_block));
    CHECK(work.statistics().contention == 2U);
    state.release.give();
    CHECK(work.shutdown().ok());
    CHECK(work.pending() == 0U);
    CHECK(work.statistics().backlog == 0U);
    CHECK(work.shutdown().ok());
    CHECK(counters.snapshot().in_use == before.in_use);
}
