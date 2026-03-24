// SPDX-License-Identifier: Apache-2.0
/// @file types.hpp
/// @brief Common OSAL type definitions
/// @details Provides tick counts, durations, time-points, and priority types used
///          throughout the OSAL. Compile-time tick-width selections are
///          validated with @c consteval helpers so misconfiguration fails during
///          translation, not at runtime. All types remain plain value types with
///          no dynamic allocation.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_core
#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>
#include <array>
#include <span>

#if defined(OSAL_BACKEND_NUTTX) && defined(__GNUC__)
#include <bits/chrono.h>
#else
#include <chrono>
#endif

namespace osal
{

/// @defgroup osal_types OSAL Types
/// @brief Portable type aliases and constants.
/// @{

// ---------------------------------------------------------------------------
// Tick / duration types
// ---------------------------------------------------------------------------

namespace detail
{

template<std::size_t N>
consteval std::size_t count_selected_tick_types(std::span<const bool, N> flags) noexcept
{
    std::size_t count = 0U;
    for (const bool flag : flags)
    {
        count += flag ? 1U : 0U;
    }
    return count;
}

inline constexpr std::array<bool, 3U> tick_type_selection_flags{
#if defined(OSAL_CFG_TICK_TYPE_U16)
    true,
#else
    false,
#endif
#if defined(OSAL_CFG_TICK_TYPE_U32)
    true,
#else
    false,
#endif
#if defined(OSAL_CFG_TICK_TYPE_U64)
    true,
#else
    false,
#endif
};

consteval bool valid_native_tick_bits(std::size_t bits) noexcept
{
    return (bits == 16U) || (bits == 32U) || (bits == 64U);
}

template<typename Tick>
consteval std::size_t tick_storage_bits() noexcept
{
    return sizeof(Tick) * 8U;
}

}  // namespace detail

static_assert(detail::count_selected_tick_types(std::span{detail::tick_type_selection_flags}) <= 1U,
              "OSAL: only one of OSAL_CFG_TICK_TYPE_U16/U32/U64 may be defined");

/// @brief Portable timeout unit used at the OSAL C-ABI boundary.
/// @details This is the OSAL's internal timeout representation, **not** the
///          backend's native tick type.  Each backend converts it internally to
///          whatever its native API requires (e.g. Zephyr @c k_timeout_t,
///          POSIX @c struct timespec, FreeRTOS @c TickType_t).
///
///          Width is configurable at compile time via:
///          - @c OSAL_CFG_TICK_TYPE_U16
///          - @c OSAL_CFG_TICK_TYPE_U32 (default)
///          - @c OSAL_CFG_TICK_TYPE_U64
///
///          Conversion from
///          @c std::chrono durations is done via @c clock_utils::ms_to_ticks(),
///          which **saturates** (never wraps):
///          - Negative or zero duration → @c NO_WAIT (do not block)
///          - Overflow → @c WAIT_FOREVER - 1 (longest finite wait)
#if defined(OSAL_CFG_TICK_TYPE_U64)
using tick_t = std::uint64_t;
#elif defined(OSAL_CFG_TICK_TYPE_U16)
using tick_t = std::uint16_t;
#else
using tick_t = std::uint32_t;
#endif

#if defined(OSAL_CFG_NATIVE_TICK_BITS)
static_assert(detail::valid_native_tick_bits(OSAL_CFG_NATIVE_TICK_BITS),
              "OSAL_CFG_NATIVE_TICK_BITS must be 16, 32, or 64");
static_assert(detail::tick_storage_bits<tick_t>() <= static_cast<std::size_t>(OSAL_CFG_NATIVE_TICK_BITS),
              "OSAL tick_t is wider than backend native tick type; choose a narrower OSAL tick type or set correct "
              "OSAL_CFG_NATIVE_TICK_BITS for this backend");
#endif

/// @brief Represents "wait forever" / no timeout.
inline constexpr tick_t WAIT_FOREVER = std::numeric_limits<tick_t>::max();

/// @brief Represents "do not wait" (non-blocking / polling).
inline constexpr tick_t NO_WAIT = tick_t{0};

// ---------------------------------------------------------------------------
// Thread types
// ---------------------------------------------------------------------------

/// @brief Thread priority — higher numeric value = higher priority.
/// @details The valid range is [OSAL_PRIORITY_LOWEST, OSAL_PRIORITY_HIGHEST].
///          Backends map this range to their native range.
using priority_t = std::int32_t;

/// @brief Lowest valid thread priority.
inline constexpr priority_t PRIORITY_LOWEST = 0;
/// @brief Default / normal thread priority.
inline constexpr priority_t PRIORITY_NORMAL = 128;
/// @brief Highest valid thread priority (reserved for critical tasks).
inline constexpr priority_t PRIORITY_HIGHEST = 255;

/// @brief CPU affinity mask.  Bit N = core N.  0 means "any core".
using affinity_t = std::uint32_t;
/// @brief Special affinity value: no affinity constraint (any core).
inline constexpr affinity_t AFFINITY_ANY = affinity_t{0};

// ---------------------------------------------------------------------------
// Clock / time-point types
// ---------------------------------------------------------------------------

/// @brief Monotonic clock resolution — milliseconds for portability.
using milliseconds = std::chrono::milliseconds;
/// @brief Monotonic clock resolution — microseconds (where supported).
using microseconds = std::chrono::microseconds;

/// @brief Portable monotonic clock used across all OSAL time APIs.
struct monotonic_clock
{
    using rep                       = std::int64_t;
    using period                    = std::milli;
    using duration                  = std::chrono::duration<rep, period>;
    using time_point                = std::chrono::time_point<monotonic_clock>;
    static constexpr bool is_steady = true;

    /// @brief Returns the current monotonic time.
    /// @return Current monotonic time_point in milliseconds.
    static time_point now() noexcept;
};

/// @brief System (wall) clock — may not be monotonic on all platforms.
struct system_clock
{
    using rep                       = std::int64_t;
    using period                    = std::milli;
    using duration                  = std::chrono::duration<rep, period>;
    using time_point                = std::chrono::time_point<system_clock>;
    static constexpr bool is_steady = false;

    /// @brief Returns the current system (wall-clock) time.
    /// @return Current system time_point in milliseconds.
    static time_point now() noexcept;
};

// ---------------------------------------------------------------------------
// Size / count helpers
// ---------------------------------------------------------------------------

/// @brief Compile-time stack size in bytes.
using stack_size_t = std::size_t;

/// @brief Queue depth / count type.
using queue_depth_t = std::size_t;

/// @brief Event flag bitmask — 32 bits, one bit per flag.
using event_bits_t = std::uint32_t;

/// @brief Maximum number of bits in an event flag group.
inline constexpr std::size_t EVENT_BITS_MAX = 32U;

/// @} // osal_types

}  // namespace osal
