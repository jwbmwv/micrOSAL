// SPDX-License-Identifier: Apache-2.0
/// @file emulated_condvar.inl
/// @brief Portable emulated condition-variable implementation.
/// @details Uses the OSAL's own extern "C" primitives (mutex, semaphore) so the
///          same code works on every backend that implements those primitives.
///          #include this file inside the `extern "C"` block of any backend .cpp
///          that does NOT have a native condition variable API.
///
///          The implementation follows Birrell's pattern with a bounded waiter
///          array.  Each waiting thread gets a binary semaphore slot.  notify_one
///          wakes the oldest waiter; notify_all wakes all.
///
///          Prerequisites at the point of inclusion:
///          - <osal/osal.hpp> already included with the correct OSAL_BACKEND_*
///          - osal_mutex_*, osal_semaphore_* already defined
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

// ---------------------------------------------------------------------------
// Condition variable (emulated — Birrell pattern: mutex + semaphore waiters)
// ---------------------------------------------------------------------------

#ifndef OSAL_CONDVAR_MAX_WAITERS
/// @brief Maximum number of threads that can simultaneously wait on a single
///        emulated condvar.  Increase if your application has more concurrent
///        waiters.
#define OSAL_CONDVAR_MAX_WAITERS 8U
#endif

#ifndef OSAL_EMULATED_CONDVAR_POOL_SIZE
/// @brief Maximum number of emulated condvars that can exist concurrently.
#define OSAL_EMULATED_CONDVAR_POOL_SIZE 16U
#endif

namespace  // anonymous — internal to the including TU
{

struct emulated_condvar_waiter
{
    osal::active_traits::semaphore_handle_t sem;     ///< Binary semaphore for this waiter.
    bool                                    in_use;  ///< Slot is occupied.
};

struct emulated_condvar_obj
{
    emulated_condvar_waiter             waiters[OSAL_CONDVAR_MAX_WAITERS];
    osal::active_traits::mutex_handle_t guard;           ///< Protects the waiter array.
    std::size_t                         n_waiters;       ///< Number of active waiters.
    bool                                pool_allocated;  ///< True if allocated from static pool.
};

static emulated_condvar_obj emu_cv_pool[OSAL_EMULATED_CONDVAR_POOL_SIZE];
static bool                 emu_cv_used[OSAL_EMULATED_CONDVAR_POOL_SIZE];

static emulated_condvar_obj* emu_cv_acquire() noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_CONDVAR_POOL_SIZE; ++i)
    {
        if (!emu_cv_used[i])
        {
            emu_cv_used[i] = true;
            return &emu_cv_pool[i];
        }
    }
    return nullptr;
}

static void emu_cv_release(emulated_condvar_obj* p) noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_CONDVAR_POOL_SIZE; ++i)
    {
        if (&emu_cv_pool[i] == p)
        {
            emu_cv_used[i] = false;
            return;
        }
    }
}

}  // anonymous namespace

// --- public extern "C" functions -------------------------------------------

/// @brief Create an emulated condition variable.
/// @param handle Output handle; populated on success.
/// @return `osal::ok()` on success, `error_code::invalid_argument` if @p handle is null,
///         `error_code::out_of_resources` if the pool is exhausted.
osal::result osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept
{
    if (!handle)
    {
        return osal::error_code::invalid_argument;
    }

    auto* cv = emu_cv_acquire();
    if (!cv)
    {
        return osal::error_code::out_of_resources;
    }

    cv->n_waiters      = 0U;
    cv->pool_allocated = true;
    for (auto& w : cv->waiters)
    {
        w.in_use = false;
    }

    // Create the internal guard mutex (non-recursive).
    if (!osal_mutex_create(&cv->guard, false).ok())
    {
        emu_cv_release(cv);
        return osal::error_code::out_of_resources;
    }

    // Pre-create binary semaphores for each waiter slot.
    for (auto& w : cv->waiters)
    {
        if (!osal_semaphore_create(&w.sem, 0U, 1U).ok())
        {
            // Cleanup already-created semaphores.
            for (auto& w2 : cv->waiters)
            {
                if (w2.in_use || (&w2 < &w))
                {
                    osal_semaphore_destroy(&w2.sem);
                }
            }
            osal_mutex_destroy(&cv->guard);
            emu_cv_release(cv);
            return osal::error_code::out_of_resources;
        }
    }

    handle->native = cv;
    return osal::ok();
}

/// @brief Destroy an emulated condition variable and release its pool slot.
/// @param handle Handle to destroy; silently ignored if null or already destroyed.
/// @return Always `osal::ok()`.
osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }
    auto* cv = static_cast<emulated_condvar_obj*>(handle->native);

    for (auto& w : cv->waiters)
    {
        osal_semaphore_destroy(&w.sem);
    }
    osal_mutex_destroy(&cv->guard);
    emu_cv_release(cv);
    handle->native = nullptr;
    return osal::ok();
}

/// @brief Atomically release @p mutex and block until notified or timeout.
/// @details Follows Birrell's condvar pattern: registers the caller in the waiter
///          array, releases @p mutex, blocks on a per-slot binary semaphore, then
///          re-acquires @p mutex before returning.
/// @param handle Condvar handle.
/// @param mutex  Mutex currently held by the caller; released during wait.
/// @param timeout Maximum ticks to wait; use `osal::WAIT_FOREVER` for indefinite.
/// @return `osal::ok()` on notification, `error_code::timeout` on expiry,
///         `error_code::not_initialized` if either handle is null,
///         `error_code::out_of_resources` if the waiter pool is full.
osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t* handle,
                               osal::active_traits::mutex_handle_t* mutex, osal::tick_t timeout) noexcept
{
    if (!handle || !handle->native || !mutex)
    {
        return osal::error_code::not_initialized;
    }
    auto* cv = static_cast<emulated_condvar_obj*>(handle->native);

    // Acquire the internal guard to find a free waiter slot.
    osal_mutex_lock(&cv->guard, osal::WAIT_FOREVER);

    emulated_condvar_waiter* my_slot = nullptr;
    for (auto& w : cv->waiters)
    {
        if (!w.in_use)
        {
            w.in_use = true;
            my_slot  = &w;
            break;
        }
    }

    if (!my_slot)
    {
        osal_mutex_unlock(&cv->guard);
        return osal::error_code::out_of_resources;  // Too many concurrent waiters.
    }

    ++cv->n_waiters;
    osal_mutex_unlock(&cv->guard);

    // Atomically: release the caller's mutex, then block on our semaphore.
    osal_mutex_unlock(mutex);

    osal::result r = osal_semaphore_take(&my_slot->sem, timeout);

    // Re-acquire the caller's mutex before returning.
    osal_mutex_lock(mutex, osal::WAIT_FOREVER);

    // Release the waiter slot.
    osal_mutex_lock(&cv->guard, osal::WAIT_FOREVER);
    my_slot->in_use = false;
    --cv->n_waiters;
    osal_mutex_unlock(&cv->guard);

    return r;
}

/// @brief Wake the oldest thread waiting on this condvar (signal semantics).
/// @param handle Condvar handle.
/// @return `osal::ok()` on success, `error_code::not_initialized` if null.
osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* cv = static_cast<emulated_condvar_obj*>(handle->native);

    osal_mutex_lock(&cv->guard, osal::WAIT_FOREVER);
    for (auto& w : cv->waiters)
    {
        if (w.in_use)
        {
            osal_semaphore_give(&w.sem);
            break;  // Wake only one.
        }
    }
    osal_mutex_unlock(&cv->guard);
    return osal::ok();
}

/// @brief Wake all threads waiting on this condvar (broadcast semantics).
/// @param handle Condvar handle.
/// @return `osal::ok()` on success, `error_code::not_initialized` if null.
osal::result osal_condvar_notify_all(osal::active_traits::condvar_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* cv = static_cast<emulated_condvar_obj*>(handle->native);

    osal_mutex_lock(&cv->guard, osal::WAIT_FOREVER);
    for (auto& w : cv->waiters)
    {
        if (w.in_use)
        {
            osal_semaphore_give(&w.sem);
        }
    }
    osal_mutex_unlock(&cv->guard);
    return osal::ok();
}
