// SPDX-License-Identifier: Apache-2.0
/// @file posix_barrier.inl
/// @brief Native pthread_barrier_t implementation for POSIX-family backends.
/// @details Implements the three OSAL barrier functions using
///          @c pthread_barrier_init / @c pthread_barrier_destroy /
///          @c pthread_barrier_wait.
///
///          All four POSIX-family backends (POSIX, Linux, NuttX, QNX) share
///          this implementation.  @c pthread_barrier_wait does not take a
///          timeout so no clock macro is required.
///
///          @c PTHREAD_BARRIER_SERIAL_THREAD is mapped to
///          @c osal::error_code::barrier_serial; all other threads get
///          @c osal::ok().
///
///          Prerequisites: @c <pthread.h> included, @c extern "C" scope active.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#pragma once

// ---------------------------------------------------------------------------
// Barrier (native — pthread_barrier_t)
// ---------------------------------------------------------------------------

osal::result osal_barrier_create(osal::active_traits::barrier_handle_t* handle, unsigned count) noexcept
{
    if (!handle || count == 0U)
    {
        return osal::error_code::invalid_argument;
    }
    auto* b = new (std::nothrow) pthread_barrier_t;
    if (!b)
    {
        return osal::error_code::out_of_resources;
    }
    const int rc = pthread_barrier_init(b, nullptr, count);
    if (rc != 0)
    {
        delete b;
        return osal::error_code::out_of_resources;
    }
    handle->native = b;
    return osal::ok();
}

osal::result osal_barrier_destroy(osal::active_traits::barrier_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }
    pthread_barrier_destroy(static_cast<pthread_barrier_t*>(handle->native));
    delete static_cast<pthread_barrier_t*>(handle->native);
    handle->native = nullptr;
    return osal::ok();
}

// Blocks until count threads have called this function.  The
// last (Nth) arriving thread receives
// error_code::barrier_serial (matching
// PTHREAD_BARRIER_SERIAL_THREAD semantics); all other threads
// receive osal::ok().
osal::result osal_barrier_wait(osal::active_traits::barrier_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    const int rc = pthread_barrier_wait(static_cast<pthread_barrier_t*>(handle->native));
    if (rc == PTHREAD_BARRIER_SERIAL_THREAD)
    {
        return osal::error_code::barrier_serial;
    }
    if (rc != 0)
    {
        return osal::error_code::unknown;
    }
    return osal::ok();
}
