// SPDX-License-Identifier: Apache-2.0
/// @file emulated_rwlock.inl
/// @brief Portable emulated read-write lock implementation.
/// @details Uses the OSAL's own extern "C" primitives (mutex, condvar) so the
///          same code works on every backend that implements those primitives.
///          #include this file inside the `extern "C"` block of any backend .cpp
///          that does NOT have a native read-write lock API.
///
///          Implementation: a mutex protects [reader_count, writer_active].
///          Writers wait until reader_count == 0 && !writer_active.
///          Readers wait until !writer_active.
///          broadcast on unlock ensures all waiters re-check the predicate.
///
///          Prerequisites at the point of inclusion:
///          - <osal/osal.hpp> already included with the correct OSAL_BACKEND_*
///          - osal_mutex_*, osal_condvar_* already defined
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#pragma once

#include <atomic>

// ---------------------------------------------------------------------------
// Read-write lock (emulated — OSAL mutex + condvar + counter)
// ---------------------------------------------------------------------------

#ifndef OSAL_EMULATED_RWLOCK_POOL_SIZE
/// @brief Maximum number of emulated rwlocks that can exist concurrently.
#define OSAL_EMULATED_RWLOCK_POOL_SIZE 8U
#endif

namespace  // anonymous — internal to the including TU
{

struct emulated_rwlock_obj
{
    osal::active_traits::mutex_handle_t   guard;          ///< Protects the state below.
    osal::active_traits::condvar_handle_t cv;             ///< Signals state changes.
    std::size_t                           reader_count;   ///< Number of active readers.
    bool                                  writer_active;  ///< True while a writer holds the lock.
};

static emulated_rwlock_obj emu_rw_pool[OSAL_EMULATED_RWLOCK_POOL_SIZE];
static std::atomic_bool    emu_rw_used[OSAL_EMULATED_RWLOCK_POOL_SIZE];

static emulated_rwlock_obj* emu_rw_acquire() noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_RWLOCK_POOL_SIZE; ++i)
    {
        if (!emu_rw_used[i].exchange(true, std::memory_order_acq_rel))
        {
            return &emu_rw_pool[i];
        }
    }
    return nullptr;
}

static void emu_rw_release(emulated_rwlock_obj* p) noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_RWLOCK_POOL_SIZE; ++i)
    {
        if (&emu_rw_pool[i] == p)
        {
            emu_rw_used[i].store(false, std::memory_order_release);
            return;
        }
    }
}

}  // anonymous namespace

// --- public extern "C" functions -------------------------------------------

/// @brief Create an emulated read-write lock.
/// @details Internally allocates a guard mutex and a condvar from their
///          respective OSAL pools.
/// @param handle Output handle; populated on success.
/// @return `osal::ok()` on success, `error_code::invalid_argument` if null,
///         `error_code::out_of_resources` if the pool is exhausted.
osal::result osal_rwlock_create(osal::active_traits::rwlock_handle_t* handle) noexcept
{
    if (!handle)
    {
        return osal::error_code::invalid_argument;
    }

    auto* rw = emu_rw_acquire();
    if (!rw)
    {
        return osal::error_code::out_of_resources;
    }

    rw->reader_count  = 0U;
    rw->writer_active = false;

    if (!osal_mutex_create(&rw->guard, false).ok())
    {
        emu_rw_release(rw);
        return osal::error_code::out_of_resources;
    }

    if (!osal_condvar_create(&rw->cv).ok())
    {
        osal_mutex_destroy(&rw->guard);
        emu_rw_release(rw);
        return osal::error_code::out_of_resources;
    }

    handle->native = rw;
    return osal::ok();
}

/// @brief Destroy an emulated read-write lock and release its pool slot.
/// @param handle Handle to destroy; silently ignored if null.
/// @return Always `osal::ok()`.
osal::result osal_rwlock_destroy(osal::active_traits::rwlock_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }
    auto* rw = static_cast<emulated_rwlock_obj*>(handle->native);

    osal_condvar_destroy(&rw->cv);
    osal_mutex_destroy(&rw->guard);
    emu_rw_release(rw);
    handle->native = nullptr;
    return osal::ok();
}

/// @brief Acquire the read lock, blocking until no writer holds the lock.
/// @details Multiple readers may hold the lock concurrently.
/// @param handle        RW-lock handle.
/// @param timeout_ticks Maximum ticks to wait; use `osal::WAIT_FOREVER` for indefinite.
/// @return `osal::ok()` on acquisition, `error_code::timeout` on expiry,
///         `error_code::not_initialized` if @p handle is null.
osal::result osal_rwlock_read_lock(osal::active_traits::rwlock_handle_t* handle, osal::tick_t timeout_ticks) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* rw = static_cast<emulated_rwlock_obj*>(handle->native);

    osal_mutex_lock(&rw->guard, osal::WAIT_FOREVER);

    if (timeout_ticks == osal::WAIT_FOREVER)
    {
        while (rw->writer_active)
        {
            osal::result r = osal_condvar_wait(&rw->cv, &rw->guard, osal::WAIT_FOREVER);
            if (!r.ok())
            {
                osal_mutex_unlock(&rw->guard);
                return r;
            }
        }
    }
    else
    {
        // Compute deadline for timed wait.
        const osal::tick_t start = osal_clock_ticks();

        while (rw->writer_active)
        {
            const osal::tick_t elapsed = osal_clock_ticks() - start;  // wrap-safe
            if (elapsed >= timeout_ticks)
            {
                osal_mutex_unlock(&rw->guard);
                return osal::error_code::timeout;
            }
            const osal::tick_t remaining = timeout_ticks - elapsed;
            osal::result       r         = osal_condvar_wait(&rw->cv, &rw->guard, remaining);
            if (!r.ok() && r != osal::error_code::timeout)
            {
                osal_mutex_unlock(&rw->guard);
                return r;
            }
            // On timeout from condvar_wait, loop will re-check predicate and
            // the outer deadline check will return timeout if appropriate.
        }
    }

    ++rw->reader_count;
    osal_mutex_unlock(&rw->guard);
    return osal::ok();
}

/// @brief Release a previously acquired read lock.
/// @details If this is the last active reader, waiting writers are broadcast.
/// @param handle RW-lock handle.
/// @return `osal::ok()` on success, `error_code::invalid_argument` if not read-locked,
///         `error_code::not_initialized` if null.
osal::result osal_rwlock_read_unlock(osal::active_traits::rwlock_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* rw = static_cast<emulated_rwlock_obj*>(handle->native);

    osal_mutex_lock(&rw->guard, osal::WAIT_FOREVER);
    if (rw->reader_count == 0U)
    {
        osal_mutex_unlock(&rw->guard);
        return osal::error_code::invalid_argument;  // Not locked for reading.
    }
    --rw->reader_count;
    // If last reader, wake any waiting writer.
    if (rw->reader_count == 0U)
    {
        osal_condvar_notify_all(&rw->cv);
    }
    osal_mutex_unlock(&rw->guard);
    return osal::ok();
}

/// @brief Acquire the write lock exclusively (no readers or other writers).
/// @param handle        RW-lock handle.
/// @param timeout_ticks Maximum ticks to wait; use `osal::WAIT_FOREVER` for indefinite.
/// @return `osal::ok()` on acquisition, `error_code::timeout` on expiry,
///         `error_code::not_initialized` if @p handle is null.
osal::result osal_rwlock_write_lock(osal::active_traits::rwlock_handle_t* handle, osal::tick_t timeout_ticks) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* rw = static_cast<emulated_rwlock_obj*>(handle->native);

    osal_mutex_lock(&rw->guard, osal::WAIT_FOREVER);

    if (timeout_ticks == osal::WAIT_FOREVER)
    {
        while (rw->writer_active || rw->reader_count > 0U)
        {
            osal::result r = osal_condvar_wait(&rw->cv, &rw->guard, osal::WAIT_FOREVER);
            if (!r.ok())
            {
                osal_mutex_unlock(&rw->guard);
                return r;
            }
        }
    }
    else
    {
        const osal::tick_t start = osal_clock_ticks();

        while (rw->writer_active || rw->reader_count > 0U)
        {
            const osal::tick_t elapsed = osal_clock_ticks() - start;  // wrap-safe
            if (elapsed >= timeout_ticks)
            {
                osal_mutex_unlock(&rw->guard);
                return osal::error_code::timeout;
            }
            const osal::tick_t remaining = timeout_ticks - elapsed;
            osal::result       r         = osal_condvar_wait(&rw->cv, &rw->guard, remaining);
            if (!r.ok() && r != osal::error_code::timeout)
            {
                osal_mutex_unlock(&rw->guard);
                return r;
            }
        }
    }

    rw->writer_active = true;
    osal_mutex_unlock(&rw->guard);
    return osal::ok();
}

/// @brief Release the write lock and broadcast to all waiters.
/// @param handle RW-lock handle.
/// @return `osal::ok()` on success, `error_code::invalid_argument` if not write-locked,
///         `error_code::not_initialized` if null.
osal::result osal_rwlock_write_unlock(osal::active_traits::rwlock_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* rw = static_cast<emulated_rwlock_obj*>(handle->native);

    osal_mutex_lock(&rw->guard, osal::WAIT_FOREVER);
    if (!rw->writer_active)
    {
        osal_mutex_unlock(&rw->guard);
        return osal::error_code::invalid_argument;  // Not locked for writing.
    }
    rw->writer_active = false;
    osal_condvar_notify_all(&rw->cv);
    osal_mutex_unlock(&rw->guard);
    return osal::ok();
}
