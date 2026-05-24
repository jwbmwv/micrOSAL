// SPDX-License-Identifier: Apache-2.0
/// @file emulated_work_queue.inl
/// @brief Portable emulated work-queue implementation.
/// @details Uses the OSAL's own extern "C" primitives (thread, mutex, semaphore)
///          so the same code works on every backend that implements those
///          primitives.  #include this file inside the `extern "C"` block of any
///          backend .cpp that does NOT have a native work-queue API.
///
///          Prerequisites at the point of inclusion:
///          - <osal/osal.hpp> already included with the correct OSAL_BACKEND_*
///          - osal_thread_create, osal_mutex_*, osal_semaphore_* already defined
///          - osal_clock_tick_period_us() already defined
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#pragma once

#include <osal/detail/atomic_compat.hpp>

// ---------------------------------------------------------------------------
// Work queue (emulated — OSAL thread + mutex + semaphore + ring buffer)
// ---------------------------------------------------------------------------

#ifndef OSAL_EMULATED_WQ_POOL_SIZE
/// @brief Maximum number of emulated work queues that can exist concurrently.
#define OSAL_EMULATED_WQ_POOL_SIZE 4U
#endif

#ifndef OSAL_EMULATED_WQ_MAX_DEPTH
/// @brief Maximum depth (number of pending items) for a single emulated work queue.
///        The actual depth at creation may be smaller, but cannot exceed this.
#define OSAL_EMULATED_WQ_MAX_DEPTH 32U
#endif

namespace  // anonymous — internal to the including TU
{

static std::size_t emu_wq_advance_index(std::size_t index, std::size_t capacity) noexcept
{
    ++index;
    return (index == capacity) ? 0U : index;
}

static std::size_t emu_wq_retreat_index(std::size_t index, std::size_t capacity) noexcept
{
    return (index == 0U) ? (capacity - 1U) : (index - 1U);
}

struct emulated_wq_entry
{
    osal_work_func_t func{};
    void*            arg{};
    std::size_t      sequence{};
};

struct emulated_wq_obj
{
    emulated_wq_entry ring[OSAL_EMULATED_WQ_MAX_DEPTH];  ///< Static ring buffer.
    std::size_t       capacity;
    std::size_t       head;   ///< Next slot to dequeue from.
    std::size_t       tail;   ///< Next slot to enqueue into.
    std::size_t       count;  ///< Current number of items.

    osal::active_traits::thread_handle_t    worker;
    osal::active_traits::mutex_handle_t     mtx;
    osal::active_traits::semaphore_handle_t not_empty;  ///< Counting sem — items available.
    osal::active_traits::condvar_handle_t   flush_cv;   ///< Broadcast when execution advances.

    std::size_t submitted_sequence;  ///< Last sequence assigned to an accepted work item.
    std::size_t completed_sequence;  ///< Last sequence that actually finished executing.
    std::size_t active_sequence;     ///< Sequence currently executing on the worker (0 when idle).

    bool stop;           ///< Destroy requested; protected by @ref mtx.
    bool worker_active;  ///< A work item is currently executing; protected by @ref mtx.
};

static emulated_wq_obj  emu_wq_pool[OSAL_EMULATED_WQ_POOL_SIZE];
static std::atomic_bool emu_wq_used[OSAL_EMULATED_WQ_POOL_SIZE];

static emulated_wq_obj* emu_wq_acquire() noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_WQ_POOL_SIZE; ++i)
    {
        if (!emu_wq_used[i].exchange(true, std::memory_order_acq_rel))
        {
            return &emu_wq_pool[i];
        }
    }
    return nullptr;
}

static void emu_wq_release(emulated_wq_obj* p) noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_WQ_POOL_SIZE; ++i)
    {
        if (&emu_wq_pool[i] == p)
        {
            emu_wq_used[i].store(false, std::memory_order_release);
            return;
        }
    }
}

/// Worker thread entry — dequeue items and execute them.
static void wq_worker_entry(void* arg) noexcept
{
    auto* wq = static_cast<emulated_wq_obj*>(arg);

    while (true)
    {
        // Block until an item is available.
        osal_semaphore_take(&wq->not_empty, osal::WAIT_FOREVER);

        osal_mutex_lock(&wq->mtx, osal::WAIT_FOREVER);

        if (wq->count == 0U && wq->stop)
        {
            osal_mutex_unlock(&wq->mtx);
            break;
        }

        if (wq->count == 0U)
        {
            // Spurious wakeup (e.g. after cancel_all drained the sem).
            osal_mutex_unlock(&wq->mtx);
            continue;
        }

        const emulated_wq_entry entry = wq->ring[wq->head];
        wq->head                      = emu_wq_advance_index(wq->head, wq->capacity);
        --wq->count;
        wq->worker_active   = true;
        wq->active_sequence = entry.sequence;
        osal_mutex_unlock(&wq->mtx);

        entry.func(entry.arg);

        osal_mutex_lock(&wq->mtx, osal::WAIT_FOREVER);
        wq->worker_active      = false;
        wq->active_sequence    = 0U;
        wq->completed_sequence = entry.sequence;
        osal_condvar_notify_all(&wq->flush_cv);
        osal_mutex_unlock(&wq->mtx);
    }
}

}  // anonymous namespace

// --- public extern "C" functions -------------------------------------------

/// @brief Create an emulated work queue backed by a dedicated OSAL thread.
/// @details Spawns a worker thread that dequeues and executes submitted work items
///          in FIFO order.  The ring buffer capacity is capped at `OSAL_EMULATED_WQ_MAX_DEPTH`.
/// @param handle      Output handle; populated on success.
/// @param stack       Stack buffer for the worker thread.
/// @param stack_bytes Size of @p stack in bytes.
/// @param depth       Maximum pending items; must be <= `OSAL_EMULATED_WQ_MAX_DEPTH`.
/// @param name        Optional thread name passed to `osal_thread_create`.
/// @return `osal::ok()` on success, `error_code::invalid_argument` if @p handle is null
///         or @p depth exceeds the maximum, `error_code::out_of_resources` if the
///         pool or worker thread cannot be created.
osal::result osal_work_queue_create(osal::active_traits::work_queue_handle_t* handle, void* stack,
                                    std::size_t stack_bytes, std::size_t depth, const char* name) noexcept
{
    if (!handle || depth == 0U) [[unlikely]]
    {
        return osal::error_code::invalid_argument;
    }

    auto* wq = emu_wq_acquire();
    if (!wq)
    {
        return osal::error_code::out_of_resources;
    }

    if (depth > OSAL_EMULATED_WQ_MAX_DEPTH) [[unlikely]]
    {
        emu_wq_release(wq);
        return osal::error_code::invalid_argument;
    }

    wq->capacity           = depth;
    wq->head               = 0U;
    wq->tail               = 0U;
    wq->count              = 0U;
    wq->submitted_sequence = 0U;
    wq->completed_sequence = 0U;
    wq->active_sequence    = 0U;
    wq->stop               = false;
    wq->worker_active      = false;

    if (!osal_mutex_create(&wq->mtx, false).ok())
    {
        emu_wq_release(wq);
        return osal::error_code::out_of_resources;
    }

    if (!osal_semaphore_create(&wq->not_empty, 0U, static_cast<unsigned>(depth + 1U)).ok())
    {
        osal_mutex_destroy(&wq->mtx);
        emu_wq_release(wq);
        return osal::error_code::out_of_resources;
    }

    if (!osal_condvar_create(&wq->flush_cv).ok())
    {
        osal_semaphore_destroy(&wq->not_empty);
        osal_mutex_destroy(&wq->mtx);
        emu_wq_release(wq);
        return osal::error_code::out_of_resources;
    }

    if (!osal_thread_create(&wq->worker, wq_worker_entry, static_cast<void*>(wq), osal::PRIORITY_NORMAL,
                            osal::AFFINITY_ANY, stack, static_cast<osal::stack_size_t>(stack_bytes), name)
             .ok())
    {
        osal_condvar_destroy(&wq->flush_cv);
        osal_semaphore_destroy(&wq->not_empty);
        osal_mutex_destroy(&wq->mtx);
        emu_wq_release(wq);
        return osal::error_code::out_of_resources;
    }

    handle->native = static_cast<void*>(wq);
    return osal::ok();
}

/// @brief Destroy an emulated work queue.
/// @details Signals the worker thread to stop (after draining remaining items),
///          joins it, and releases all resources.
/// @param handle Handle to destroy; silently ignored if null.
/// @return Always `osal::ok()`.
osal::result osal_work_queue_destroy(osal::active_traits::work_queue_handle_t* handle) noexcept
{
    if (!handle || !handle->native) [[unlikely]]
    {
        return osal::ok();
    }
    auto* wq = static_cast<emulated_wq_obj*>(handle->native);

    osal_mutex_lock(&wq->mtx, osal::WAIT_FOREVER);
    wq->stop = true;
    osal_semaphore_give(&wq->not_empty);
    osal_mutex_unlock(&wq->mtx);

    osal_thread_join(&wq->worker, osal::WAIT_FOREVER);

    osal_condvar_destroy(&wq->flush_cv);
    osal_semaphore_destroy(&wq->not_empty);
    osal_mutex_destroy(&wq->mtx);
    emu_wq_release(wq);
    handle->native = nullptr;
    return osal::ok();
}

/// @brief Submit a work item to the queue (task context).
/// @param handle Queue handle.
/// @param func   Work function to execute; must not be null.
/// @param arg    Opaque argument passed to @p func.
/// @return `osal::ok()` on success, `error_code::invalid_argument` if @p func is null,
///         `error_code::overflow` if the ring buffer is full,
///         `error_code::not_initialized` if @p handle is null.
osal::result osal_work_queue_submit(osal::active_traits::work_queue_handle_t* handle, osal_work_func_t func,
                                    void* arg) noexcept
{
    if (!handle || !handle->native) [[unlikely]]
    {
        return osal::error_code::not_initialized;
    }
    if (!func) [[unlikely]]
    {
        return osal::error_code::invalid_argument;
    }
    auto* wq = static_cast<emulated_wq_obj*>(handle->native);

    osal_mutex_lock(&wq->mtx, osal::WAIT_FOREVER);
    if (wq->count >= wq->capacity)
    {
        osal_mutex_unlock(&wq->mtx);
        return osal::error_code::overflow;
    }
    const std::size_t sequence = ++wq->submitted_sequence;
    wq->ring[wq->tail]         = emulated_wq_entry{func, arg, sequence};
    wq->tail                   = emu_wq_advance_index(wq->tail, wq->capacity);
    ++wq->count;
    osal_mutex_unlock(&wq->mtx);

    osal_semaphore_give(&wq->not_empty);
    return osal::ok();
}

/// @brief Submit a work item from ISR context — not supported by this emulation.
/// @details The emulated work queue uses a mutex internally, which is not ISR-safe.
///          Always returns `error_code::not_supported`.
/// @param handle Ignored.
/// @param func   Ignored.
/// @param arg    Ignored.
/// @return Always `error_code::not_supported`.
osal::result osal_work_queue_submit_from_isr(osal::active_traits::work_queue_handle_t* /*handle*/,
                                             osal_work_func_t /*func*/, void* /*arg*/) noexcept
{
    return osal::error_code::not_supported;
}

/// @brief Wait until all currently queued or executing items have been executed.
/// @details Captures the last live work-item sequence at call time and waits until execution
///          reaches that frontier. Items canceled after the flush starts do not satisfy it.
/// @param handle  Queue handle.
/// @param timeout Maximum ticks to wait; use `osal::WAIT_FOREVER` for indefinite.
/// @return `osal::ok()` once flushed, `error_code::timeout` on expiry,
///         `error_code::not_initialized` if null.
osal::result osal_work_queue_flush(osal::active_traits::work_queue_handle_t* handle, osal::tick_t timeout) noexcept
{
    if (!handle || !handle->native) [[unlikely]]
    {
        return osal::error_code::not_initialized;
    }
    auto*              wq    = static_cast<emulated_wq_obj*>(handle->native);
    const osal::tick_t start = (timeout == osal::WAIT_FOREVER) ? 0U : osal_clock_ticks();

    osal_mutex_lock(&wq->mtx, osal::WAIT_FOREVER);
    std::size_t target_sequence = wq->completed_sequence;
    if (wq->count != 0U)
    {
        const std::size_t last_pending_index = emu_wq_retreat_index(wq->tail, wq->capacity);
        target_sequence                      = wq->ring[last_pending_index].sequence;
    }
    else if (wq->worker_active)
    {
        target_sequence = wq->active_sequence;
    }

    if (wq->completed_sequence >= target_sequence)
    {
        osal_mutex_unlock(&wq->mtx);
        return osal::ok();
    }

    while (wq->completed_sequence < target_sequence)
    {
        osal::tick_t remaining = osal::WAIT_FOREVER;
        if (timeout != osal::WAIT_FOREVER)
        {
            const osal::tick_t elapsed = osal_clock_ticks() - start;
            if (elapsed >= timeout)
            {
                osal_mutex_unlock(&wq->mtx);
                return osal::error_code::timeout;
            }
            remaining = timeout - elapsed;
        }

        const osal::result r = osal_condvar_wait(&wq->flush_cv, &wq->mtx, remaining);
        if (!r.ok() && r != osal::error_code::timeout)
        {
            osal_mutex_unlock(&wq->mtx);
            return r;
        }
    }

    osal_mutex_unlock(&wq->mtx);
    return osal::ok();
}

/// @brief Discard all pending (not yet started) work items.
/// @details Atomically resets the ring buffer head/tail and drains the counting
///          semaphore so the worker will not wake for the discarded items.
/// @param handle Queue handle.
/// @return `osal::ok()` on success, `error_code::not_initialized` if null.
osal::result osal_work_queue_cancel_all(osal::active_traits::work_queue_handle_t* handle) noexcept
{
    if (!handle || !handle->native) [[unlikely]]
    {
        return osal::error_code::not_initialized;
    }
    auto* wq = static_cast<emulated_wq_obj*>(handle->native);

    osal_mutex_lock(&wq->mtx, osal::WAIT_FOREVER);
    const std::size_t drained = wq->count;
    wq->head                  = 0U;
    wq->tail                  = 0U;
    wq->count                 = 0U;
    osal_mutex_unlock(&wq->mtx);

    for (std::size_t i = 0U; i < drained; ++i)
    {
        osal_semaphore_try_take(&wq->not_empty);
    }

    return osal::ok();
}

std::size_t osal_work_queue_pending(const osal::active_traits::work_queue_handle_t* handle) noexcept
{
    if (!handle || !handle->native) [[unlikely]]
    {
        return 0U;
    }
    auto* wq = static_cast<emulated_wq_obj*>(handle->native);
    osal_mutex_lock(&wq->mtx, osal::WAIT_FOREVER);
    const std::size_t pending = wq->count;
    osal_mutex_unlock(&wq->mtx);
    return pending;
}
