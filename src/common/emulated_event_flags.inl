// SPDX-License-Identifier: Apache-2.0
/// @file emulated_event_flags.inl
/// @brief Portable emulated event-flags implementation.
/// @details Uses the OSAL's own extern "C" primitives (mutex, semaphore) so the
///          same code works on every backend that implements those primitives.
///          #include this file inside the `extern "C"` block of any backend .cpp
///          that does NOT have a native event-flag-group API.
///
///          Design (per-waiter semaphore array — inspired by Birrell's condvar):
///          ──────────────────────────────────────────────────────────────────────
///          • A guard mutex protects the bits variable and the waiter array.
///          • Each waiting thread occupies a slot with a binary semaphore.
///          • set() : lock guard, OR bits, give EVERY in‑use waiter semaphore,
///                    unlock guard.
///          • set_isr() : atomic OR on bits,
///                        give_isr every in‑use waiter semaphore.
///          • clear() : lock guard, AND‑invert bits, unlock guard.
///          • get() : atomic read.
///          • wait_any/all() : lock guard → quick-check → if not matched,
///                  claim waiter slot → unlock guard → loop
///                  {semaphore_take(remaining) → lock guard → re-check → …}
///                  → release slot → unlock guard.
///
///          ISR safety: set_isr uses atomic bit updates plus osal_semaphore_give_isr.
///          Backends without has_isr_event_flags support reject set_isr() rather
///          than silently degrading to a polling-only wakeup.
///
///          Prerequisites at the point of inclusion:
///          - <osal/osal.hpp> already included with the correct OSAL_BACKEND_*
///          - osal_mutex_*, osal_semaphore_*, osal_clock_ticks already defined
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#pragma once

#include <osal/detail/atomic_compat.hpp>

// ---------------------------------------------------------------------------
// Event flags (emulated — OSAL mutex + per-waiter semaphores)
// ---------------------------------------------------------------------------

#ifndef OSAL_EVENT_FLAGS_MAX_WAITERS
/// @brief Maximum number of threads that can simultaneously wait on a single
///        emulated event-flags group.
#define OSAL_EVENT_FLAGS_MAX_WAITERS 8U
#endif

#ifndef OSAL_EMULATED_EVENT_FLAGS_POOL_SIZE
/// @brief Maximum number of emulated event-flag groups that can exist concurrently.
#define OSAL_EMULATED_EVENT_FLAGS_POOL_SIZE 8U
#endif

namespace  // anonymous — internal to the including TU
{

struct emu_ef_waiter
{
    osal::active_traits::semaphore_handle_t sem;     ///< Binary semaphore for this waiter.
    std::atomic_bool                        in_use;  ///< Slot is occupied by a waiting thread.
};

struct emu_ef_obj
{
    osal::active_traits::mutex_handle_t guard;                                  ///< Protects bits and waiter array.
    emu_ef_waiter                       waiters[OSAL_EVENT_FLAGS_MAX_WAITERS];  ///< Per-waiter semaphore slots.
    std::atomic<osal::event_bits_t>     bits;                                   ///< The flag bits.
};

static emu_ef_obj       emu_ef_pool[OSAL_EMULATED_EVENT_FLAGS_POOL_SIZE];
static std::atomic_bool emu_ef_used[OSAL_EMULATED_EVENT_FLAGS_POOL_SIZE];

static emu_ef_obj* emu_ef_acquire() noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_EVENT_FLAGS_POOL_SIZE; ++i)
    {
        if (!emu_ef_used[i].exchange(true, std::memory_order_acq_rel))
        {
            return &emu_ef_pool[i];
        }
    }
    return nullptr;
}

static void emu_ef_release(emu_ef_obj* p) noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_EVENT_FLAGS_POOL_SIZE; ++i)
    {
        if (&emu_ef_pool[i] == p)
        {
            emu_ef_used[i].store(false, std::memory_order_release);
            return;
        }
    }
}

/// @brief Wake all waiters by giving each one's semaphore (task context).
static void emu_ef_wake_all(emu_ef_obj* ef) noexcept
{
    for (auto& w : ef->waiters)
    {
        if (w.in_use.load(std::memory_order_acquire))
        {
            osal_semaphore_give(&w.sem);
        }
    }
}

/// @brief Timed event-flags wait helper (shared by wait_any and wait_all).
static osal::result emu_ef_wait(emu_ef_obj* ef, osal::event_bits_t wait_bits, osal::event_bits_t* actual,
                                bool clear_on_exit, bool all, osal::tick_t timeout) noexcept
{
    auto load_bits = [&]() noexcept -> osal::event_bits_t { return ef->bits.load(std::memory_order_acquire); };
    auto matched   = [&]() -> bool
    {
        const osal::event_bits_t bits = load_bits();
        return all ? ((bits & wait_bits) == wait_bits) : ((bits & wait_bits) != 0U);
    };

    // ------- Quick check under lock ---------------------------------------
    osal_mutex_lock(&ef->guard, osal::WAIT_FOREVER);
    if (matched())
    {
        const osal::event_bits_t bits = load_bits();
        if (actual != nullptr)
        {
            *actual = bits;
        }
        if (clear_on_exit)
        {
            (void)ef->bits.fetch_and(~wait_bits, std::memory_order_acq_rel);
        }
        osal_mutex_unlock(&ef->guard);
        return osal::ok();
    }

    if (timeout == osal::NO_WAIT)
    {
        if (actual != nullptr)
        {
            *actual = load_bits();
        }
        osal_mutex_unlock(&ef->guard);
        return osal::error_code::would_block;
    }

    // ------- Claim a waiter slot ------------------------------------------
    emu_ef_waiter* my_slot = nullptr;
    for (auto& w : ef->waiters)
    {
        if (!w.in_use.load(std::memory_order_acquire))
        {
            (void)osal_semaphore_try_take(&w.sem);
            w.in_use.store(true, std::memory_order_release);
            my_slot = &w;
            break;
        }
    }
    if (my_slot == nullptr)
    {
        osal_mutex_unlock(&ef->guard);
        return osal::error_code::out_of_resources;
    }
    osal_mutex_unlock(&ef->guard);

    // ------- Blocking loop ------------------------------------------------
    // Capture start tick for wrap-safe elapsed comparisons.
    // Using (now - start) >= timeout is correct across uint32_t rollover:
    // unsigned subtraction is always well-defined and naturally handles wrap.
    const osal::tick_t start = osal_clock_ticks();

    for (;;)
    {
        // Determine remaining time.
        osal::tick_t remaining = osal::WAIT_FOREVER;
        if (timeout != osal::WAIT_FOREVER)
        {
            const osal::tick_t now     = osal_clock_ticks();
            const osal::tick_t elapsed = now - start;  // wrap-safe
            if (elapsed >= timeout)
            {
                // Timed out.
                osal_mutex_lock(&ef->guard, osal::WAIT_FOREVER);
                if (actual != nullptr)
                {
                    *actual = load_bits();
                }
                my_slot->in_use.store(false, std::memory_order_release);
                osal_mutex_unlock(&ef->guard);
                return osal::error_code::timeout;
            }
            remaining = timeout - elapsed;
        }

        // Block until set() or timeout.
        osal_semaphore_take(&my_slot->sem, remaining);

        // Re-check bits under lock.
        osal_mutex_lock(&ef->guard, osal::WAIT_FOREVER);
        if (matched())
        {
            const osal::event_bits_t bits = load_bits();
            if (actual != nullptr)
            {
                *actual = bits;
            }
            if (clear_on_exit)
            {
                (void)ef->bits.fetch_and(~wait_bits, std::memory_order_acq_rel);
            }
            my_slot->in_use.store(false, std::memory_order_release);
            osal_mutex_unlock(&ef->guard);
            return osal::ok();
        }
        osal_mutex_unlock(&ef->guard);

        // Not matched — check deadline again on next iteration.
    }
}

}  // anonymous namespace

// --- public extern "C" functions -------------------------------------------

/// @brief Create an emulated event-flags group.
/// @param handle Output handle; populated on success.
/// @return `osal::ok()` on success, `error_code::invalid_argument` if @p handle is null,
///         `error_code::out_of_resources` if the pool or internal semaphores are exhausted.
osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
{
    if (!handle)
    {
        return osal::error_code::invalid_argument;
    }

    auto* ef = emu_ef_acquire();
    if (!ef)
    {
        return osal::error_code::out_of_resources;
    }

    ef->bits.store(0U, std::memory_order_relaxed);
    for (auto& w : ef->waiters)
    {
        w.in_use.store(false, std::memory_order_relaxed);
    }

    // Create the internal guard mutex (non-recursive).
    if (!osal_mutex_create(&ef->guard, false).ok())
    {
        emu_ef_release(ef);
        return osal::error_code::out_of_resources;
    }

    // Pre-create binary semaphores for each waiter slot.
    for (std::size_t i = 0U; i < OSAL_EVENT_FLAGS_MAX_WAITERS; ++i)
    {
        if (!osal_semaphore_create(&ef->waiters[i].sem, 0U, 1U).ok())
        {
            // Cleanup already-created semaphores.
            for (std::size_t j = 0U; j < i; ++j)
            {
                osal_semaphore_destroy(&ef->waiters[j].sem);
            }
            osal_mutex_destroy(&ef->guard);
            emu_ef_release(ef);
            return osal::error_code::out_of_resources;
        }
    }

    handle->native = ef;
    return osal::ok();
}

/// @brief Destroy an emulated event-flags group and release its pool slot.
/// @param handle Handle to destroy; silently ignored if null or already destroyed.
/// @return Always `osal::ok()`.
osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }
    auto* ef = static_cast<emu_ef_obj*>(handle->native);

    for (auto& w : ef->waiters)
    {
        osal_semaphore_destroy(&w.sem);
    }
    osal_mutex_destroy(&ef->guard);
    emu_ef_release(ef);
    handle->native = nullptr;
    return osal::ok();
}

/// @brief Set (OR) the specified bits in the event-flags group.
/// @details Wakes every thread currently waiting on this group so each can
///          re-evaluate its wait condition.
/// @param handle Event-flags handle.
/// @param bits   Bit mask to OR into the current flags.
/// @return `osal::ok()` on success, `error_code::not_initialized` if null.
osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* ef = static_cast<emu_ef_obj*>(handle->native);
    osal_mutex_lock(&ef->guard, osal::WAIT_FOREVER);
    (void)ef->bits.fetch_or(bits, std::memory_order_acq_rel);
    emu_ef_wake_all(ef);
    osal_mutex_unlock(&ef->guard);
    return osal::ok();
}

/// @brief Clear (AND-invert) the specified bits in the event-flags group.
/// @param handle Event-flags handle.
/// @param bits   Bit mask to clear.
/// @return `osal::ok()` on success, `error_code::not_initialized` if null.
osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* ef = static_cast<emu_ef_obj*>(handle->native);
    osal_mutex_lock(&ef->guard, osal::WAIT_FOREVER);
    (void)ef->bits.fetch_and(~bits, std::memory_order_acq_rel);
    osal_mutex_unlock(&ef->guard);
    return osal::ok();
}

/// @brief Read the current flag bits without blocking (atomic read).
/// @param handle Event-flags handle (const).
/// @return Current bit pattern, or 0 if @p handle is null.
osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return 0U;
    }
    return static_cast<const emu_ef_obj*>(handle->native)->bits.load(std::memory_order_acquire);
}

/// @brief Wait until any of the specified bits are set (OR-wait).
/// @param handle        Event-flags handle.
/// @param wait_bits     Bit mask to wait for (any bit is sufficient).
/// @param actual        If non-null, receives the flag value at the time of wakeup.
/// @param clear_on_exit If true, the matched bits are cleared before returning.
/// @param timeout       Maximum ticks to wait; use `osal::WAIT_FOREVER` for indefinite.
/// @return `osal::ok()` when matched, `error_code::would_block` if `NO_WAIT` and no
///         bits set, `error_code::timeout` on expiry, `error_code::not_initialized` if null.
osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t wait_bits,
                                       osal::event_bits_t* actual, bool clear_on_exit, osal::tick_t timeout) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    return emu_ef_wait(static_cast<emu_ef_obj*>(handle->native), wait_bits, actual, clear_on_exit, false, timeout);
}

/// @brief Wait until all of the specified bits are set (AND-wait).
/// @param handle        Event-flags handle.
/// @param wait_bits     Bit mask to wait for (all bits must be set).
/// @param actual        If non-null, receives the flag value at the time of wakeup.
/// @param clear_on_exit If true, the matched bits are cleared before returning.
/// @param timeout       Maximum ticks to wait; use `osal::WAIT_FOREVER` for indefinite.
/// @return `osal::ok()` when all matched, `error_code::would_block` if `NO_WAIT`,
///         `error_code::timeout` on expiry, `error_code::not_initialized` if null.
osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t wait_bits,
                                       osal::event_bits_t* actual, bool clear_on_exit, osal::tick_t timeout) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    return emu_ef_wait(static_cast<emu_ef_obj*>(handle->native), wait_bits, actual, clear_on_exit, true, timeout);
}

/// @brief Set bits from ISR context (atomic OR + ISR-safe semaphore give).
/// @details Backends without `has_isr_event_flags == true` reject this call with
///          `error_code::not_supported` rather than silently degrading the wakeup semantics.
/// @param handle Event-flags handle.
/// @param bits   Bit mask to OR into the current flags.
/// @return `osal::ok()` on success, `error_code::not_supported` when ISR event flags
///         are unavailable, or `error_code::not_initialized` if null.
osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }

    if constexpr (!osal::capabilities<osal::active_backend>::has_isr_event_flags)
    {
        return osal::error_code::not_supported;
    }

    auto* ef = static_cast<emu_ef_obj*>(handle->native);

    (void)ef->bits.fetch_or(bits, std::memory_order_acq_rel);

    for (auto& w : ef->waiters)
    {
        if (w.in_use.load(std::memory_order_acquire))
        {
            osal_semaphore_give_isr(&w.sem);
        }
    }
    return osal::ok();
}
