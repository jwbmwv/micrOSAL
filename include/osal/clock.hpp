// SPDX-License-Identifier: Apache-2.0
/// @file clock.hpp
/// @brief OSAL clock API — monotonic and system clocks
/// @details Exposes osal::monotonic_clock and osal::system_clock.
///          The monotonic clock is always available and guaranteed non-decreasing.
///          The system (wall) clock is available on backends with has_system_clock.
///
///          Usage:
///          @code
///          auto t0 = osal::monotonic_clock::now();
///          // ... work ...
///          auto elapsed = osal::monotonic_clock::now() - t0;
///          auto ms = std::chrono::duration_cast<osal::milliseconds>(elapsed);
///          @endcode
///
///          Tick conversion:
///          - osal::clock_utils provides portable tick ↔ ms helpers.
///          - The tick period is queried at runtime via get_tick_period_us().
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_clock
#pragma once

#include "types.hpp"
#include <cstdint>

extern "C"
{
    /// @brief Returns the monotonic time in milliseconds since an arbitrary epoch.
    std::int64_t osal_clock_monotonic_ms() noexcept;
    /// @brief Returns the system time in milliseconds since Unix epoch (if available).
    std::int64_t osal_clock_system_ms() noexcept;
    /// @brief Returns the RTOS tick count.
    osal::tick_t osal_clock_ticks() noexcept;
    /// @brief Returns the duration of one RTOS tick in microseconds.
    std::uint32_t osal_clock_tick_period_us() noexcept;
}  // extern "C"

namespace osal
{

/// @defgroup osal_clock OSAL Clock
/// @brief Monotonic and system clocks with chrono integration.
/// @{

// ---------------------------------------------------------------------------
// monotonic_clock::now() implementation
// ---------------------------------------------------------------------------

// Defined header-only (one-liner delegating to C function).
inline monotonic_clock::time_point monotonic_clock::now() noexcept
{
    return time_point{duration{osal_clock_monotonic_ms()}};
}

// ---------------------------------------------------------------------------
// system_clock::now() implementation
// ---------------------------------------------------------------------------

inline system_clock::time_point system_clock::now() noexcept
{
    // osal_clock_system_ms() returns monotonic ms on backends that lack a
    // wall clock, so no capability check is needed here.
    return time_point{duration{osal_clock_system_ms()}};
}

// ---------------------------------------------------------------------------
// clock_utils
// ---------------------------------------------------------------------------

/// @brief Utility helpers for tick ↔ duration conversions.
struct clock_utils
{
    /// @brief Converts a duration to RTOS ticks, with saturation.
    /// @details Behaviour:
    ///   - @p ms ≤ 0  →  @c NO_WAIT  (non-blocking / poll)
    ///   - Conversion result ≥ @c WAIT_FOREVER  →  saturated to
    ///     @c WAIT_FOREVER - 1, preserving the "infinite wait" sentinel.
    /// @param ms  Duration. Negative values are treated as zero-wait.
    /// @return RTOS tick count in [@c NO_WAIT, @c WAIT_FOREVER - 1].
    static tick_t ms_to_ticks(milliseconds ms) noexcept
    {
        if (ms.count() <= 0)
        {
            return NO_WAIT;
        }
        constexpr tick_t    kMax      = WAIT_FOREVER - tick_t{1};
        const std::uint32_t period_us = osal_clock_tick_period_us();
        if (period_us == 0U)
        {
            // 1-tick-per-ms fallback — clamp to [NO_WAIT, WAIT_FOREVER-1].
            const auto raw = static_cast<std::uint64_t>(ms.count());
            return static_cast<tick_t>((raw < static_cast<std::uint64_t>(kMax)) ? raw : kMax);
        }
        // Round up: ceil(ms * 1000 / period_us), then saturate.
        const std::uint64_t us    = static_cast<std::uint64_t>(ms.count()) * 1000U;
        const std::uint64_t ticks = (us + period_us - 1U) / period_us;
        return static_cast<tick_t>((ticks < static_cast<std::uint64_t>(kMax)) ? ticks : kMax);
    }

    /// @brief Converts RTOS ticks to milliseconds.
    /// @param ticks Tick count.
    /// @return Equivalent duration in milliseconds.
    static milliseconds ticks_to_ms(tick_t ticks) noexcept
    {
        const std::uint32_t period_us = osal_clock_tick_period_us();
        const std::uint64_t us        = static_cast<std::uint64_t>(ticks) * period_us;
        return milliseconds{static_cast<std::int64_t>(us / 1000U)};
    }

    /// @brief Returns current tick count.
    static tick_t now_ticks() noexcept { return osal_clock_ticks(); }
};

/// @} // osal_clock

}  // namespace osal
