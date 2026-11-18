// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "detail/atomic_compat.hpp"
#include "error.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace osal
{

/// Independently sampled gauges and modulo-2^32 operation counters.
struct diagnostic_snapshot
{
    std::uint32_t capacity{};    ///< Configured resource capacity, or zero when not applicable.
    std::uint32_t in_use{};      ///< Current number of acquired resources.
    std::uint32_t high_water{};  ///< Maximum observed in-use count or backlog.
    std::uint32_t attempts{};    ///< Number of recorded operations, modulo 2^32.
    std::uint32_t successes{};   ///< Number of successful recorded operations, modulo 2^32.
    std::uint32_t failures{};    ///< Number of failed recorded operations, modulo 2^32.
    std::uint32_t contention{};  ///< Number of recorded contention events, modulo 2^32.
    std::uint32_t drops{};       ///< Number of recorded drops, modulo 2^32.
    std::uint32_t backlog{};     ///< Current queued work or messages.
};

/// @brief Thread-safe, allocation-free counters and gauges for one resource.
/// @details Each field is sampled independently with relaxed atomic operations;
///          a snapshot is therefore not a transactionally consistent view.
class diagnostic_counters
{
public:
    /// @brief Records one operation outcome.
    /// @param[in] status  Operation result; success is determined by @c status.ok().
    void attempt(result status) noexcept
    {
        attempts_.fetch_add(1U, std::memory_order_relaxed);
        (status.ok() ? successes_ : failures_).fetch_add(1U, std::memory_order_relaxed);
    }

    /// @brief Increments the current in-use gauge and updates its high-water mark.
    void acquire() noexcept
    {
        const auto current = in_use_.fetch_add(1U, std::memory_order_relaxed) + 1U;
        raise_high_water(current);
    }

    /// @brief Decrements the current in-use gauge when it is nonzero.
    void release() noexcept
    {
        auto current = in_use_.load(std::memory_order_relaxed);
        while (current != 0U && !in_use_.compare_exchange_weak(current, current - 1U, std::memory_order_relaxed))
        {
        }
    }

    /// @brief Sets the configured capacity gauge.
    /// @param[in] capacity  Capacity to report.
    void set_capacity(std::uint32_t capacity) noexcept { capacity_.store(capacity, std::memory_order_relaxed); }
    /// @brief Records one dropped item.
    void dropped() noexcept { drops_.fetch_add(1U, std::memory_order_relaxed); }
    /// @brief Records one contention event.
    void contended() noexcept { contention_.fetch_add(1U, std::memory_order_relaxed); }
    /// @brief Sets the current backlog gauge and updates its high-water mark.
    /// @param[in] backlog  Backlog to report.
    void set_backlog(std::uint32_t backlog) noexcept
    {
        backlog_.store(backlog, std::memory_order_relaxed);
        raise_high_water(backlog);
    }

    /// @brief Samples all counters and gauges independently.
    /// @return A weakly consistent snapshot; individual values may reflect different instants.
    [[nodiscard]] diagnostic_snapshot snapshot() const noexcept
    {
        return {capacity_.load(std::memory_order_relaxed),   in_use_.load(std::memory_order_relaxed),
                high_water_.load(std::memory_order_relaxed), attempts_.load(std::memory_order_relaxed),
                successes_.load(std::memory_order_relaxed),  failures_.load(std::memory_order_relaxed),
                contention_.load(std::memory_order_relaxed), drops_.load(std::memory_order_relaxed),
                backlog_.load(std::memory_order_relaxed)};
    }

private:
    /// @brief Raises the high-water gauge using a relaxed compare-exchange loop.
    /// @param[in] current  Candidate value to preserve if it exceeds the observed maximum.
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

/// @brief Returns the process-wide counters for a diagnostic resource.
/// @param[in] resource  Resource category; must be a value other than @c count.
/// @return Mutable counters with static lifetime for the selected category.
[[nodiscard]] inline diagnostic_counters& diagnostics(diagnostic_resource resource) noexcept
{
    return detail::resource_diagnostics[static_cast<std::size_t>(resource)];
}

}  // namespace osal
