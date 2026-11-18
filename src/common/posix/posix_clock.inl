// SPDX-License-Identifier: Apache-2.0
/// @file posix_clock.inl
/// @brief Shared clock implementation for POSIX-family backends.
/// @details Provides osal_clock_monotonic_ms and osal_clock_system_ms via
///          clock_gettime.  Include inside the extern "C" block of any POSIX,
///          Linux, NuttX or QNX backend translation unit.
///
///          Each backend still provides its own osal_clock_ticks() and
///          osal_clock_tick_period_us() because tick sources differ:
///          - Linux / POSIX / QNX : clock_gettime(CLOCK_MONOTONIC) in ms
///          - NuttX               : clock_systime_ticks() + CONFIG_USEC_PER_TICK
///
///          Prerequisites: <time.h> included, extern "C" scope active.

#pragma once

// Uses `clock_gettime(CLOCK_MONOTONIC)`.  The reference epoch is
// undefined but the value is strictly monotonically increasing.
std::int64_t osal_clock_monotonic_ms() noexcept
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000LL + static_cast<std::int64_t>(ts.tv_nsec) / 1'000'000LL;
}

// Uses `clock_gettime(CLOCK_REALTIME)`.
std::int64_t osal_clock_system_ms() noexcept
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000LL + static_cast<std::int64_t>(ts.tv_nsec) / 1'000'000LL;
}
