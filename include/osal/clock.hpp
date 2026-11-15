// SPDX-License-Identifier: Apache-2.0
/// @file clock.hpp
/// @brief OSAL clock API — monotonic, system, and high-resolution clocks
/// @details Exposes osal::monotonic_clock, osal::system_clock, and the draft
///          osal::high_resolution_clock API.
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

#include "backends.hpp"
#include "types.hpp"
#include <cstdint>

extern "C"
{
    /// @brief Returns the monotonic time in milliseconds since an arbitrary epoch.
    /// @return Non-decreasing elapsed milliseconds.
    std::int64_t osal_clock_monotonic_ms() noexcept;
    /// @brief Returns the system time in milliseconds since Unix epoch (if available).
    /// @return Wall-clock milliseconds, or monotonic milliseconds when unsupported.
    std::int64_t osal_clock_system_ms() noexcept;
    /// @brief Returns the RTOS tick count.
    /// @return Backend tick counter; rollover follows the backend and tick_t width.
    osal::tick_t osal_clock_ticks() noexcept;
    /// @brief Returns the duration of one RTOS tick in microseconds.
    /// @return Backend tick period in microseconds.
    std::uint32_t osal_clock_tick_period_us() noexcept;
    /// @brief Returns the backend high-resolution time source in nanoseconds.
    /// @pre The active backend advertises high-resolution clock support.
    /// @return Backend high-resolution timestamp in nanoseconds.
    std::int64_t osal_clock_high_resolution_ns() noexcept;
    /// @brief Returns the nominal resolution of the backend high-resolution source in nanoseconds.
    /// @pre The active backend advertises high-resolution clock support.
    /// @return Nominal source resolution in nanoseconds.
    std::int64_t osal_clock_high_resolution_resolution_ns() noexcept;
}  // extern "C"

namespace osal
{

/// @defgroup osal_clock OSAL Clock
/// @brief Monotonic and system clocks with chrono integration.
/// @{

/// @brief Draft high-resolution clock API.
/// @details Backends that opt into @c support_requirement::high_resolution_clock
///          provide a native nanosecond-resolution source. Unsupported backends
///          fall back to promoted monotonic-clock values. Use require_support()
///          when a true high-resolution source is mandatory.
struct high_resolution_clock
{
    using rep        = std::int64_t;
    using period     = std::nano;
    using duration   = nanoseconds;
    using time_point = std::chrono::time_point<high_resolution_clock>;

    static constexpr bool is_steady    = true;
    static constexpr bool is_supported = supports_requirement<support_requirement::high_resolution_clock>;

    /// @brief Enforce high-resolution clock support at compile time.
    /// @tparam Backend  Backend whose high-resolution clock capability is checked.
    /// @details Compilation fails if the selected backend lacks this capability.
    template<typename Backend = active_backend>
    static consteval void require_support()
    {
        require_backend_support<support_requirement::high_resolution_clock, Backend>();
    }

    /// @brief Returns the current timestamp using the best available draft source.
    /// @return Native high-resolution time, or promoted monotonic time when unsupported.
    static time_point now() noexcept;

    /// @brief Returns the nominal resolution of the draft source.
    /// @return Native source resolution, or the backend tick period when unsupported.
    static duration resolution() noexcept;
};

namespace detail
{

template<typename ToDuration, typename Rep, typename Period>
inline ToDuration ceil_duration(std::chrono::duration<Rep, Period> value) noexcept
{
    const ToDuration casted = std::chrono::duration_cast<ToDuration>(value);
    if (casted < value)
    {
        return casted + ToDuration{1};
    }
    return casted;
}

template<typename TimePoint>
inline TimePoint saturating_add(TimePoint base, typename TimePoint::duration delta) noexcept
{
    const auto max_delta = TimePoint::max() - base;
    return (delta >= max_delta) ? TimePoint::max() : (base + delta);
}

}  // namespace detail

/// @brief Absolute deadline helper for polling loops and cooperative work slices.
/// @details Stores an absolute expiry on a steady clock and exposes wrap-safe
///          expiry and remaining-time queries. The default public alias is
///          @c osal::monotonic_deadline; @c high_resolution_deadline uses
///          @c high_resolution_clock and inherits its support/fallback semantics.
/// @tparam Clock  Steady chrono-compatible clock used to measure expiry.
template<typename Clock>
class basic_deadline
{
    static_assert(Clock::is_steady, "osal::basic_deadline requires a steady clock");

public:
    using clock      = Clock;
    using duration   = typename clock::duration;
    using time_point = typename clock::time_point;

    /// @brief Constructs an already-expired deadline at time_point::min().
    constexpr basic_deadline() noexcept = default;

    /// @brief Creates a deadline relative to @c clock::now().
    /// @details Positive durations are rounded up to the clock's duration unit.
    ///          Addition saturates at time_point::max(); non-positive durations expire now.
    /// @tparam Rep     Timeout representation.
    /// @tparam Period  Timeout unit.
    /// @param[in] timeout  Duration before expiry.
    /// @return Deadline measured from the current clock reading.
    template<typename Rep, typename Period>
    static basic_deadline after(std::chrono::duration<Rep, Period> timeout) noexcept
    {
        const time_point now = clock::now();
        if (timeout <= timeout.zero())
        {
            return at(now);
        }
        return at(detail::saturating_add(now, detail::ceil_duration<duration>(timeout)));
    }

    /// @brief Creates a deadline that expires at an absolute time point.
    /// @param[in] when  Absolute expiry on this deadline's clock.
    /// @return Deadline with the supplied expiry.
    static constexpr basic_deadline at(time_point when) noexcept { return basic_deadline{when}; }

    /// @brief Returns true when the deadline has expired.
    /// @return Whether the current clock reading reaches or exceeds expiry.
    bool expired() const noexcept { return expired(clock::now()); }

    /// @brief Returns true when the provided time point reaches or exceeds the deadline.
    /// @param[in] now  Clock reading to compare with expiry.
    /// @return Whether @p now reaches or exceeds expiry.
    constexpr bool expired(time_point now) const noexcept { return now >= expiry_; }

    /// @brief Returns the remaining time until expiry, saturating at zero.
    /// @return Non-negative time remaining from the current clock reading.
    duration remaining() const noexcept { return remaining(clock::now()); }

    /// @brief Returns the remaining time from a supplied time point.
    /// @param[in] now  Clock reading from which to measure remaining time.
    /// @return Time until expiry, or zero when already expired.
    constexpr duration remaining(time_point now) const noexcept
    {
        return expired(now) ? duration::zero() : (expiry_ - now);
    }

    /// @brief Returns the absolute expiry time.
    /// @return Stored expiry on this deadline's clock.
    constexpr time_point expires_at() const noexcept { return expiry_; }

    /// @brief Restarts the deadline relative to @c clock::now().
    /// @tparam Rep     Timeout representation.
    /// @tparam Period  Timeout unit.
    /// @param[in] timeout  New relative timeout, rounded and saturated as in after().
    template<typename Rep, typename Period>
    void restart(std::chrono::duration<Rep, Period> timeout) noexcept
    {
        expiry_ = after(timeout).expiry_;
    }

private:
    explicit constexpr basic_deadline(time_point when) noexcept : expiry_{when} {}

    time_point expiry_{time_point::min()};
};

/// @brief Explicit steady deadline alias bound to @c monotonic_clock.
using monotonic_deadline = basic_deadline<monotonic_clock>;

/// @brief Deadline alias bound to the draft high-resolution clock.
using high_resolution_deadline = basic_deadline<high_resolution_clock>;

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
// high_resolution_clock draft implementation
// ---------------------------------------------------------------------------

inline high_resolution_clock::time_point high_resolution_clock::now() noexcept
{
    if constexpr (is_supported)
    {
        return time_point{duration{osal_clock_high_resolution_ns()}};
    }
    const auto since_epoch = monotonic_clock::now().time_since_epoch();
    return time_point{std::chrono::duration_cast<high_resolution_clock::duration>(since_epoch)};
}

inline high_resolution_clock::duration high_resolution_clock::resolution() noexcept
{
    if constexpr (is_supported)
    {
        return duration{osal_clock_high_resolution_resolution_ns()};
    }
    return std::chrono::duration_cast<high_resolution_clock::duration>(microseconds{osal_clock_tick_period_us()});
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
    /// @param[in] ms  Duration. Negative values are treated as zero-wait.
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
        constexpr std::uint64_t kMicrosecondsPerMillisecond = 1'000U;
        const auto              raw_milliseconds            = static_cast<std::uint64_t>(ms.count());
        const std::uint64_t     milliseconds_quotient       = raw_milliseconds / period_us;
        const std::uint64_t     milliseconds_remainder      = raw_milliseconds % period_us;
        const std::uint64_t     remainder_ticks =
            ((milliseconds_remainder * kMicrosecondsPerMillisecond) + period_us - 1U) / period_us;
        const auto max_ticks = static_cast<std::uint64_t>(kMax);
        if (milliseconds_quotient > ((max_ticks - remainder_ticks) / kMicrosecondsPerMillisecond))
        {
            return kMax;
        }
        const std::uint64_t ticks = (milliseconds_quotient * kMicrosecondsPerMillisecond) + remainder_ticks;
        return static_cast<tick_t>((ticks < static_cast<std::uint64_t>(kMax)) ? ticks : kMax);
    }

    /// @brief Converts RTOS ticks to milliseconds.
    /// @param[in] ticks  Tick count.
    /// @return Equivalent duration in milliseconds.
    static milliseconds ticks_to_ms(tick_t ticks) noexcept
    {
        const std::uint32_t period_us = osal_clock_tick_period_us();
        const std::uint64_t us        = static_cast<std::uint64_t>(ticks) * period_us;
        return milliseconds{static_cast<std::int64_t>(us / 1000U)};
    }

    /// @brief Returns current tick count.
    /// @return Backend tick counter; rollover follows tick_t and the backend.
    static tick_t now_ticks() noexcept { return osal_clock_ticks(); }
};

/// @} // osal_clock

}  // namespace osal
