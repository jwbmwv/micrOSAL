// SPDX-License-Identifier: Apache-2.0
/// @file posix_condvar.inl
/// @brief Shared pthread_cond_t condvar implementation for POSIX-family backends.
/// @details Include inside the extern "C" block of POSIX, Linux and QNX backends.
///          NuttX keeps its own nxsem-based condvar because it does not expose a
///          full pthread condattr interface.
///
///          Required definitions before including this file:
///          1. `cond_init_monotonic(pthread_cond_t*)` — static helper that
///             initialises a condvar with CLOCK_MONOTONIC (or falls back to
///             CLOCK_REALTIME on platforms that lack it).
///          2. `OSAL_POSIX_COND_ABS(t)` — macro that converts a tick_t timeout
///             to an absolute struct timespec on the same clock used by
///             cond_init_monotonic.  Examples:
///             @code
///             // POSIX (kCondClock = CLOCK_MONOTONIC or CLOCK_REALTIME):
///             #define OSAL_POSIX_COND_ABS(t) ms_to_abs_timespec_cond(t)
///             // Linux:
///             #define OSAL_POSIX_COND_ABS(t) ms_to_abs_timespec(t)
///             // QNX:
///             #define OSAL_POSIX_COND_ABS(t) ms_to_abs_mono(t)
///             @endcode
///
///          The handle stores a raw `pthread_cond_t*` in handle->native.
///
///          Prerequisites: <pthread.h> included, extern "C" scope active,
///                         cond_init_monotonic and OSAL_POSIX_COND_ABS defined.

#pragma once

// -------------------------------------------------------------------------
// Condition variable (native — pthread_cond_t)
// -------------------------------------------------------------------------

/// @brief Create a POSIX condition variable using `CLOCK_MONOTONIC` if available.
/// @details Allocates a heap `pthread_cond_t` and initialises it via
///          `cond_init_monotonic()` (supplied by the including backend).
/// @param handle Output handle; populated on success.
/// @return `osal::ok()` on success, `error_code::invalid_argument` if null,
///         `error_code::out_of_resources` on allocation or init failure.
osal::result osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept
{
    if (!handle)
    {
        return osal::error_code::invalid_argument;
    }
    auto* cv = new (std::nothrow) pthread_cond_t;
    if (!cv)
    {
        return osal::error_code::out_of_resources;
    }
    if (cond_init_monotonic(cv) != 0)
    {
        delete cv;
        return osal::error_code::out_of_resources;
    }
    handle->native = cv;
    return osal::ok();
}

/// @brief Destroy a POSIX condition variable.
/// @param handle Handle to destroy; silently ignored if null.
/// @return Always `osal::ok()`.
osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }
    pthread_cond_destroy(static_cast<pthread_cond_t*>(handle->native));
    delete static_cast<pthread_cond_t*>(handle->native);
    handle->native = nullptr;
    return osal::ok();
}

/// @brief Atomically release @p mutex and block on the condvar until notified or timeout.
/// @details Delegates to `pthread_cond_wait` (infinite) or `pthread_cond_timedwait`
///          with an absolute deadline derived from `OSAL_POSIX_COND_ABS(timeout)`.
/// @param handle  Condvar handle.
/// @param mutex   Mutex currently held; released during wait.
/// @param timeout Maximum ticks to wait; use `osal::WAIT_FOREVER` for indefinite.
/// @return `osal::ok()` on notification, `error_code::timeout` on expiry,
///         `error_code::not_initialized` if either handle is null.
osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t* handle,
                               osal::active_traits::mutex_handle_t* mutex, osal::tick_t timeout) noexcept
{
    if (!handle || !handle->native || !mutex || !mutex->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* cv  = static_cast<pthread_cond_t*>(handle->native);
    auto* mtx = static_cast<pthread_mutex_t*>(mutex->native);

    if (timeout == osal::WAIT_FOREVER)
    {
        const int rc = pthread_cond_wait(cv, mtx);
        return (rc == 0) ? osal::ok() : osal::result{osal::error_code::unknown};
    }
    const struct timespec abs = OSAL_POSIX_COND_ABS(timeout);
    const int             rc  = pthread_cond_timedwait(cv, mtx, &abs);
    return (rc == 0) ? osal::ok() : osal::error_code::timeout;
}

/// @brief Wake one waiting thread via `pthread_cond_signal`.
/// @param handle Condvar handle.
/// @return `osal::ok()` on success, `error_code::not_initialized` if null.
osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    pthread_cond_signal(static_cast<pthread_cond_t*>(handle->native));
    return osal::ok();
}

/// @brief Wake all waiting threads via `pthread_cond_broadcast`.
/// @param handle Condvar handle.
/// @return `osal::ok()` on success, `error_code::not_initialized` if null.
osal::result osal_condvar_notify_all(osal::active_traits::condvar_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    pthread_cond_broadcast(static_cast<pthread_cond_t*>(handle->native));
    return osal::ok();
}
