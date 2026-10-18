// SPDX-License-Identifier: Apache-2.0
/// @file backend_timeout_adapter.hpp
/// @brief Internal helpers for mapping OSAL timeout tokens to backend-native forms.
/// @details
/// This adapter keeps the public OSAL ABI stable (`osal::tick_t`) while letting
/// each backend consume a representation that matches its native APIs.
///
/// Guarantees:
/// - `WAIT_FOREVER` and `NO_WAIT` are preserved explicitly.
/// - Finite waits are exposed as 64-bit milliseconds for backend conversion.
/// - Integer conversions used by APIs like poll/epoll are clamped, never wrapped.
#pragma once

#include <osal/types.hpp>

#include <climits>
#include <cstdint>
#include <ctime>
#include <limits>

namespace osal::detail
{

enum class timeout_kind : std::uint8_t
{
    no_wait      = 0U,
    wait_forever = 1U,
    finite       = 2U,
};

struct timeout_value
{
    timeout_kind  kind{timeout_kind::no_wait};
    std::uint64_t finite_ms{0U};
};

struct backend_timeout_adapter
{
    [[nodiscard]] static constexpr timeout_value decode(osal::tick_t timeout) noexcept
    {
        if (timeout == osal::NO_WAIT)
        {
            return {timeout_kind::no_wait, 0U};
        }
        if (timeout == osal::WAIT_FOREVER)
        {
            return {timeout_kind::wait_forever, 0U};
        }
        return {timeout_kind::finite, static_cast<std::uint64_t>(timeout)};
    }

    [[nodiscard]] static int to_poll_timeout_ms(osal::tick_t timeout) noexcept
    {
        const timeout_value value = decode(timeout);
        if (value.kind == timeout_kind::wait_forever)
        {
            return -1;
        }
        if (value.kind == timeout_kind::no_wait)
        {
            return 0;
        }
        return (value.finite_ms > static_cast<std::uint64_t>(INT_MAX)) ? INT_MAX : static_cast<int>(value.finite_ms);
    }

    [[nodiscard]] static timespec to_abs_timespec(clockid_t clock_id, osal::tick_t timeout) noexcept
    {
        const timeout_value value = decode(timeout);
        const std::uint64_t ms    = (value.kind == timeout_kind::finite) ? value.finite_ms : 0U;

        struct timespec ts
        {
        };
        clock_gettime(clock_id, &ts);

        std::uint64_t seconds = ms / 1000U;
        ts.tv_nsec += static_cast<long>((ms % 1000U) * 1'000'000U);
        if (ts.tv_nsec >= 1'000'000'000L)
        {
            ++seconds;
            ts.tv_nsec -= 1'000'000'000L;
        }
        const auto max_seconds = std::numeric_limits<time_t>::max();
        if (seconds > static_cast<std::uint64_t>(max_seconds - ts.tv_sec))
        {
            ts.tv_sec  = max_seconds;
            ts.tv_nsec = 999'999'999L;
        }
        else
        {
            ts.tv_sec += static_cast<time_t>(seconds);
        }
        return ts;
    }
};

}  // namespace osal::detail
