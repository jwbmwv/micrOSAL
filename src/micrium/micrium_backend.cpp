// SPDX-License-Identifier: Apache-2.0
/// @file micrium_backend.cpp
/// @brief Micrium µC/OS-III implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the Micrium µC/OS-III API:
///          - thread   → OSTaskCreate / OSTaskDel
///          - mutex    → OSMutexCreate / OSMutexPend / OSMutexPost
///          - semaphore→ OSSemCreate / OSSemPend / OSSemPost
///          - queue    → fixed-size values in caller-owned storage
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
#include <atomic>
#include <limits>
#include <cassert>
#include <cstring>

namespace
{
struct bounded_sem_count
{
    std::atomic<unsigned> count{};
    unsigned              maximum{};

    osal::result reserve() noexcept
    {
        unsigned value = count.load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < 32U; ++attempt)
        {
            if (value >= maximum)
            {
                return osal::error_code::overflow;
            }
            if (count.compare_exchange_weak(value, value + 1U, std::memory_order_relaxed))
            {
                return osal::ok();
            }
        }
        return osal::error_code::would_block;
    }
};
static_assert(std::atomic<unsigned>::is_always_lock_free);

// ---------------------------------------------------------------------------
// Pool sizes
// ---------------------------------------------------------------------------
#define OSAL_UC_MAX_THREADS 8
#define OSAL_UC_MAX_MUTEXES 16
#define OSAL_UC_MAX_SEMS 16
#define OSAL_UC_MAX_QUEUES 8
#define OSAL_UC_MAX_TIMERS 8
#define OSAL_UC_MAX_FLAGS 8
bounded_sem_count uc_sem_counts[OSAL_UC_MAX_SEMS];

// ---------------------------------------------------------------------------
// Static object pools
// ---------------------------------------------------------------------------
OS_TCB            uc_tcbs[OSAL_UC_MAX_THREADS];
std::atomic<bool> uc_tcb_used[OSAL_UC_MAX_THREADS];
OS_MUTEX          uc_mutexes[OSAL_UC_MAX_MUTEXES];
std::atomic<bool> uc_mutex_used[OSAL_UC_MAX_MUTEXES];
OS_SEM            uc_sems[OSAL_UC_MAX_SEMS];
std::atomic<bool> uc_sem_used[OSAL_UC_MAX_SEMS];
struct uc_queue_slot
{
    std::uint8_t* storage{};
    std::size_t   item_size{}, capacity{}, head{}, tail{}, count{};
};
uc_queue_slot     uc_queues[OSAL_UC_MAX_QUEUES];
std::atomic<bool> uc_queue_used[OSAL_UC_MAX_QUEUES];
OS_TMR            uc_timers[OSAL_UC_MAX_TIMERS];
std::atomic<bool> uc_timer_used[OSAL_UC_MAX_TIMERS];
OS_FLAG_GRP       uc_flags[OSAL_UC_MAX_FLAGS];
std::atomic<bool> uc_flag_used[OSAL_UC_MAX_FLAGS];
static_assert(std::atomic<bool>::is_always_lock_free);

struct uc_irq_guard
{
    CPU_SR state = CPU_SR_Save();
    ~uc_irq_guard() { CPU_SR_Restore(state); }
};

template<typename T, std::size_t N>
T* pool_acquire(T (&pool)[N], std::atomic<bool> (&used)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!used[i].exchange(true, std::memory_order_acquire))
        {
            return &pool[i];
        }
    }
    return nullptr;
}

template<typename T, std::size_t N>
void pool_release(T (&pool)[N], std::atomic<bool> (&used)[N], T* p) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (&pool[i] == p)
        {
            used[i].store(false, std::memory_order_release);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr OS_PRIO osal_to_uc_priority(osal::priority_t p) noexcept
{
    const OS_PRIO max_prio = OS_CFG_PRIO_MAX - 2U;  // 0 is reserved for ISR, max-1 for idle
    return max_prio - static_cast<OS_PRIO>((static_cast<std::uint32_t>(p) * static_cast<std::uint32_t>(max_prio)) /
                                           static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
}

constexpr OS_TICK to_uc_ticks(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return 0U;  // µC/OS-III: timeout=0 means wait forever
    }
    if (t == osal::NO_WAIT)
    {
        return 1U;  // Minimum non-zero; checked separately with OS_OPT_PEND_NON_BLOCKING
    }
    return static_cast<OS_TICK>(t);  // NOLINT(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
}

constexpr OS_OPT uc_pend_opt(osal::tick_t t) noexcept
{
    return (t == osal::NO_WAIT) ? OS_OPT_PEND_NON_BLOCKING : OS_OPT_PEND_BLOCKING;
}

// ---------------------------------------------------------------------------
// Timer callback context
// ---------------------------------------------------------------------------
struct uc_timer_ctx
{
    osal_timer_callback_t fn{};
    void*                 arg{};
    OS_TMR*               tmr{};
    std::atomic<bool>     ever_started{};
    bool                  auto_reload{};
};
uc_timer_ctx uc_timer_ctxs[OSAL_UC_MAX_TIMERS];

void uc_tmr_callback(void* /*p_tmr*/, void* p_arg) noexcept
{
    auto* ctx = static_cast<uc_timer_ctx*>(p_arg);
    if (ctx != nullptr && ctx->fn != nullptr)
    {
        ctx->fn(ctx->arg);
    }
}

osal::result uc_event_wait_impl(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t wait_bits,
                                osal::event_bits_t* actual_bits, bool clear_on_exit, bool all,
                                osal::tick_t timeout_ticks) noexcept
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

}  // namespace

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        OS_ERR        err;
        const OS_TICK ticks = OSTimeGet(&err);
        return static_cast<std::int64_t>(ticks) * 1000LL / OSCfg_TickRate_Hz;
    }

    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();  // µC/OS-III has no wall clock
    }

    osal::tick_t osal_clock_ticks() noexcept
    {
        OS_ERR err;
        return static_cast<osal::tick_t>(OSTimeGet(&err));
    }

    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return static_cast<std::uint32_t>(1'000'000U / OSCfg_TickRate_Hz);
    }

    // ---------------------------------------------------------------------------
    // Thread (Task)
    // ---------------------------------------------------------------------------

    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        if (handle == nullptr || entry == nullptr || stack == nullptr || stack_bytes == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
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

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        auto*  tcb = static_cast<OS_TCB*>(handle->native);
        OSTaskDel(tcb, &err);
        if (err != OS_ERR_NONE && err != OS_ERR_TASK_NOT_EXIST)
        {
            return osal::error_code::unknown;
        }
        pool_release(uc_tcbs, uc_tcb_used, tcb);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return osal::error_code::not_supported;
    }

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

    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    void osal_thread_yield() noexcept
    {
        OS_ERR err;
        OSTimeDly(0U, OS_OPT_TIME_DLY, &err);
    }

    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        OS_ERR err;
        OSTimeDlyHMSM(0U, 0U, static_cast<CPU_INT32U>(ms / 1000U), static_cast<CPU_INT32U>(ms % 1000U),
                      OS_OPT_TIME_HMSM_STRICT, &err);
    }

    // ---------------------------------------------------------------------------
    // Mutex (OS_MUTEX — recursive, priority inheritance)
    // ---------------------------------------------------------------------------

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

    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

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

    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        if (handle == nullptr || max_count == 0U || initial_count > max_count ||
            max_count > std::numeric_limits<OS_SEM_CTR>::max())
        {
            return osal::error_code::invalid_argument;
        }
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
        handle->native                     = static_cast<void*>(s);
        uc_sem_counts[s - uc_sems].maximum = max_count;
        uc_sem_counts[s - uc_sems].count.store(initial_count, std::memory_order_relaxed);
        return osal::ok();
    }

    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        OS_ERR err;
        auto*  s = static_cast<OS_SEM*>(handle->native);
        OSSemDel(s, OS_OPT_DEL_ALWAYS, &err);
        if (err != OS_ERR_NONE)
        {
            return osal::error_code::unknown;
        }
        pool_release(uc_sems, uc_sem_used, s);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR     err;
        auto&      count    = uc_sem_counts[static_cast<OS_SEM*>(handle->native) - uc_sems];
        const auto reserved = count.reserve();
        if (!reserved.ok())
        {
            return reserved;
        }
        OSSemPost(static_cast<OS_SEM*>(handle->native), OS_OPT_POST_1, &err);
        if (err != OS_ERR_NONE)
        {
            count.count.fetch_sub(1U, std::memory_order_relaxed);
        }
        return (err == OS_ERR_NONE) ? osal::ok() : osal::error_code::overflow;
    }

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // OSSemPost is ISR-safe
    }

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
            uc_sem_counts[static_cast<OS_SEM*>(handle->native) - uc_sems].count.fetch_sub(1U,
                                                                                          std::memory_order_relaxed);
            return osal::ok();
        }
        if (err == OS_ERR_TIMEOUT || err == OS_ERR_PEND_WOULD_BLOCK)
        {
            return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
        }
        return osal::error_code::unknown;
    }

    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (fixed-size values in caller-owned storage)
    // ---------------------------------------------------------------------------

    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        if (handle == nullptr || buffer == nullptr || item_size == 0U || capacity == 0U ||
            capacity > std::numeric_limits<std::size_t>::max() / item_size)
        {
            return osal::error_code::invalid_argument;
        }
        auto* q = pool_acquire(uc_queues, uc_queue_used);
        if (q == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        *q             = {static_cast<std::uint8_t*>(buffer), item_size, capacity, 0U, 0U, 0U};
        handle->native = static_cast<void*>(q);
        return osal::ok();
    }

    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* q = static_cast<uc_queue_slot*>(handle->native);
        pool_release(uc_queues, uc_queue_used, q);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (item == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        auto*         q = static_cast<uc_queue_slot*>(handle->native);
        OS_ERR        err;
        const OS_TICK start = timeout_ticks == osal::NO_WAIT ? 0U : OSTimeGet(&err);
        for (;;)
        {
            {
                uc_irq_guard guard;
                if (q->count < q->capacity)
                {
                    std::memcpy(q->storage + q->tail * q->item_size, item, q->item_size);
                    q->tail = (q->tail + 1U) % q->capacity;
                    ++q->count;
                    return osal::ok();
                }
            }
            if (timeout_ticks == osal::NO_WAIT)
            {
                return osal::error_code::would_block;
            }
            if (timeout_ticks != osal::WAIT_FOREVER && static_cast<OS_TICK>(OSTimeGet(&err) - start) >= timeout_ticks)
            {
                return osal::error_code::timeout;
            }
            OSTimeDly(1U, OS_OPT_TIME_DLY, &err);
            if (err != OS_ERR_NONE)
            {
                return osal::error_code::unknown;
            }
        }
    }

    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (item == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        auto*         q = static_cast<uc_queue_slot*>(handle->native);
        OS_ERR        err;
        const OS_TICK start = timeout_ticks == osal::NO_WAIT ? 0U : OSTimeGet(&err);
        for (;;)
        {
            {
                uc_irq_guard guard;
                if (q->count != 0U)
                {
                    std::memcpy(item, q->storage + q->head * q->item_size, q->item_size);
                    q->head = (q->head + 1U) % q->capacity;
                    --q->count;
                    return osal::ok();
                }
            }
            if (timeout_ticks == osal::NO_WAIT)
            {
                return osal::error_code::would_block;
            }
            if (timeout_ticks != osal::WAIT_FOREVER && static_cast<OS_TICK>(OSTimeGet(&err) - start) >= timeout_ticks)
            {
                return osal::error_code::timeout;
            }
            OSTimeDly(1U, OS_OPT_TIME_DLY, &err);
            if (err != OS_ERR_NONE)
            {
                return osal::error_code::unknown;
            }
        }
    }

    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* /*handle*/, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        uc_irq_guard guard;
        auto*        q = static_cast<const uc_queue_slot*>(handle->native);
        return q->count;
    }

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        uc_irq_guard guard;
        auto*        q = static_cast<const uc_queue_slot*>(handle->native);
        return q->capacity - q->count;
    }

    // ---------------------------------------------------------------------------
    // Timer (OS_TMR)
    // ---------------------------------------------------------------------------

    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        if (handle == nullptr || callback == nullptr || period_ticks == 0U || period_ticks == osal::WAIT_FOREVER ||
            period_ticks > std::numeric_limits<OS_TICK>::max())
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        OS_TMR* t = pool_acquire(uc_timers, uc_timer_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        auto* ctx = &uc_timer_ctxs[t - uc_timers];
        ctx->ever_started.store(false, std::memory_order_relaxed);
        ctx->auto_reload = auto_reload;
        ctx->fn          = callback;
        ctx->arg         = arg;
        ctx->tmr         = t;

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

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        OS_ERR err;
        auto*  t = static_cast<OS_TMR*>(handle->native);
        // Native deletion has no verified callback lifetime barrier here.
        if (uc_timer_ctxs[t - uc_timers].ever_started.load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        OSTmrDel(t, &err);
        if (err != OS_ERR_NONE)
        {
            return osal::error_code::unknown;
        }
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

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_ERR err;
        uc_timer_ctxs[static_cast<OS_TMR*>(handle->native) - uc_timers].ever_started.store(true,
                                                                                           std::memory_order_release);
        OSTmrStart(static_cast<OS_TMR*>(handle->native), &err);
        return (err == OS_ERR_NONE) ? osal::ok() : osal::error_code::unknown;
    }

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

    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        const auto stopped = osal_timer_stop(handle);
        if (!stopped.ok())
        {
            return stopped;
        }
        return osal_timer_start(handle);
    }

    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*       t   = static_cast<OS_TMR*>(handle->native);
        const auto& ctx = uc_timer_ctxs[t - uc_timers];
        if (ctx.ever_started.load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        if (new_period_ticks == 0U || new_period_ticks == osal::WAIT_FOREVER ||
            new_period_ticks > std::numeric_limits<OS_TICK>::max())
        {
            return osal::error_code::invalid_argument;
        }
        t->Period = ctx.auto_reload ? static_cast<OS_TICK>(new_period_ticks) : 0U;
        t->Dly    = ctx.auto_reload ? 0U : static_cast<OS_TICK>(new_period_ticks);
        return osal::ok();
    }

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

    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<osal::event_bits_t>(static_cast<const OS_FLAG_GRP*>(handle->native)->Flags);
    }

    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool coe, osal::tick_t timeout) noexcept
    {
        return uc_event_wait_impl(handle, bits, actual, coe, false, timeout);
    }

    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool coe, osal::tick_t timeout) noexcept
    {
        return uc_event_wait_impl(handle, bits, actual, coe, true, timeout);
    }

    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        return osal_event_flags_set(handle, bits);  // OSFlagPost is ISR-safe
    }

    // ---------------------------------------------------------------------------
    // Wait-set — not supported on µC/OS-III
    // ---------------------------------------------------------------------------

    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t*, int*, std::size_t, std::size_t* n,
                                    osal::tick_t) noexcept
    {
        if (n != nullptr)
        {
            *n = 0U;
        }
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
