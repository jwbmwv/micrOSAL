// SPDX-License-Identifier: Apache-2.0
/// @file micrium_backend.cpp
/// @brief Micrium µC/OS-III implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the Micrium µC/OS-III API:
///          - thread   → OSTaskCreate / OSTaskDel
///          - mutex    → OSMutexCreate / OSMutexPend / OSMutexPost
///          - semaphore→ OSSemCreate / OSSemPend / OSSemPost
///          - queue    → OSQCreate / OSQPend / OSQPost
///          - timer    → OSTmrCreate / OSTmrStart / OSTmrStop
///          - event_flags → OSFlagCreate / OSFlagPend / OSFlagPost
///
///          All kernel objects are held in static pools.
///          Dynamic allocation is never used.
///
///          Includes required: <os.h> (µC/OS-III master header)
///          Build macro:       OSAL_BACKEND_MICRIUM
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_MICRIUM
#define OSAL_BACKEND_MICRIUM
#endif
#include <osal/osal.hpp>

#include <os.h>  ///< µC/OS-III master header
#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Pool sizes
// ---------------------------------------------------------------------------
#define OSAL_UC_MAX_THREADS 8
#define OSAL_UC_MAX_MUTEXES 16
#define OSAL_UC_MAX_SEMS 16
#define OSAL_UC_MAX_QUEUES 8
#define OSAL_UC_MAX_TIMERS 8
#define OSAL_UC_MAX_FLAGS 8

// ---------------------------------------------------------------------------
// Static object pools
// ---------------------------------------------------------------------------
static OS_TCB      uc_tcbs[OSAL_UC_MAX_THREADS];
static bool        uc_tcb_used[OSAL_UC_MAX_THREADS];
static OS_MUTEX    uc_mutexes[OSAL_UC_MAX_MUTEXES];
static bool        uc_mutex_used[OSAL_UC_MAX_MUTEXES];
static OS_SEM      uc_sems[OSAL_UC_MAX_SEMS];
static bool        uc_sem_used[OSAL_UC_MAX_SEMS];
static OS_Q        uc_queues[OSAL_UC_MAX_QUEUES];
static bool        uc_queue_used[OSAL_UC_MAX_QUEUES];
static OS_TMR      uc_timers[OSAL_UC_MAX_TIMERS];
static bool        uc_timer_used[OSAL_UC_MAX_TIMERS];
static OS_FLAG_GRP uc_flags[OSAL_UC_MAX_FLAGS];
static bool        uc_flag_used[OSAL_UC_MAX_FLAGS];

template<typename T, std::size_t N>
static T* pool_acquire(T (&pool)[N], bool (&used)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!used[i])
        {
            used[i] = true;
            return &pool[i];
        }
    }
    return nullptr;
}

template<typename T, std::size_t N>
static void pool_release(T (&pool)[N], bool (&used)[N], T* p) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (&pool[i] == p)
        {
            used[i] = false;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Map OSAL priority [0=lowest, 255=highest] to µC/OS-III [OS_CFG_PRIO_MAX-2=lowest, 1=highest].
static constexpr OS_PRIO osal_to_uc_priority(osal::priority_t p) noexcept
{
    const OS_PRIO max_prio = OS_CFG_PRIO_MAX - 2U;  // 0 is reserved for ISR, max-1 for idle
    return max_prio - static_cast<OS_PRIO>((static_cast<std::uint32_t>(p) * static_cast<std::uint32_t>(max_prio)) /
                                           static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
}

/// @brief Convert OSAL tick count to µC/OS-III OS_TICK.
static constexpr OS_TICK to_uc_ticks(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return 0U;  // µC/OS-III: timeout=0 means wait forever
    }
    if (t == osal::NO_WAIT)
    {
        return 1U;  // Minimum non-zero; checked separately with OS_OPT_PEND_NON_BLOCKING
    }
    return static_cast<OS_TICK>(t);
}

/// @brief Return µC/OS-III pend option for given timeout.
static constexpr OS_OPT uc_pend_opt(osal::tick_t t) noexcept
{
    return (t == osal::NO_WAIT) ? OS_OPT_PEND_NON_BLOCKING : OS_OPT_PEND_BLOCKING;
}

// ---------------------------------------------------------------------------
// Timer callback context
// ---------------------------------------------------------------------------
struct uc_timer_ctx
{
    osal_timer_callback_t fn;
    void*                 arg;
    OS_TMR*               tmr;
};
static uc_timer_ctx uc_timer_ctxs[OSAL_UC_MAX_TIMERS];

static void uc_tmr_callback(void* /*p_tmr*/, void* p_arg) noexcept
{
    auto* ctx = static_cast<uc_timer_ctx*>(p_arg);
    if (ctx != nullptr && ctx->fn != nullptr)
    {
        ctx->fn(ctx->arg);
    }
}

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    /// @brief Return monotonic time in milliseconds via OSTimeGet().
    /// @return Milliseconds elapsed since scheduler start.
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        OS_ERR        err;
        const OS_TICK ticks = OSTimeGet(&err);
        return static_cast<std::int64_t>(ticks) * (1000 / OSCfg_TickRate_Hz);
    }

    /// @brief Return wall-clock time in milliseconds (aliased to monotonic; µC/OS-III has no wall clock).
    /// @return Milliseconds since scheduler start.
    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();  // µC/OS-III has no wall clock
    }

    /// @brief Return the raw µC/OS-III tick counter.
    /// @return Current OS_TICK value from OSTimeGet().
    osal::tick_t osal_clock_ticks() noexcept
    {
        OS_ERR err;
        return static_cast<osal::tick_t>(OSTimeGet(&err));
    }

    /// @brief Return the configured tick period in microseconds.
    /// @return Microseconds per tick (1 000 000 / OSCfg_TickRate_Hz).
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return static_cast<std::uint32_t>(1'000'000U / OSCfg_TickRate_Hz);
    }

    // ---------------------------------------------------------------------------
    // Thread (Task)
    // ---------------------------------------------------------------------------

    /// @brief Create and immediately start a µC/OS-III task.
    /// @param handle      Output handle populated with the OS_TCB pointer.
    /// @param entry       Task entry function.
    /// @param arg         Opaque argument forwarded to @p entry.
    /// @param priority    OSAL priority [0=lowest, 255=highest]; mapped to µC/OS-III priority.
    /// @param stack       Caller-supplied stack buffer.
    /// @param stack_bytes Size of @p stack in bytes.
    /// @param name        Human-readable task name (may be nullptr).
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        assert(handle != nullptr && entry != nullptr && stack != nullptr);
        OS_TCB* tcb = pool_acquire(uc_tcbs, uc_tcb_used);
        if (tcb == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        OS_ERR             err;
        const CPU_STK_SIZE stk_size = static_cast<CPU_STK_SIZE>(stack_bytes / sizeof(CPU_STK));

        OSTaskCreate(tcb, const_cast<CPU_CHAR*>(name != nullptr ? name : "osal"), reinterpret_cast<OS_TASK_PTR>(entry),
                     arg, osal_to_uc_priority(priority),
                     static_cast<CPU_STK*>(stack),  // stack base
                     stk_size / 10U,                // stack limit (watermark)
                     stk_size,                      // stack size
                     0U,                            // q size (not used)
                     0U,                            // time quanta
                     nullptr,                       // p_ext
                     static_cast<OS_OPT>(OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR), &err);

        if (err != OS_ERR_NONE)
        {
            pool_release(uc_tcbs, uc_tcb_used, tcb);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(tcb);
        return osal::ok();
    }

    /// @brief Delete a task and release its TCB pool slot.
    /// @param handle Task handle to join.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        auto*  tcb = static_cast<OS_TCB*>(handle->native);
        OSTaskDel(tcb, &err);
        pool_release(uc_tcbs, uc_tcb_used, tcb);
        handle->native = nullptr;
        return (err == OS_ERR_NONE || err == OS_ERR_TASK_NOT_EXIST) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Detach a task, releasing its TCB pool slot without waiting.
    /// @param handle Task handle to detach.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        pool_release(uc_tcbs, uc_tcb_used, static_cast<OS_TCB*>(handle->native));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Change the priority of a running task via OSTaskChangePrio().
    /// @param handle   Task handle.
    /// @param priority New OSAL priority; mapped to µC/OS-III range.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        OSTaskChangePrio(static_cast<OS_TCB*>(handle->native), osal_to_uc_priority(priority), &err);
        return (err == OS_ERR_NONE) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Set thread CPU affinity (not supported on µC/OS-III).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Suspend a task (not supported on µC/OS-III through this OSAL).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended task (not supported on µC/OS-III through this OSAL).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Yield the current task's time slice via OSTimeDly(0).
    void osal_thread_yield() noexcept
    {
        OS_ERR err;
        OSTimeDly(0U, OS_OPT_TIME_DLY, &err);
    }

    /// @brief Sleep for at least @p ms milliseconds via OSTimeDlyHMSM().
    /// @param ms Delay in milliseconds.
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        OS_ERR err;
        OSTimeDlyHMSM(0U, 0U, static_cast<CPU_INT32U>(ms / 1000U), static_cast<CPU_INT32U>(ms % 1000U),
                      OS_OPT_TIME_HMSM_STRICT, &err);
    }

    // ---------------------------------------------------------------------------
    // Mutex (OS_MUTEX — recursive, priority inheritance)
    // ---------------------------------------------------------------------------

    /// @brief Create a µC/OS-III mutex with priority inheritance.
    /// @param handle Output handle populated with the new OS_MUTEX pointer.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        OS_MUTEX* m = pool_acquire(uc_mutexes, uc_mutex_used);
        if (m == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_ERR err;
        OSMutexCreate(m, const_cast<CPU_CHAR*>("m"), &err);
        if (err != OS_ERR_NONE)
        {
            pool_release(uc_mutexes, uc_mutex_used, m);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(m);
        return osal::ok();
    }

    /// @brief Destroy a mutex via OSMutexDel() and release its pool slot.
    /// @param handle Mutex handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        OS_ERR err;
        auto*  m = static_cast<OS_MUTEX*>(handle->native);
        OSMutexDel(m, OS_OPT_DEL_ALWAYS, &err);
        pool_release(uc_mutexes, uc_mutex_used, m);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquire a mutex, blocking up to @p timeout_ticks ticks.
    /// @param handle        Mutex handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown on failure.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        CPU_TS ts;
        OSMutexPend(static_cast<OS_MUTEX*>(handle->native), to_uc_ticks(timeout_ticks), uc_pend_opt(timeout_ticks), &ts,
                    &err);
        if (err == OS_ERR_NONE)
        {
            return osal::ok();
        }
        if (err == OS_ERR_TIMEOUT || err == OS_ERR_PEND_WOULD_BLOCK)
        {
            return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
        }
        return osal::error_code::unknown;
    }

    /// @brief Try to acquire a mutex without blocking.
    /// @param handle Mutex handle.
    /// @return osal::ok() if acquired; osal::error_code::would_block if not immediately available.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release a previously acquired mutex via OSMutexPost().
    /// @param handle Mutex handle.
    /// @return osal::ok() on success; osal::error_code::not_owner if the caller does not own the mutex.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        OSMutexPost(static_cast<OS_MUTEX*>(handle->native), OS_OPT_POST_NONE, &err);
        return (err == OS_ERR_NONE) ? osal::ok() : osal::error_code::not_owner;
    }

    // ---------------------------------------------------------------------------
    // Semaphore (OS_SEM)
    // ---------------------------------------------------------------------------

    /// @brief Create a counting semaphore via OSSemCreate().
    /// @param handle        Output handle populated with the new OS_SEM pointer.
    /// @param initial_count Initial count.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned /*max_count*/) noexcept
    {
        OS_SEM* s = pool_acquire(uc_sems, uc_sem_used);
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_ERR err;
        OSSemCreate(s, const_cast<CPU_CHAR*>("s"), static_cast<OS_SEM_CTR>(initial_count), &err);
        if (err != OS_ERR_NONE)
        {
            pool_release(uc_sems, uc_sem_used, s);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(s);
        return osal::ok();
    }

    /// @brief Destroy a semaphore via OSSemDel() and release its pool slot.
    /// @param handle Semaphore handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        OS_ERR err;
        auto*  s = static_cast<OS_SEM*>(handle->native);
        OSSemDel(s, OS_OPT_DEL_ALWAYS, &err);
        pool_release(uc_sems, uc_sem_used, s);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment (post) a semaphore via OSSemPost().
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::overflow if the counter would overflow.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        OSSemPost(static_cast<OS_SEM*>(handle->native), OS_OPT_POST_1, &err);
        return (err == OS_ERR_NONE) ? osal::ok() : osal::error_code::overflow;
    }

    /// @brief Increment a semaphore from an ISR context (OSSemPost is ISR-safe).
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::overflow if the counter would overflow.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // OSSemPost is ISR-safe
    }

    /// @brief Decrement (wait on) a semaphore via OSSemPend(), blocking up to @p timeout_ticks ticks.
    /// @param handle        Semaphore handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown on failure.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        CPU_TS ts;
        OSSemPend(static_cast<OS_SEM*>(handle->native), to_uc_ticks(timeout_ticks), uc_pend_opt(timeout_ticks), &ts,
                  &err);
        if (err == OS_ERR_NONE)
        {
            return osal::ok();
        }
        if (err == OS_ERR_TIMEOUT || err == OS_ERR_PEND_WOULD_BLOCK)
        {
            return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
        }
        return osal::error_code::unknown;
    }

    /// @brief Try to decrement a semaphore without blocking.
    /// @param handle Semaphore handle.
    /// @return osal::ok() if decremented; osal::error_code::would_block if count was zero.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (OS_Q — µC/OS-III native message queue)
    // ---------------------------------------------------------------------------

    /// @brief Create a µC/OS-III native message queue via OSQCreate().
    /// @param handle   Output handle populated with the new OS_Q pointer.
    /// @param capacity Maximum number of messages the queue can hold.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/,
                                   std::size_t /*item_size*/, std::size_t capacity) noexcept
    {
        OS_Q* q = pool_acquire(uc_queues, uc_queue_used);
        if (q == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_ERR err;
        OSQCreate(q, const_cast<CPU_CHAR*>("q"), static_cast<OS_MSG_QTY>(capacity), &err);
        if (err != OS_ERR_NONE)
        {
            pool_release(uc_queues, uc_queue_used, q);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(q);
        return osal::ok();
    }

    /// @brief Destroy a queue via OSQDel() and release its pool slot.
    /// @param handle Queue handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        OS_ERR err;
        auto*  q = static_cast<OS_Q*>(handle->native);
        OSQDel(q, OS_OPT_DEL_ALWAYS, &err);
        pool_release(uc_queues, uc_queue_used, q);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Send a message to a queue via OSQPost().
    /// @details µC/OS-III sends pointer-sized messages; @p item is treated as a pointer.
    /// @param handle        Queue handle.
    /// @param item          Pointer to the message data.
    /// @param timeout_ticks Maximum wait in OSAL ticks (currently non-blocking on post).
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        // µC/OS-III queue sends a pointer + size.  We treat item as a pointer-sized message.
        OSQPost(static_cast<OS_Q*>(handle->native), const_cast<void*>(item), sizeof(void*),
                OS_OPT_POST_FIFO | ((timeout_ticks == osal::NO_WAIT) ? static_cast<OS_OPT>(0) : static_cast<OS_OPT>(0)),
                &err);
        if (err == OS_ERR_NONE)
        {
            return osal::ok();
        }
        return (err == OS_ERR_Q_MAX) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Send a message from an ISR context (delegates to osal_queue_send with NO_WAIT).
    /// @param handle Queue handle.
    /// @param item   Pointer to the message data.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Receive a message from a queue via OSQPend(), blocking up to @p timeout_ticks ticks.
    /// @param handle        Queue handle.
    /// @param item          Buffer to receive the dequeued message into.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown on failure.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR      err;
        OS_MSG_SIZE msg_size;
        CPU_TS      ts;
        void* msg = OSQPend(static_cast<OS_Q*>(handle->native), to_uc_ticks(timeout_ticks), uc_pend_opt(timeout_ticks),
                            &msg_size, &ts, &err);
        if (err == OS_ERR_NONE && msg != nullptr)
        {
            std::memcpy(item, &msg, sizeof(void*));
            return osal::ok();
        }
        if (err == OS_ERR_TIMEOUT || err == OS_ERR_PEND_WOULD_BLOCK)
        {
            return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
        }
        return osal::error_code::unknown;
    }

    /// @brief Receive a message from an ISR context (delegates to osal_queue_receive with NO_WAIT).
    /// @param handle Queue handle.
    /// @param item   Buffer to receive the dequeued message into.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Peek at the front message without removing it (not supported on µC/OS-III).
    /// @return osal::error_code::not_supported always.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* /*handle*/, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Return the number of messages currently in the queue.
    /// @param handle Queue handle.
    /// @return Message count, or 0 if the handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* q = static_cast<const OS_Q*>(handle->native);
        return static_cast<std::size_t>(q->MsgQ.NbrEntries);
    }

    /// @brief Return the number of free slots remaining in the queue.
    /// @param handle Queue handle.
    /// @return Available slots, or 0 if the handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* q = static_cast<const OS_Q*>(handle->native);
        return static_cast<std::size_t>(q->MsgQ.NbrEntriesSize - q->MsgQ.NbrEntries);
    }

    // ---------------------------------------------------------------------------
    // Timer (OS_TMR)
    // ---------------------------------------------------------------------------

    /// @brief Create (but do not start) a µC/OS-III software timer.
    /// @param handle       Output handle populated with the OS_TMR pointer.
    /// @param name         Human-readable timer name (may be nullptr).
    /// @param callback     Callback invoked on each expiry.
    /// @param arg          Opaque argument forwarded to @p callback.
    /// @param period_ticks Expiry period in OSAL ticks.
    /// @param auto_reload  True for periodic; false for one-shot.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr && callback != nullptr);
        OS_TMR* t = pool_acquire(uc_timers, uc_timer_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        // Find a context slot.
        uc_timer_ctx* ctx = nullptr;
        for (auto& slot : uc_timer_ctxs)
        {
            if (slot.fn == nullptr)
            {
                ctx = &slot;
                break;
            }
        }
        if (ctx == nullptr)
        {
            pool_release(uc_timers, uc_timer_used, t);
            return osal::error_code::out_of_resources;
        }
        ctx->fn  = callback;
        ctx->arg = arg;
        ctx->tmr = t;

        OS_ERR err;
        // µC/OS-III timer ticks are in units of 1/OSTmrTickRate (typically 10 Hz).
        const OS_TICK tmr_ticks = static_cast<OS_TICK>(period_ticks);
        const OS_OPT  opt       = auto_reload ? OS_OPT_TMR_PERIODIC : OS_OPT_TMR_ONE_SHOT;

        OSTmrCreate(t, const_cast<CPU_CHAR*>(name != nullptr ? name : "t"),
                    auto_reload ? 0U : tmr_ticks,  // dly (one-shot: initial delay)
                    auto_reload ? tmr_ticks : 0U,  // period (periodic: repeat interval)
                    opt, reinterpret_cast<OS_TMR_CALLBACK_PTR>(uc_tmr_callback), static_cast<void*>(ctx), &err);

        if (err != OS_ERR_NONE)
        {
            ctx->fn = nullptr;
            pool_release(uc_timers, uc_timer_used, t);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(t);
        return osal::ok();
    }

    /// @brief Destroy a timer via OSTmrDel() and release its pool slot.
    /// @param handle Timer handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        OS_ERR err;
        auto*  t = static_cast<OS_TMR*>(handle->native);
        OSTmrDel(t, &err);
        for (auto& ctx : uc_timer_ctxs)
        {
            if (ctx.tmr == t)
            {
                ctx.fn  = nullptr;
                ctx.tmr = nullptr;
                break;
            }
        }
        pool_release(uc_timers, uc_timer_used, t);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Activate (start) a timer via OSTmrStart().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        OSTmrStart(static_cast<OS_TMR*>(handle->native), &err);
        return (err == OS_ERR_NONE) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Deactivate (stop) a timer via OSTmrStop().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        OSTmrStop(static_cast<OS_TMR*>(handle->native), OS_OPT_TMR_NONE, nullptr, &err);
        return (err == OS_ERR_NONE || err == OS_ERR_TMR_STOPPED) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Stop and immediately restart a timer.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; forwarded error from osal_timer_start().
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Change the period of a timer by directly updating the OS_TMR struct fields.
    /// @param handle          Timer handle.
    /// @param new_period_ticks New period in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* t   = static_cast<OS_TMR*>(handle->native);
        t->Period = static_cast<OS_TICK>(new_period_ticks);
        t->Dly    = static_cast<OS_TICK>(new_period_ticks);
        return osal::ok();
    }

    /// @brief Query whether a timer is currently active.
    /// @param handle Timer handle.
    /// @return True if the timer state is OS_TMR_STATE_RUNNING; false otherwise.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        return static_cast<const OS_TMR*>(handle->native)->State == OS_TMR_STATE_RUNNING;
    }

    // ---------------------------------------------------------------------------
    // Event flags (OS_FLAG_GRP — native µC/OS-III event flag group)
    // ---------------------------------------------------------------------------

    /// @brief Create a native µC/OS-III event flag group via OSFlagCreate().
    /// @param handle Output handle populated with the new OS_FLAG_GRP pointer.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        OS_FLAG_GRP* f = pool_acquire(uc_flags, uc_flag_used);
        if (f == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_ERR err;
        OSFlagCreate(f, const_cast<CPU_CHAR*>("ef"), 0U, &err);
        if (err != OS_ERR_NONE)
        {
            pool_release(uc_flags, uc_flag_used, f);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(f);
        return osal::ok();
    }

    /// @brief Destroy an event flag group via OSFlagDel() and release its pool slot.
    /// @param handle Event flags handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        OS_ERR err;
        auto*  f = static_cast<OS_FLAG_GRP*>(handle->native);
        OSFlagDel(f, OS_OPT_DEL_ALWAYS, &err);
        pool_release(uc_flags, uc_flag_used, f);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Set (OR) one or more event bits via OSFlagPost() with OS_OPT_POST_FLAG_SET.
    /// @param handle Event flags handle.
    /// @param bits   Bit mask of flags to set.
    /// @return osal::ok() on success; osal::error_code::unknown on OS failure.
    osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        OSFlagPost(static_cast<OS_FLAG_GRP*>(handle->native), static_cast<OS_FLAGS>(bits), OS_OPT_POST_FLAG_SET, &err);
        return (err == OS_ERR_NONE) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Clear event bits via OSFlagPost() with OS_OPT_POST_FLAG_CLR.
    /// @param handle Event flags handle.
    /// @param bits   Bit mask of flags to clear.
    /// @return osal::ok() on success; osal::error_code::unknown on OS failure.
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        OSFlagPost(static_cast<OS_FLAG_GRP*>(handle->native), static_cast<OS_FLAGS>(bits), OS_OPT_POST_FLAG_CLR, &err);
        return (err == OS_ERR_NONE) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Read the current event flags without waiting or clearing.
    /// @param handle Event flags handle.
    /// @return Current OS_FLAGS bit mask; 0 if the handle is invalid.
    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<osal::event_bits_t>(static_cast<const OS_FLAG_GRP*>(handle->native)->Flags);
    }

    /// @brief Internal helper: wait for event flags with configurable AND/OR and clear-on-exit semantics.
    /// @param handle       Event flags handle.
    /// @param wait_bits    Bit mask to wait for.
    /// @param actual_bits  Optional output: bits that were set at wakeup.
    /// @param clear_on_exit Clear matched bits after wakeup when true.
    /// @param all          True = wait for ALL bits; false = wait for ANY bit.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout on expiry.
    static osal::result uc_event_wait_impl(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual_bits,
                                           bool clear_on_exit, bool all, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_OPT wait_opt = all ? OS_OPT_PEND_FLAG_SET_ALL : OS_OPT_PEND_FLAG_SET_ANY;
        if (clear_on_exit)
        {
            wait_opt |= OS_OPT_PEND_FLAG_CONSUME;
        }
        wait_opt |= uc_pend_opt(timeout_ticks);

        OS_ERR         err;
        CPU_TS         ts;
        const OS_FLAGS got = OSFlagPend(static_cast<OS_FLAG_GRP*>(handle->native), static_cast<OS_FLAGS>(wait_bits),
                                        to_uc_ticks(timeout_ticks), wait_opt, &ts, &err);
        if (actual_bits != nullptr)
        {
            *actual_bits = static_cast<osal::event_bits_t>(got);
        }
        if (err == OS_ERR_NONE)
        {
            return osal::ok();
        }
        return osal::error_code::timeout;
    }

    /// @brief Wait until any of the specified event bits are set.
    /// @param handle  Event flags handle.
    /// @param bits    Bit mask: wake when any bit in this mask becomes set.
    /// @param actual  Optional output: full current bits at wakeup.
    /// @param coe     Clear matched bits after wakeup when true.
    /// @param timeout Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout on expiry.
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool coe, osal::tick_t timeout) noexcept
    {
        return uc_event_wait_impl(handle, bits, actual, coe, false, timeout);
    }

    /// @brief Wait until all of the specified event bits are set simultaneously.
    /// @param handle  Event flags handle.
    /// @param bits    Bit mask: wake when every bit in this mask is set.
    /// @param actual  Optional output: full current bits at wakeup.
    /// @param coe     Clear matched bits after wakeup when true.
    /// @param timeout Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout on expiry.
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool coe, osal::tick_t timeout) noexcept
    {
        return uc_event_wait_impl(handle, bits, actual, coe, true, timeout);
    }

    /// @brief Set event bits from an ISR context (OSFlagPost is ISR-safe).
    /// @param handle Event flags handle.
    /// @param bits   Bit mask of flags to set.
    /// @return osal::ok() on success; osal::error_code::unknown on OS failure.
    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        return osal_event_flags_set(handle, bits);  // OSFlagPost is ISR-safe
    }

    // ---------------------------------------------------------------------------
    // Wait-set — not supported on µC/OS-III
    // ---------------------------------------------------------------------------

    /// @brief Create a wait-set (not supported on µC/OS-III).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Destroy a wait-set (not supported on µC/OS-III).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Add an object to a wait-set (not supported on µC/OS-III).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Remove an object from a wait-set (not supported on µC/OS-III).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait on a wait-set for any registered object to signal (not supported on µC/OS-III).
    /// @param n Sets *n to 0.
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t*, int*, std::size_t, std::size_t* n,
                                    osal::tick_t) noexcept
    {
        if (n)
            *n = 0U;
        return osal::error_code::not_supported;
    }

    // ---------------------------------------------------------------------------
    // Condition variable (emulated — Birrell pattern: mutex + semaphore)
    // ---------------------------------------------------------------------------

#include "../common/emulated_condvar.inl"

    // ---------------------------------------------------------------------------
    // Work queue (emulated — OSAL thread + mutex + semaphore + ring buffer)
    // ---------------------------------------------------------------------------

#include "../common/emulated_work_queue.inl"

    // ---------------------------------------------------------------------------
    // Memory pool (emulated — bitmap + mutex + counting semaphore)
    // ---------------------------------------------------------------------------

#include "../common/emulated_memory_pool.inl"

    // ---------------------------------------------------------------------------
    // Read-write lock (emulated — mutex + condvar + counter)
    // ---------------------------------------------------------------------------

#include "../common/emulated_rwlock.inl"

    // ---------------------------------------------------------------------------
    // Stream buffer (emulated — SPSC lock-free ring + binary semaphores)
    // ---------------------------------------------------------------------------

#include "../common/emulated_stream_buffer.inl"

    // ---------------------------------------------------------------------------
    // Message buffer (emulated — length-prefixed SPSC ring + binary semaphores)
    // ---------------------------------------------------------------------------

#include "../common/emulated_message_buffer.inl"
    // ---------------------------------------------------------------------------
    // Spinlock
    // ---------------------------------------------------------------------------
#include "../common/emulated_spinlock.inl"

    // ---------------------------------------------------------------------------
    // Barrier
    // ---------------------------------------------------------------------------
#include "../common/emulated_barrier.inl"

    // ---------------------------------------------------------------------------
    // Task notification
    // ---------------------------------------------------------------------------
#include "../common/emulated_task_notify.inl"

}  // extern "C"
