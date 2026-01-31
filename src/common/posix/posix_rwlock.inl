// SPDX-License-Identifier: Apache-2.0
/// @file posix_rwlock.inl
/// @brief Shared pthread_rwlock_t implementation for POSIX-family backends.
/// @details Include inside the extern "C" block of any backend that uses
///          pthread_rwlock_t.  All six read-write lock functions are identical
///          across POSIX, Linux, NuttX and QNX modulo the clock used for timed
///          waits, which is encapsulated by the OSAL_POSIX_RW_ABS macro.
///
///          Required macro — define once in the owning .cpp before this include:
///          @code
///          // POSIX (CLOCK_REALTIME — mandated by pthread_rwlock_timedrdlock):
///          #define OSAL_POSIX_RW_ABS(t)  ms_to_abs_timespec_realtime(t)
///
///          // Linux / NuttX / QNX (CLOCK_MONOTONIC):
///          #define OSAL_POSIX_RW_ABS(t)  ms_to_abs_timespec(t)   // or ms_to_abs_mono(t)
///          @endcode
///
///          Prerequisites: <pthread.h> included, extern "C" scope active,
///                         OSAL_POSIX_RW_ABS(t) defined.

// -------------------------------------------------------------------------
// Read-write lock (native — pthread_rwlock_t)
// -------------------------------------------------------------------------

/// @brief Create a POSIX read-write lock via `pthread_rwlock_init`.
/// @param handle Output handle; populated on success.
/// @return `osal::ok()` on success, `error_code::invalid_argument` if null,
///         `error_code::out_of_resources` on allocation or init failure.
osal::result osal_rwlock_create(osal::active_traits::rwlock_handle_t* handle) noexcept
{
    if (!handle)
    {
        return osal::error_code::invalid_argument;
    }
    auto* rw = new (std::nothrow) pthread_rwlock_t;
    if (!rw)
    {
        return osal::error_code::out_of_resources;
    }
    const int rc = pthread_rwlock_init(rw, nullptr);
    if (rc != 0)
    {
        delete rw;
        return osal::error_code::out_of_resources;
    }
    handle->native = rw;
    return osal::ok();
}

/// @brief Destroy a POSIX read-write lock via `pthread_rwlock_destroy`.
/// @param handle Handle to destroy; silently ignored if null.
/// @return Always `osal::ok()`.
osal::result osal_rwlock_destroy(osal::active_traits::rwlock_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }
    pthread_rwlock_destroy(static_cast<pthread_rwlock_t*>(handle->native));
    delete static_cast<pthread_rwlock_t*>(handle->native);
    handle->native = nullptr;
    return osal::ok();
}

/// @brief Acquire the read lock via `pthread_rwlock_rdlock` or `pthread_rwlock_timedrdlock`.
/// @param handle        RW-lock handle.
/// @param timeout_ticks Ticks to wait; `WAIT_FOREVER` → blocking, `NO_WAIT` → try,
///                      other → `pthread_rwlock_timedrdlock` with absolute deadline
///                      from `OSAL_POSIX_RW_ABS(timeout_ticks)`.
/// @return `osal::ok()` on acquisition, `error_code::timeout` on failure,
///         `error_code::not_initialized` if null.
osal::result osal_rwlock_read_lock(osal::active_traits::rwlock_handle_t* handle, osal::tick_t timeout_ticks) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* rw = static_cast<pthread_rwlock_t*>(handle->native);
    if (timeout_ticks == osal::WAIT_FOREVER)
    {
        const int rc = pthread_rwlock_rdlock(rw);
        return (rc == 0) ? osal::ok() : osal::result{osal::error_code::unknown};
    }
    if (timeout_ticks == osal::NO_WAIT)
    {
        const int rc = pthread_rwlock_tryrdlock(rw);
        return (rc == 0) ? osal::ok() : osal::result{osal::error_code::timeout};
    }
    const struct timespec abs = OSAL_POSIX_RW_ABS(timeout_ticks);
    const int             rc  = pthread_rwlock_timedrdlock(rw, &abs);
    return (rc == 0) ? osal::ok() : osal::result{osal::error_code::timeout};
}

/// @brief Release a read lock via `pthread_rwlock_unlock`.
/// @param handle RW-lock handle.
/// @return `osal::ok()` on success, `error_code::not_initialized` if null,
///         `error_code::unknown` on pthread error.
osal::result osal_rwlock_read_unlock(osal::active_traits::rwlock_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    const int rc = pthread_rwlock_unlock(static_cast<pthread_rwlock_t*>(handle->native));
    return (rc == 0) ? osal::ok() : osal::result{osal::error_code::unknown};
}

/// @brief Acquire the write lock via `pthread_rwlock_wrlock` or `pthread_rwlock_timedwrlock`.
/// @param handle        RW-lock handle.
/// @param timeout_ticks Ticks to wait; `WAIT_FOREVER` → blocking, `NO_WAIT` → try,
///                      other → `pthread_rwlock_timedwrlock` with absolute deadline
///                      from `OSAL_POSIX_RW_ABS(timeout_ticks)`.
/// @return `osal::ok()` on acquisition, `error_code::timeout` on failure,
///         `error_code::not_initialized` if null.
osal::result osal_rwlock_write_lock(osal::active_traits::rwlock_handle_t* handle, osal::tick_t timeout_ticks) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* rw = static_cast<pthread_rwlock_t*>(handle->native);
    if (timeout_ticks == osal::WAIT_FOREVER)
    {
        const int rc = pthread_rwlock_wrlock(rw);
        return (rc == 0) ? osal::ok() : osal::result{osal::error_code::unknown};
    }
    if (timeout_ticks == osal::NO_WAIT)
    {
        const int rc = pthread_rwlock_trywrlock(rw);
        return (rc == 0) ? osal::ok() : osal::result{osal::error_code::timeout};
    }
    const struct timespec abs = OSAL_POSIX_RW_ABS(timeout_ticks);
    const int             rc  = pthread_rwlock_timedwrlock(rw, &abs);
    return (rc == 0) ? osal::ok() : osal::result{osal::error_code::timeout};
}

/// @brief Release a write lock via `pthread_rwlock_unlock`.
/// @param handle RW-lock handle.
/// @return `osal::ok()` on success, `error_code::not_initialized` if null,
///         `error_code::unknown` on pthread error.
osal::result osal_rwlock_write_unlock(osal::active_traits::rwlock_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    const int rc = pthread_rwlock_unlock(static_cast<pthread_rwlock_t*>(handle->native));
    return (rc == 0) ? osal::ok() : osal::result{osal::error_code::unknown};
}
