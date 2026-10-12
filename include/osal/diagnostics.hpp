// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "error.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace osal
{

/// Independently sampled gauges and modulo-2^32 operation counters.
struct diagnostic_snapshot
{
    std::uint32_t capacity{};
    std::uint32_t in_use{};
    std::uint32_t high_water{};
    std::uint32_t attempts{};
    std::uint32_t successes{};
    std::uint32_t failures{};
    std::uint32_t contention{};
    std::uint32_t drops{};
    std::uint32_t backlog{};
};

class diagnostic_counters
{
public:
    void attempt(result status) noexcept
    {
        attempts_.fetch_add(1U, std::memory_order_relaxed);
        (status.ok() ? successes_ : failures_).fetch_add(1U, std::memory_order_relaxed);
    }

    void acquire() noexcept
    {
        const auto current = in_use_.fetch_add(1U, std::memory_order_relaxed) + 1U;
        raise_high_water(current);
    }

    void release() noexcept
    {
        auto current = in_use_.load(std::memory_order_relaxed);
        while (current != 0U && !in_use_.compare_exchange_weak(current, current - 1U, std::memory_order_relaxed))
        {
        }
    }

    void set_capacity(std::uint32_t capacity) noexcept { capacity_.store(capacity, std::memory_order_relaxed); }
    void dropped() noexcept { drops_.fetch_add(1U, std::memory_order_relaxed); }
    void contended() noexcept { contention_.fetch_add(1U, std::memory_order_relaxed); }
    void set_backlog(std::uint32_t backlog) noexcept
    {
        backlog_.store(backlog, std::memory_order_relaxed);
        raise_high_water(backlog);
    }

    [[nodiscard]] diagnostic_snapshot snapshot() const noexcept
    {
        return {capacity_.load(std::memory_order_relaxed),   in_use_.load(std::memory_order_relaxed),
                high_water_.load(std::memory_order_relaxed), attempts_.load(std::memory_order_relaxed),
                successes_.load(std::memory_order_relaxed),  failures_.load(std::memory_order_relaxed),
                contention_.load(std::memory_order_relaxed), drops_.load(std::memory_order_relaxed),
                backlog_.load(std::memory_order_relaxed)};
    }

private:
    void raise_high_water(std::uint32_t current) noexcept
    {
        auto previous = high_water_.load(std::memory_order_relaxed);
        while (previous < current && !high_water_.compare_exchange_weak(previous, current, std::memory_order_relaxed))
        {
        }
    }

    std::atomic<std::uint32_t> capacity_{0}, in_use_{0}, high_water_{0};
    std::atomic<std::uint32_t> attempts_{0}, successes_{0}, failures_{0};
    std::atomic<std::uint32_t> contention_{0}, drops_{0}, backlog_{0};
};

enum class diagnostic_resource : std::size_t
{
    mutex,
    semaphore,
    queue,
    thread,
    timer,
    work_queue,
    emulated_memory_pool,
    emulated_work_queue,
    count
};

namespace detail
{
inline std::array<diagnostic_counters, static_cast<std::size_t>(diagnostic_resource::count)> resource_diagnostics{};
}

[[nodiscard]] inline diagnostic_counters& diagnostics(diagnostic_resource resource) noexcept
{
    return detail::resource_diagnostics[static_cast<std::size_t>(resource)];
}

}  // namespace osal
