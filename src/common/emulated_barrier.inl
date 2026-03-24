// SPDX-License-Identifier: Apache-2.0
/// @file emulated_barrier.inl
/// @brief Portable emulated barrier implementation.
/// @details Uses the OSAL's own extern "C" primitives (mutex + condvar) so the
///          same code works on every backend that exposes those MicrOSAL
///          building blocks. The barrier uses a generation counter so waiters
///          can distinguish the current rendezvous cycle from the next one.
///
///          #include inside the `extern "C"` block of any backend .cpp where
///          MicrOSAL provides barrier support via shared emulation.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#pragma once

#include <osal/detail/atomic_compat.hpp>

#ifndef OSAL_EMULATED_BARRIER_POOL_SIZE
/// @brief Maximum number of emulated barriers that can exist concurrently.
#define OSAL_EMULATED_BARRIER_POOL_SIZE 8U
#endif

namespace
{

struct emulated_barrier_obj
{
    osal::active_traits::mutex_handle_t   guard;
    osal::active_traits::condvar_handle_t cv;
    unsigned                              threshold;
    unsigned                              arrived;
    unsigned                              generation;
};

static emulated_barrier_obj emu_barrier_pool[OSAL_EMULATED_BARRIER_POOL_SIZE];
static std::atomic_bool     emu_barrier_used[OSAL_EMULATED_BARRIER_POOL_SIZE];

static emulated_barrier_obj* emu_barrier_acquire() noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_BARRIER_POOL_SIZE; ++i)
    {
        if (!emu_barrier_used[i].exchange(true, std::memory_order_acq_rel))
        {
            return &emu_barrier_pool[i];
        }
    }
    return nullptr;
}

static void emu_barrier_release(emulated_barrier_obj* barrier) noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_BARRIER_POOL_SIZE; ++i)
    {
        if (&emu_barrier_pool[i] == barrier)
        {
            emu_barrier_used[i].store(false, std::memory_order_release);
            return;
        }
    }
}

}  // namespace

/// @brief Create an emulated barrier.
/// @return `osal::ok()` on success, `invalid_argument` for null or zero-count,
///         `out_of_resources` if the pool is exhausted.
osal::result osal_barrier_create(osal::active_traits::barrier_handle_t* handle, unsigned count) noexcept
{
    if (!handle || (count == 0U))
    {
        if (handle)
        {
            handle->native = nullptr;
        }
        return osal::error_code::invalid_argument;
    }

    auto* barrier = emu_barrier_acquire();
    if (!barrier)
    {
        handle->native = nullptr;
        return osal::error_code::out_of_resources;
    }

    barrier->threshold  = count;
    barrier->arrived    = 0U;
    barrier->generation = 0U;

    if (!osal_mutex_create(&barrier->guard, false).ok())
    {
        emu_barrier_release(barrier);
        handle->native = nullptr;
        return osal::error_code::out_of_resources;
    }

    if (!osal_condvar_create(&barrier->cv).ok())
    {
        osal_mutex_destroy(&barrier->guard);
        emu_barrier_release(barrier);
        handle->native = nullptr;
        return osal::error_code::out_of_resources;
    }

    handle->native = barrier;
    return osal::ok();
}

/// @brief Destroy an emulated barrier and release its pool slot.
osal::result osal_barrier_destroy(osal::active_traits::barrier_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }

    auto* barrier = static_cast<emulated_barrier_obj*>(handle->native);
    osal_condvar_destroy(&barrier->cv);
    osal_mutex_destroy(&barrier->guard);
    emu_barrier_release(barrier);
    handle->native = nullptr;
    return osal::ok();
}

/// @brief Wait until the barrier threshold is reached.
/// @return `barrier_serial` for the releasing thread, `ok` for all others.
osal::result osal_barrier_wait(osal::active_traits::barrier_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }

    auto*        barrier = static_cast<emulated_barrier_obj*>(handle->native);
    osal::result r       = osal_mutex_lock(&barrier->guard, osal::WAIT_FOREVER);
    if (!r.ok())
    {
        return r;
    }

    const unsigned generation = barrier->generation;
    ++barrier->arrived;

    if (barrier->arrived == barrier->threshold)
    {
        barrier->arrived = 0U;
        ++barrier->generation;
        (void)osal_condvar_notify_all(&barrier->cv);
        (void)osal_mutex_unlock(&barrier->guard);
        return osal::error_code::barrier_serial;
    }

    while (generation == barrier->generation)
    {
        r = osal_condvar_wait(&barrier->cv, &barrier->guard, osal::WAIT_FOREVER);
        if (!r.ok())
        {
            (void)osal_mutex_unlock(&barrier->guard);
            return r;
        }
    }

    (void)osal_mutex_unlock(&barrier->guard);
    return osal::ok();
}
