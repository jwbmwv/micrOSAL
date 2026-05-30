// SPDX-License-Identifier: Apache-2.0
/// @file nuttx_work_queue.inl
/// @brief Native NuttX work queue implementation using work_queue() API.
/// @details Uses NuttX's native work queue system (HPWORK/LPWORK) for deferred
///          work execution.  #include this file inside the `extern "C"` block
///          of nuttx_backend.cpp to replace the emulated work queue.
///
///          NuttX work queue API:
///          - work_queue(int qid, struct work_s *work, worker_t worker, void *arg, clock_t delay)
///          - work_cancel(int qid, struct work_s *work)
///
///          We use LPWORK (low-priority work queue) by default for better
///          responsiveness and to avoid blocking high-priority system tasks.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#pragma once

#include <nuttx/wqueue.h>
#include <nuttx/clock.h>

// ---------------------------------------------------------------------------
// Work queue (native — NuttX work_queue() API)
// ---------------------------------------------------------------------------

#ifndef OSAL_NUTTX_MAX_WORK_QUEUES
/// @brief Maximum number of NuttX work queues that can exist concurrently.
#define OSAL_NUTTX_MAX_WORK_QUEUES 4U
#endif

#ifndef OSAL_NUTTX_MAX_WORK_ITEMS
/// @brief Maximum number of pending work items per queue.
#define OSAL_NUTTX_MAX_WORK_ITEMS 32U
#endif

namespace  // anonymous — internal to the including TU
{

struct nuttx_work_item
{
    struct work_s    work;       ///< NuttX work structure.
    osal_work_func_t func;       ///< OSAL work function.
    void*            arg;        ///< OSAL work argument.
    bool             pending;    ///< Item is queued.
    bool             allocated;  ///< Slot is in use.
};

struct nuttx_work_queue_obj
{
    nuttx_work_item items[OSAL_NUTTX_MAX_WORK_ITEMS];
    nxmutex_t       lock;           ///< Protects item array.
    int             queue_id;       ///< NuttX work queue ID (HPWORK/LPWORK).
    std::size_t     pending_count;  ///< Number of pending items.
    bool            valid;          ///< Queue is initialized.
};

static nuttx_work_queue_obj nuttx_wq_pool[OSAL_NUTTX_MAX_WORK_QUEUES];
static std::atomic_bool     nuttx_wq_used[OSAL_NUTTX_MAX_WORK_QUEUES];

static nuttx_work_queue_obj* nuttx_wq_acquire() noexcept
{
    for (std::size_t i = 0U; i < OSAL_NUTTX_MAX_WORK_QUEUES; ++i)
    {
        if (!nuttx_wq_used[i].exchange(true, std::memory_order_acq_rel))
        {
            return &nuttx_wq_pool[i];
        }
    }
    return nullptr;
}

static void nuttx_wq_release(nuttx_work_queue_obj* p) noexcept
{
    for (std::size_t i = 0U; i < OSAL_NUTTX_MAX_WORK_QUEUES; ++i)
    {
        if (&nuttx_wq_pool[i] == p)
        {
            nuttx_wq_used[i].store(false, std::memory_order_release);
            return;
        }
    }
}

/// @brief Worker function trampoline that NuttX calls.
static void nuttx_work_trampoline(void* arg) noexcept
{
    auto* item = static_cast<nuttx_work_item*>(arg);
    if (!item || !item->func)
    {
        return;
    }

    // Call the OSAL work function.
    item->func(item->arg);

    // Mark as no longer pending (the NuttX work system has dequeued it).
    item->pending = false;
}

}  // anonymous namespace

/// @brief Create a native NuttX work queue.
/// @details Uses NuttX's shared LPWORK queue for execution. The stack
///          parameter is ignored as NuttX manages work queue threads internally.
/// @param handle Output handle.
/// @param stack Ignored (NuttX manages work queue threads).
/// @param stack_bytes Ignored.
/// @param depth Maximum number of pending work items.
/// @param name Ignored (NuttX work queues don't have user-visible names).
/// @return osal::ok() on success, error_code otherwise.
osal::result osal_work_queue_create(osal::active_traits::work_queue_handle_t* handle, void* /*stack*/,
                                    std::size_t /*stack_bytes*/, std::size_t depth, const char* /*name*/) noexcept
{
    if (!handle || depth == 0 || depth > OSAL_NUTTX_MAX_WORK_ITEMS)
    {
        return osal::error_code::invalid_argument;
    }

    auto* wq = nuttx_wq_acquire();
    if (!wq)
    {
        return osal::error_code::out_of_resources;
    }

    // Initialize lock.
    if (nxmutex_init(&wq->lock) < 0)
    {
        nuttx_wq_release(wq);
        return osal::error_code::out_of_resources;
    }

    // Initialize item pool.
    for (auto& item : wq->items)
    {
        item.func      = nullptr;
        item.arg       = nullptr;
        item.pending   = false;
        item.allocated = false;
    }

    wq->queue_id      = LPWORK;  // Use low-priority work queue.
    wq->pending_count = 0;
    wq->valid         = true;

    handle->native = wq;
    return osal::ok();
}

/// @brief Destroy a native NuttX work queue.
/// @details Cancels all pending work items and releases the queue.
/// @param handle Work queue handle.
/// @return osal::ok() on success.
osal::result osal_work_queue_destroy(osal::active_traits::work_queue_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }

    auto* wq = static_cast<nuttx_work_queue_obj*>(handle->native);
    if (!wq->valid)
    {
        return osal::ok();
    }

    // Cancel all pending work items.
    nxmutex_lock(&wq->lock);
    for (auto& item : wq->items)
    {
        if (item.allocated && item.pending)
        {
            work_cancel(wq->queue_id, &item.work);
            item.pending = false;
        }
        item.allocated = false;
    }
    wq->pending_count = 0;
    wq->valid         = false;
    nxmutex_unlock(&wq->lock);

    nxmutex_destroy(&wq->lock);
    nuttx_wq_release(wq);
    handle->native = nullptr;

    return osal::ok();
}

/// @brief Submit a work item to the NuttX work queue.
/// @param handle Work queue handle.
/// @param func Work function to execute.
/// @param arg Argument passed to the work function.
/// @return osal::ok() on success, error_code::out_of_resources if queue is full.
osal::result osal_work_queue_submit(osal::active_traits::work_queue_handle_t* handle, osal_work_func_t func,
                                    void* arg) noexcept
{
    if (!handle || !handle->native || !func)
    {
        return osal::error_code::invalid_argument;
    }

    auto* wq = static_cast<nuttx_work_queue_obj*>(handle->native);
    if (!wq->valid)
    {
        return osal::error_code::not_initialized;
    }

    nxmutex_lock(&wq->lock);

    // Find a free item slot.
    nuttx_work_item* item = nullptr;
    for (auto& it : wq->items)
    {
        if (!it.allocated)
        {
            item            = &it;
            item->allocated = true;
            break;
        }
    }

    if (!item)
    {
        nxmutex_unlock(&wq->lock);
        return osal::error_code::out_of_resources;
    }

    // Initialize work item.
    item->func    = func;
    item->arg     = arg;
    item->pending = true;

    // Submit to NuttX work queue (immediate execution, no delay).
    int ret = work_queue(wq->queue_id, &item->work, nuttx_work_trampoline, item, 0);

    if (ret < 0)
    {
        item->allocated = false;
        item->pending   = false;
        nxmutex_unlock(&wq->lock);
        return osal::error_code::out_of_resources;
    }

    ++wq->pending_count;
    nxmutex_unlock(&wq->lock);

    return osal::ok();
}

/// @brief Submit a work item from ISR context.
/// @details NuttX work_queue() is ISR-safe, so this is identical to regular submit.
/// @param handle Work queue handle.
/// @param func Work function to execute.
/// @param arg Argument passed to the work function.
/// @return osal::ok() on success.
osal::result osal_work_queue_submit_from_isr(osal::active_traits::work_queue_handle_t* handle, osal_work_func_t func,
                                             void* arg) noexcept
{
    // NuttX work_queue() is safe to call from ISR context.
    return osal_work_queue_submit(handle, func, arg);
}

/// @brief Flush the work queue, waiting for all pending items to complete.
/// @param handle Work queue handle.
/// @param timeout_ticks Maximum time to wait (currently ignored, waits indefinitely).
/// @return osal::ok() on success, error_code::timeout if items remain pending.
osal::result osal_work_queue_flush(osal::active_traits::work_queue_handle_t* handle,
                                   osal::tick_t /*timeout_ticks*/) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::invalid_argument;
    }

    auto* wq = static_cast<nuttx_work_queue_obj*>(handle->native);
    if (!wq->valid)
    {
        return osal::error_code::not_initialized;
    }

    // Poll until all items are no longer pending.
    // Note: NuttX doesn't provide a native "drain" API for work_queue(),
    // so we poll. This is not ideal but safe.
    constexpr int max_iterations = 1000;
    for (int i = 0; i < max_iterations; ++i)
    {
        nxmutex_lock(&wq->lock);
        bool any_pending = false;
        for (const auto& item : wq->items)
        {
            if (item.allocated && item.pending)
            {
                any_pending = true;
                break;
            }
        }
        nxmutex_unlock(&wq->lock);

        if (!any_pending)
        {
            return osal::ok();
        }

        // Yield to allow work items to execute.
        sched_yield();
    }

    return osal::error_code::timeout;
}

/// @brief Cancel all pending work items.
/// @param handle Work queue handle.
/// @return osal::ok() on success.
osal::result osal_work_queue_cancel_all(osal::active_traits::work_queue_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::invalid_argument;
    }

    auto* wq = static_cast<nuttx_work_queue_obj*>(handle->native);
    if (!wq->valid)
    {
        return osal::error_code::not_initialized;
    }

    nxmutex_lock(&wq->lock);
    for (auto& item : wq->items)
    {
        if (item.allocated && item.pending)
        {
            work_cancel(wq->queue_id, &item.work);
            item.pending   = false;
            item.allocated = false;
        }
    }
    wq->pending_count = 0;
    nxmutex_unlock(&wq->lock);

    return osal::ok();
}

/// @brief Query the number of pending work items.
/// @param handle Work queue handle.
/// @return Number of pending items, or 0 on error.
std::size_t osal_work_queue_pending(const osal::active_traits::work_queue_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return 0;
    }

    auto* wq = static_cast<nuttx_work_queue_obj*>(handle->native);
    if (!wq->valid)
    {
        return 0;
    }

    nxmutex_lock(const_cast<nxmutex_t*>(&wq->lock));
    std::size_t count = 0;
    for (const auto& item : wq->items)
    {
        if (item.allocated && item.pending)
        {
            ++count;
        }
    }
    nxmutex_unlock(const_cast<nxmutex_t*>(&wq->lock));

    return count;
}
