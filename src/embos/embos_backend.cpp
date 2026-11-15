// SPDX-License-Identifier: Apache-2.0
/// @file embos_backend.cpp
/// @brief SEGGER embOS implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to SEGGER embOS native API:
///          - thread   → OS_TASK_CreateEx / OS_TASK_Terminate
///          - mutex    → OS_MUTEX_Create / OS_MUTEX_LockTimed / OS_MUTEX_Unlock
///          - semaphore→ OS_SEMAPHORE_Create / OS_SEMAPHORE_TakeTimed / OS_SEMAPHORE_Give
///          - queue    → fixed-size values in caller-owned storage
///          - timer    → OS_TIMER_CreateEx / OS_TIMER_Start / OS_TIMER_Stop
///          - event_flags→ OS_EVENT_Set / OS_EVENT_GetMaskTimed / OS_EVENT_Reset
///
///          All kernel objects are held in static pools.
///          Dynamic allocation is never used.
///
///          Includes required: <RTOS.h> (embOS master header)
///          Build macro:       OSAL_BACKEND_EMBOS
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_EMBOS
#define OSAL_BACKEND_EMBOS
#endif
#include <osal/osal.hpp>

#include <RTOS.h>  ///< SEGGER embOS header
#include <atomic>
#include <limits>
#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Pool sizes
// ---------------------------------------------------------------------------
#define OSAL_EMBOS_MAX_THREADS 8
#define OSAL_EMBOS_MAX_MUTEXES 16
#define OSAL_EMBOS_MAX_SEMS 16
#define OSAL_EMBOS_MAX_QUEUES 8
#define OSAL_EMBOS_MAX_TIMERS 8
#define OSAL_EMBOS_MAX_EVENTS 8

// ---------------------------------------------------------------------------
// Static pools
// ---------------------------------------------------------------------------
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
bounded_sem_count eos_sem_counts[OSAL_EMBOS_MAX_SEMS];

OS_TASK           eos_tasks[OSAL_EMBOS_MAX_THREADS];
std::atomic<bool> eos_task_used[OSAL_EMBOS_MAX_THREADS];
OS_MUTEX          eos_mutexes[OSAL_EMBOS_MAX_MUTEXES];
std::atomic<bool> eos_mutex_used[OSAL_EMBOS_MAX_MUTEXES];
OS_SEMAPHORE      eos_sems[OSAL_EMBOS_MAX_SEMS];
std::atomic<bool> eos_sem_used[OSAL_EMBOS_MAX_SEMS];

struct eos_queue_slot
{
    std::uint8_t*     storage{};
    std::size_t       item_size{}, capacity{}, head{}, tail{}, count{};
    std::atomic<bool> used{false};
};
eos_queue_slot eos_queues[OSAL_EMBOS_MAX_QUEUES];

struct eos_timer_slot
{
    OS_TIMER_EX           tmr{};
    osal_timer_callback_t fn          = nullptr;
    void*                 arg         = nullptr;
    OS_TIME               period      = 0;
    bool                  auto_reload = false;
    std::atomic<bool>     used{false};
    std::atomic<bool>     callback_gate{false};
    std::atomic<bool>     ever_started{false};
    std::atomic<bool>     active{false};
};
eos_timer_slot eos_timers[OSAL_EMBOS_MAX_TIMERS];

OS_EVENT          eos_events[OSAL_EMBOS_MAX_EVENTS];
std::atomic<bool> eos_event_used[OSAL_EMBOS_MAX_EVENTS];
static_assert(std::atomic<bool>::is_always_lock_free);

struct eos_irq_guard
{
    eos_irq_guard() { OS_IncDI(); }
    ~eos_irq_guard() { OS_DecRI(); }
};

// ---------------------------------------------------------------------------
// Pool helpers
// ---------------------------------------------------------------------------
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

template<typename T, std::size_t N>
T* pool_acquire_by_used(T (&pool)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!pool[i].used.exchange(true, std::memory_order_acquire))
        {
            return &pool[i];
        }
    }
    return nullptr;
}

template<typename T, std::size_t N>
void pool_release_by_used(T (&pool)[N], T* p) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (&pool[i] == p)
        {
            pool[i].used.store(false, std::memory_order_release);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr OS_PRIO osal_to_embos_priority(osal::priority_t p) noexcept
{
    if (p == 0U)
    {
        return 1U;  // minimum non-idle
    }
    return static_cast<OS_PRIO>(p);
}

constexpr OS_TIME to_embos_ticks(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return 0;  // embOS: 0 = wait forever for timed operations
    }
    if (t == osal::NO_WAIT)
    {
        return 1;  // minimum ticks — checked separately
    }
    return static_cast<OS_TIME>(t);  // NOLINT(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------
void eos_timer_callback(void* p_arg) noexcept
{
    auto* slot = static_cast<eos_timer_slot*>(p_arg);
    if (slot == nullptr || slot->callback_gate.exchange(true, std::memory_order_acquire))
    {
        return;
    }
    if (slot->active && slot->fn != nullptr)
    {
        slot->active = slot->auto_reload;
        slot->fn(slot->arg);
    }
    // Retain the lifetime gate through rearm: callback-side destroy must fail.
    if (slot->active && slot->auto_reload)
    {
        OS_TIMER_StartEx(&slot->tmr);
    }
    slot->callback_gate.store(false, std::memory_order_release);
}

}  // namespace

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(OS_TIME_GetResult_ms(OS_TIME_Get_Cycles()));
    }

    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();
    }

    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(OS_TIME_Get());
    }

    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        const OS_U32 freq = OS_TIME_GetFreq();
        return (freq > 0U) ? static_cast<std::uint32_t>(1'000'000U / freq) : 1000U;
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
        OS_TASK* t = pool_acquire(eos_tasks, eos_task_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        OS_TASK_CreateEx(t, name != nullptr ? name : "osal", osal_to_embos_priority(priority),
                         reinterpret_cast<void (*)(void*)>(entry), stack, static_cast<OS_UINT>(stack_bytes), 0U, arg);
        handle->native = static_cast<void*>(t);
        return osal::ok();
    }

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* t = static_cast<OS_TASK*>(handle->native);
        OS_TASK_Terminate(t);
        pool_release(eos_tasks, eos_task_used, t);
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
        OS_TASK_SetPriority(static_cast<OS_TASK*>(handle->native), osal_to_embos_priority(priority));
        return osal::ok();
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
        OS_TASK_Yield();
    }

    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        OS_TASK_Delay(static_cast<OS_TIME>(ms));
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        OS_MUTEX* m = pool_acquire(eos_mutexes, eos_mutex_used);
        if (m == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_MUTEX_Create(m);
        handle->native = static_cast<void*>(m);
        return osal::ok();
    }

    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* m = static_cast<OS_MUTEX*>(handle->native);
        OS_MUTEX_Delete(m);
        pool_release(eos_mutexes, eos_mutex_used, m);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* m = static_cast<OS_MUTEX*>(handle->native);

        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            OS_MUTEX_Lock(m);
            return osal::ok();
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            const int r = OS_MUTEX_LockTimed(m, 1U);
            return (r != 0) ? osal::ok() : osal::error_code::would_block;
        }
        const int r = OS_MUTEX_LockTimed(m, to_embos_ticks(timeout_ticks));  // NOLINT(cppcoreguidelines-init-variables)
        return (r != 0) ? osal::ok() : osal::error_code::timeout;
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
        OS_MUTEX_Unlock(static_cast<OS_MUTEX*>(handle->native));
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Semaphore
    // ---------------------------------------------------------------------------

    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        if (handle == nullptr || max_count == 0U || initial_count > max_count ||
            max_count > std::numeric_limits<OS_UINT>::max())
        {
            return osal::error_code::invalid_argument;
        }
        OS_SEMAPHORE* s = pool_acquire(eos_sems, eos_sem_used);
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_SEMAPHORE_Create(s, static_cast<OS_UINT>(initial_count));
        handle->native                       = static_cast<void*>(s);
        eos_sem_counts[s - eos_sems].maximum = max_count;
        eos_sem_counts[s - eos_sems].count.store(initial_count, std::memory_order_relaxed);
        return osal::ok();
    }

    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* s = static_cast<OS_SEMAPHORE*>(handle->native);
        OS_SEMAPHORE_Delete(s);
        pool_release(eos_sems, eos_sem_used, s);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto&      count    = eos_sem_counts[static_cast<OS_SEMAPHORE*>(handle->native) - eos_sems];
        const auto reserved = count.reserve();
        if (!reserved.ok())
        {
            return reserved;
        }
        OS_SEMAPHORE_Give(static_cast<OS_SEMAPHORE*>(handle->native));
        return osal::ok();
    }

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // embOS APIs are ISR-safe
    }

    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<OS_SEMAPHORE*>(handle->native);

        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            OS_SEMAPHORE_Take(s);
            eos_sem_counts[s - eos_sems].count.fetch_sub(1U, std::memory_order_relaxed);
            return osal::ok();
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            const int r = OS_SEMAPHORE_TakeTimed(s, 1U);
            if (r != 0)
            {
                eos_sem_counts[s - eos_sems].count.fetch_sub(1U, std::memory_order_relaxed);
            }
            return (r != 0) ? osal::ok() : osal::error_code::would_block;
        }
        const int r =
            OS_SEMAPHORE_TakeTimed(s, to_embos_ticks(timeout_ticks));  // NOLINT(cppcoreguidelines-init-variables)
        if (r != 0)
        {
            eos_sem_counts[s - eos_sems].count.fetch_sub(1U, std::memory_order_relaxed);
        }
        return (r != 0) ? osal::ok() : osal::error_code::timeout;
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
        auto* slot = pool_acquire_by_used(eos_queues);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->storage   = static_cast<std::uint8_t*>(buffer);
        slot->item_size = item_size;
        slot->capacity  = capacity;
        slot->head = slot->tail = slot->count = 0U;
        handle->native                        = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<eos_queue_slot*>(handle->native);
        pool_release_by_used(eos_queues, slot);
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
        auto*      slot  = static_cast<eos_queue_slot*>(handle->native);
        const auto start = OS_TIME_Get();
        for (;;)
        {
            {
                eos_irq_guard guard;
                if (slot->count < slot->capacity)
                {
                    std::memcpy(slot->storage + slot->tail * slot->item_size, item, slot->item_size);
                    slot->tail = (slot->tail + 1U) % slot->capacity;
                    ++slot->count;
                    return osal::ok();
                }
            }
            if (timeout_ticks == osal::NO_WAIT)
            {
                return osal::error_code::would_block;
            }
            if (timeout_ticks != osal::WAIT_FOREVER &&
                static_cast<osal::tick_t>(static_cast<OS_U32>(OS_TIME_Get()) - static_cast<OS_U32>(start)) >=
                    timeout_ticks)
            {
                return osal::error_code::timeout;
            }
            OS_TASK_Delay(1);
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
        auto*      slot  = static_cast<eos_queue_slot*>(handle->native);
        const auto start = OS_TIME_Get();
        for (;;)
        {
            {
                eos_irq_guard guard;
                if (slot->count != 0U)
                {
                    std::memcpy(item, slot->storage + slot->head * slot->item_size, slot->item_size);
                    slot->head = (slot->head + 1U) % slot->capacity;
                    --slot->count;
                    return osal::ok();
                }
            }
            if (timeout_ticks == osal::NO_WAIT)
            {
                return osal::error_code::would_block;
            }
            if (timeout_ticks != osal::WAIT_FOREVER &&
                static_cast<osal::tick_t>(static_cast<OS_U32>(OS_TIME_Get()) - static_cast<OS_U32>(start)) >=
                    timeout_ticks)
            {
                return osal::error_code::timeout;
            }
            OS_TASK_Delay(1);
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
        eos_irq_guard guard;
        const auto*   slot = static_cast<const eos_queue_slot*>(handle->native);
        return slot->count;
    }

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        eos_irq_guard guard;
        const auto*   slot = static_cast<const eos_queue_slot*>(handle->native);
        return slot->capacity - slot->count;
    }

    // ---------------------------------------------------------------------------
    // Timer (OS_TIMER_EX — extended timer with user context)
    // ---------------------------------------------------------------------------

    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        if (handle == nullptr || callback == nullptr || period_ticks == 0U || period_ticks == osal::WAIT_FOREVER ||
            period_ticks > static_cast<osal::tick_t>(std::numeric_limits<OS_TIME>::max()))
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        auto* slot = pool_acquire_by_used(eos_timers);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->fn          = callback;
        slot->arg         = arg;
        slot->period      = static_cast<OS_TIME>(period_ticks);
        slot->auto_reload = auto_reload;
        slot->callback_gate.store(false, std::memory_order_release);
        slot->ever_started.store(false, std::memory_order_relaxed);
        slot->active.store(false, std::memory_order_relaxed);

        OS_TIMER_CreateEx(&slot->tmr, eos_timer_callback, slot->period, slot);
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<eos_timer_slot*>(handle->native);
        // Native deletion is not a verified callback-dispatch barrier.
        if (slot->ever_started.load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        if (slot->callback_gate.exchange(true, std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        OS_TIMER_DeleteEx(&slot->tmr);
        slot->fn          = nullptr;
        slot->auto_reload = false;
        // Keep the gate closed until a subsequent create initializes this slot.
        pool_release_by_used(eos_timers, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<eos_timer_slot*>(handle->native);
        if (slot->ever_started.exchange(true, std::memory_order_acq_rel))
        {
            return osal::error_code::not_supported;
        }
        slot->active = true;
        OS_TIMER_SetPeriodEx(&slot->tmr, slot->period);
        OS_TIMER_StartEx(&slot->tmr);
        return osal::ok();
    }

    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<eos_timer_slot*>(handle->native);
        if (slot->callback_gate.exchange(true, std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        slot->active = false;
        OS_TIMER_StopEx(&slot->tmr);
        slot->callback_gate.store(false, std::memory_order_release);
        return osal::ok();
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
        auto* slot = static_cast<eos_timer_slot*>(handle->native);
        if (slot->active.load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        if (new_period_ticks == 0U || new_period_ticks == osal::WAIT_FOREVER ||
            new_period_ticks > static_cast<osal::tick_t>(std::numeric_limits<OS_TIME>::max()))
        {
            return osal::error_code::invalid_argument;
        }
        if (slot->callback_gate.exchange(true, std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        slot->period = static_cast<OS_TIME>(new_period_ticks);
        slot->callback_gate.store(false, std::memory_order_release);
        return osal::ok();
    }

    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        return OS_TIMER_IsRunningEx(&static_cast<const eos_timer_slot*>(handle->native)->tmr) != 0U;
    }

    // ---------------------------------------------------------------------------
    // Event flags (OS_EVENT — embOS native event object)
    // ---------------------------------------------------------------------------

    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        OS_EVENT* e = pool_acquire(eos_events, eos_event_used);
        if (e == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_EVENT_Create(e);
        handle->native = static_cast<void*>(e);
        return osal::ok();
    }

    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* e = static_cast<OS_EVENT*>(handle->native);
        OS_EVENT_Delete(e);
        pool_release(eos_events, eos_event_used, e);
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
        OS_EVENT_Set(static_cast<OS_EVENT*>(handle->native), static_cast<OS_EVENT_MASK>(bits));
        return osal::ok();
    }

    // embOS OS_EVENT_Reset() clears all flags; selective clear is best-effort.
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_EVENT_ResetMask(static_cast<OS_EVENT*>(handle->native), static_cast<OS_EVENT_MASK>(bits));
        return osal::ok();
    }

    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<osal::event_bits_t>(OS_EVENT_Get(static_cast<const OS_EVENT*>(handle->native)));
    }

    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*         e    = static_cast<OS_EVENT*>(handle->native);
        OS_EVENT_MASK mask = static_cast<OS_EVENT_MASK>(bits);
        OS_EVENT_MASK got  = 0U;

        if (timeout == osal::WAIT_FOREVER)
        {
            got = OS_EVENT_GetMask(e, mask);
        }
        else
        {
            got = OS_EVENT_GetMaskTimed(e, mask, to_embos_ticks(timeout));
        }

        if (got != 0U)
        {
            if (actual != nullptr)
            {
                *actual = static_cast<osal::event_bits_t>(got);
            }
            if (clear_on_exit)
            {
                OS_EVENT_ResetMask(e, got);
            }
            return osal::ok();
        }
        return osal::error_code::timeout;
    }

    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*         e    = static_cast<OS_EVENT*>(handle->native);
        OS_EVENT_MASK mask = static_cast<OS_EVENT_MASK>(bits);
        OS_EVENT_MASK got  = 0U;

        if (timeout == osal::WAIT_FOREVER)  // NOLINT(bugprone-branch-clone)
        {
            got = OS_EVENT_GetMaskMode(e, mask, OS_EVENT_MODE_SET_ALL);
        }
        else
        {
            got = OS_EVENT_GetMaskModeTimed(e, mask, OS_EVENT_MODE_SET_ALL, to_embos_ticks(timeout));
        }

        if ((got & mask) == mask)
        {
            if (actual != nullptr)
            {
                *actual = static_cast<osal::event_bits_t>(got);
            }
            if (clear_on_exit)
            {
                OS_EVENT_ResetMask(e, mask);
            }
            return osal::ok();
        }
        return osal::error_code::timeout;
    }

    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        return osal_event_flags_set(handle, bits);  // embOS API calls are ISR-safe
    }

    // ---------------------------------------------------------------------------
    // Wait-set — not supported
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

    osal::result osal_task_notify(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_TASKEVENT_Set(static_cast<OS_TASK*>(handle->native), static_cast<OS_TASKEVENT>(value));
        return osal::ok();
    }

    osal::result osal_task_notify_isr(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_TASKEVENT_SetFromISR(static_cast<OS_TASK*>(handle->native), static_cast<OS_TASKEVENT>(value));
        return osal::ok();
    }

    // Maps onto OS_TASKEVENT_GetMaskedEx for finite waits and
    // OS_TASKEVENT_GetMasked for indefinite waits.
    // The clear_on_entry / clear_on_exit semantics are
    // approximated: the calling task's pending event bits are returned
    // in value_out and then cleared from the task's event register
    // by the embOS receive primitive.
    osal::result osal_task_notify_wait(std::uint32_t clear_on_entry, std::uint32_t clear_on_exit,
                                       std::uint32_t* value_out, osal::tick_t timeout_ticks) noexcept
    {
        (void)clear_on_exit;  // embOS clears received bits automatically

        // Best-effort clear of specified bits before waiting
        if (clear_on_entry != 0U)
        {
            OS_TASKEVENT_Clear(static_cast<OS_TASKEVENT>(clear_on_entry));
        }

        OS_TASKEVENT received = 0U;
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            received = OS_TASKEVENT_GetMasked(static_cast<OS_TASKEVENT>(0xFFFFU));
        }
        else if (timeout_ticks == osal::NO_WAIT)
        {
            received = OS_TASKEVENT_Get();
            if (received == 0U)
            {
                return osal::error_code::timeout;
            }
        }
        else
        {
            OS_TASKEVENT got = 0U;
            const int    rc  =  // NOLINT(cppcoreguidelines-init-variables)
                OS_TASKEVENT_GetMaskedEx(static_cast<OS_TASKEVENT>(0xFFFFU), to_embos_ticks(timeout_ticks), &got);
            if (rc != 0)
            {
                return osal::error_code::timeout;
            }
            received = got;
        }

        if (value_out != nullptr)
        {
            *value_out = static_cast<std::uint32_t>(received);
        }
        return osal::ok();
    }

}  // extern "C"
