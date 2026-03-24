// SPDX-License-Identifier: Apache-2.0
/// @file posix_pthread_work_queue.inl
/// @brief Shared pthread-based work-queue implementation for POSIX-family backends.
/// @details Implements osal_work_queue_* using a dedicated pthread worker thread,
///          a ring buffer, and a pthread_mutex_t + pthread_cond_t.  Include inside
///          the extern "C" block of POSIX and Linux backends.
///
///          NuttX and QNX use ../emulated_work_queue.inl which builds on OSAL's
///          own mutex/semaphore/thread primitives instead.
///
///          Required definitions before including this file:
///          1. `cond_init_monotonic(pthread_cond_t*)` — initialises a condvar
///             with CLOCK_MONOTONIC (or fallback).
///          2. `OSAL_POSIX_COND_ABS(t)` — absolute timespec for condvar timed
///             waits on the same clock used by cond_init_monotonic.
///
///          Prerequisites: <pthread.h>, <new>, <cstring> included, extern "C"
///                         scope active, cond_init_monotonic and
///                         OSAL_POSIX_COND_ABS defined.

#pragma once

// -------------------------------------------------------------------------
// Work queue (emulated — dedicated pthread + ring buffer)
// -------------------------------------------------------------------------

struct posix_wq_obj
{
    osal::work_item* ring;
    std::size_t      capacity;
    std::size_t      head;
    std::size_t      tail;
    std::size_t      count;

    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;  ///< signalled when an item is enqueued
    pthread_cond_t  flushed;    ///< signalled when a flush sentinel executes
    pthread_t       worker;

    volatile bool stop;
    volatile bool flush_done;
};

static void* posix_wq_worker(void* arg) noexcept
{
    auto* wq = static_cast<posix_wq_obj*>(arg);
    while (true)
    {
        pthread_mutex_lock(&wq->mutex);
        while (wq->count == 0U && !wq->stop)
        {
            pthread_cond_wait(&wq->not_empty, &wq->mutex);
        }
        if (wq->stop && wq->count == 0U)
        {
            pthread_mutex_unlock(&wq->mutex);
            break;
        }
        osal::work_item item = wq->ring[wq->head];
        wq->head             = (wq->head + 1U) % wq->capacity;
        --wq->count;
        pthread_mutex_unlock(&wq->mutex);

        if (item.func != nullptr)
        {
            item.func(item.arg);
        }
        else
        {
            // Sentinel item — signal flush completion.
            pthread_mutex_lock(&wq->mutex);
            wq->flush_done = true;
            pthread_cond_signal(&wq->flushed);
            pthread_mutex_unlock(&wq->mutex);
        }
    }
    return nullptr;
}

// -------------------------------------------------------------------------
// Work queue (emulated — dedicated pthread + ring buffer)
// -------------------------------------------------------------------------

/// @brief Create a POSIX pthread-based work queue.
/// @details Spawns a dedicated `pthread` worker that dequeues and executes submitted
///          items in FIFO order using a heap-allocated ring buffer.  @p stack is
///          ignored (pthreads manage their own stacks).
/// @param handle      Output handle; populated on success.
/// @param stack       Ignored (POSIX pthreads manage their own stack).
/// @param stack_bytes Ignored.
/// @param depth       Maximum number of pending work items.
/// @param name        Ignored (pthread naming requires optional OS support).
/// @return `osal::ok()` on success, `error_code::invalid_argument` if null,
///         `error_code::out_of_resources` on allocation or thread creation failure.
osal::result osal_work_queue_create(osal::active_traits::work_queue_handle_t* handle, void* /*stack*/,
                                    std::size_t /*stack_bytes*/, std::size_t depth, const char* /*name*/) noexcept
{
    if (!handle)
    {
        return osal::error_code::invalid_argument;
    }
    auto* wq = new (std::nothrow) posix_wq_obj{};
    if (!wq)
    {
        return osal::error_code::out_of_resources;
    }
    wq->ring = new (std::nothrow) osal::work_item[depth];
    if (!wq->ring)
    {
        delete wq;
        return osal::error_code::out_of_resources;
    }
    wq->capacity   = depth;
    wq->head       = 0U;
    wq->tail       = 0U;
    wq->count      = 0U;
    wq->stop       = false;
    wq->flush_done = false;

    pthread_mutex_init(&wq->mutex, nullptr);
    cond_init_monotonic(&wq->not_empty);
    cond_init_monotonic(&wq->flushed);

    const int rc = pthread_create(&wq->worker, nullptr, posix_wq_worker, wq);
    if (rc != 0)
    {
        pthread_cond_destroy(&wq->flushed);
        pthread_cond_destroy(&wq->not_empty);
        pthread_mutex_destroy(&wq->mutex);
        delete[] wq->ring;
        delete wq;
        return osal::error_code::out_of_resources;
    }
    handle->native = static_cast<void*>(wq);
    return osal::ok();
}

/// @brief Destroy a POSIX work queue, stopping and joining the worker thread.
/// @param handle Handle to destroy; silently ignored if null.
/// @return Always `osal::ok()`.
osal::result osal_work_queue_destroy(osal::active_traits::work_queue_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }
    auto* wq = static_cast<posix_wq_obj*>(handle->native);
    pthread_mutex_lock(&wq->mutex);
    wq->stop = true;
    pthread_cond_signal(&wq->not_empty);
    pthread_mutex_unlock(&wq->mutex);

    pthread_join(wq->worker, nullptr);
    pthread_cond_destroy(&wq->flushed);
    pthread_cond_destroy(&wq->not_empty);
    pthread_mutex_destroy(&wq->mutex);
    delete[] wq->ring;
    delete wq;
    handle->native = nullptr;
    return osal::ok();
}

/// @brief Submit a work item to the queue (task context).
/// @param handle Queue handle.
/// @param func   Work function to call; must not be null.
/// @param arg    Opaque argument passed to @p func.
/// @return `osal::ok()` on success, `error_code::invalid_argument` if @p func is null,
///         `error_code::overflow` if the ring is full,
///         `error_code::not_initialized` if @p handle is null.
osal::result osal_work_queue_submit(osal::active_traits::work_queue_handle_t* handle, osal_work_func_t func,
                                    void* arg) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    if (!func)
    {
        return osal::error_code::invalid_argument;
    }
    auto* wq = static_cast<posix_wq_obj*>(handle->native);
    pthread_mutex_lock(&wq->mutex);
    if (wq->count >= wq->capacity)
    {
        pthread_mutex_unlock(&wq->mutex);
        return osal::error_code::overflow;
    }
    wq->ring[wq->tail] = osal::work_item{func, arg};
    wq->tail           = (wq->tail + 1U) % wq->capacity;
    ++wq->count;
    pthread_cond_signal(&wq->not_empty);
    pthread_mutex_unlock(&wq->mutex);
    return osal::ok();
}

/// @brief Submit a work item from ISR context — not supported.
/// @return Always `error_code::not_supported`.
osal::result osal_work_queue_submit_from_isr(osal::active_traits::work_queue_handle_t* /*handle*/,
                                             osal_work_func_t /*func*/, void* /*arg*/) noexcept
{
    return osal::error_code::not_supported;
}

/// @brief Wait until all submitted items have been executed.
/// @details Enqueues a sentinel item and uses `pthread_cond_timedwait` on a flush
///          condition variable to wait for the worker to process it.
/// @param handle  Queue handle.
/// @param timeout Maximum ticks to wait; use `osal::WAIT_FOREVER` for indefinite.
/// @return `osal::ok()` when flushed, `error_code::timeout` on expiry,
///         `error_code::overflow` if the ring is full, `error_code::not_initialized` if null.
osal::result osal_work_queue_flush(osal::active_traits::work_queue_handle_t* handle, osal::tick_t timeout) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* wq = static_cast<posix_wq_obj*>(handle->native);

    pthread_mutex_lock(&wq->mutex);
    if (wq->count >= wq->capacity)
    {
        pthread_mutex_unlock(&wq->mutex);
        return osal::error_code::overflow;
    }
    wq->flush_done     = false;
    wq->ring[wq->tail] = osal::work_item{nullptr, nullptr};
    wq->tail           = (wq->tail + 1U) % wq->capacity;
    ++wq->count;
    pthread_cond_signal(&wq->not_empty);

    if (timeout == osal::WAIT_FOREVER)
    {
        while (!wq->flush_done)
        {
            pthread_cond_wait(&wq->flushed, &wq->mutex);
        }
    }
    else
    {
        const struct timespec ts = OSAL_POSIX_COND_ABS(timeout);
        while (!wq->flush_done)
        {
            const int rc = pthread_cond_timedwait(&wq->flushed, &wq->mutex, &ts);
            if (rc != 0 && !wq->flush_done)
            {
                pthread_mutex_unlock(&wq->mutex);
                return osal::error_code::timeout;
            }
        }
    }
    pthread_mutex_unlock(&wq->mutex);
    return osal::ok();
}

/// @brief Discard all pending (not yet started) work items.
/// @param handle Queue handle.
/// @return `osal::ok()` on success, `error_code::not_initialized` if null.
osal::result osal_work_queue_cancel_all(osal::active_traits::work_queue_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* wq = static_cast<posix_wq_obj*>(handle->native);
    pthread_mutex_lock(&wq->mutex);
    wq->head  = 0U;
    wq->tail  = 0U;
    wq->count = 0U;
    pthread_mutex_unlock(&wq->mutex);
    return osal::ok();
}

/// @brief Return the number of pending (not yet executed) work items.
/// @param handle Queue handle (const).
/// @return Item count, or 0 if @p handle is null.
std::size_t osal_work_queue_pending(const osal::active_traits::work_queue_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return 0U;
    }
    return static_cast<const posix_wq_obj*>(handle->native)->count;
}
