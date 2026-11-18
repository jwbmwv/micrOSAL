// SPDX-License-Identifier: Apache-2.0
/// @file cmsis_rtos_backend.cpp
/// @brief CMSIS-RTOS v1 implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the CMSIS-RTOS v1 API (cmsis_os.h):
///          - thread    → osThreadCreate / osThreadTerminate
///          - mutex     → osMutexCreate / osMutexWait / osMutexRelease
///          - semaphore → osSemaphoreCreate / osSemaphoreWait / osSemaphoreRelease
///          - queue     → osMessageCreate / osMessagePut / osMessageGet
///          - timer     → osTimerCreate / osTimerStart / osTimerStop
///          - memory_pool → osPoolCreate / osPoolAlloc / osPoolFree
///
///          CMSIS-RTOS v1 does not provide:
///          - Per-object event flags (osSignal is per-thread) → emulated
///          - Condition variable → emulated (Birrell pattern)
///          - Work queue → emulated (thread + ring buffer)
///          - Read-write lock → emulated (mutex + condvar + counter)
///
///          The CMSIS-RTOS v1 API uses definition macros (osMutexDef, etc.) to
///          create static control blocks.  This backend pre-creates pools of
///          definitions for each primitive type.
///
///          Includes required: <cmsis_os.h> (CMSIS-RTOS v1 header)
///          Build macro:       OSAL_BACKEND_CMSIS_RTOS
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_CMSIS_RTOS
#define OSAL_BACKEND_CMSIS_RTOS
#endif
#include <osal/osal.hpp>

#include <cmsis_os.h>  ///< CMSIS-RTOS v1 header
#include <atomic>
#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Pool sizes (tunable)
// ---------------------------------------------------------------------------
#ifndef OSAL_CMSIS1_MAX_THREADS
#define OSAL_CMSIS1_MAX_THREADS 8
#endif
#ifndef OSAL_CMSIS1_MAX_MUTEXES
#define OSAL_CMSIS1_MAX_MUTEXES 16
#endif
#ifndef OSAL_CMSIS1_MAX_SEMS
#define OSAL_CMSIS1_MAX_SEMS 16
#endif
#ifndef OSAL_CMSIS1_MAX_QUEUES
#define OSAL_CMSIS1_MAX_QUEUES 8
#endif
#ifndef OSAL_CMSIS1_MAX_TIMERS
#define OSAL_CMSIS1_MAX_TIMERS 8
#endif
#ifndef OSAL_CMSIS1_MAX_EVENTS
#define OSAL_CMSIS1_MAX_EVENTS 8
#endif
#ifndef OSAL_CMSIS1_MAX_POOLS
#define OSAL_CMSIS1_MAX_POOLS 4
#endif

// ---------------------------------------------------------------------------
// Pool tracking — CMSIS-RTOS v1 IDs are stored directly in handle->native
// ---------------------------------------------------------------------------
namespace
{

struct cmsis1_thread_slot
{
    osThreadId        id{};
    std::atomic<bool> used{};
};
cmsis1_thread_slot cmsis1_threads[OSAL_CMSIS1_MAX_THREADS];

struct cmsis1_mutex_slot
{
    osMutexId         id{};
    std::atomic<bool> used{};
};
cmsis1_mutex_slot cmsis1_mutexes[OSAL_CMSIS1_MAX_MUTEXES];

struct cmsis1_sem_slot
{
    osSemaphoreId         id{};
    std::atomic<bool>     used{};
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
cmsis1_sem_slot cmsis1_sems[OSAL_CMSIS1_MAX_SEMS];

struct cmsis1_queue_slot
{
    osMessageQId             id{};
    std::size_t              item_size{};
    std::size_t              capacity{};
    std::atomic<std::size_t> count{0U};
    std::atomic<bool>        used{};
    std::atomic<bool>        active{};
};
cmsis1_queue_slot cmsis1_queues[OSAL_CMSIS1_MAX_QUEUES];

cmsis1_queue_slot* cmsis1_queue_acquire(std::size_t item_size, std::size_t capacity) noexcept
{
    // CMSIS-RTOS v1 has no portable message-queue delete operation. Reuse a
    // compatible native queue before consuming a previously unused slot.
    for (unsigned pass = 0U; pass < 2U; ++pass)
    {
        for (auto& queue : cmsis1_queues)
        {
            bool inactive = false;
            if (queue.active.compare_exchange_strong(inactive, true, std::memory_order_acquire))
            {
                const bool matches =
                    pass == 0U ? queue.used && queue.item_size == item_size && queue.capacity == capacity : !queue.used;
                if (matches)
                {
                    queue.used      = true;
                    queue.item_size = item_size;
                    queue.capacity  = capacity;
                    queue.count.store(0U, std::memory_order_relaxed);
                    return &queue;
                }
                queue.active.store(false, std::memory_order_release);
            }
        }
    }
    return nullptr;
}

void cmsis1_queue_drain(cmsis1_queue_slot* queue) noexcept
{
    if (queue == nullptr || queue->id == nullptr)
    {
        return;
    }

    while (osMessageGet(queue->id, 0U).status == osEventMessage)
    {
    }
    queue->count.store(0U, std::memory_order_relaxed);
}

struct cmsis1_timer_slot
{
    osTimerId             id{};
    osal_timer_callback_t fn{};
    void*                 arg{};
    osal::tick_t          period{};
    bool                  auto_reload{};
    std::atomic<bool>     used{};
    std::atomic<bool>     ever_started{};
    std::atomic<bool>     active{};
};
cmsis1_timer_slot cmsis1_timers[OSAL_CMSIS1_MAX_TIMERS];

struct cmsis1_pool_slot
{
    osPoolId          id{};
    std::size_t       block_size{};
    std::size_t       block_count{};
    std::atomic<bool> used{};
};
cmsis1_pool_slot cmsis1_pools[OSAL_CMSIS1_MAX_POOLS];
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);

// ---------------------------------------------------------------------------
// Pool helpers
// ---------------------------------------------------------------------------
template<typename T, std::size_t N>
T* slot_acquire(T (&pool)[N]) noexcept
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
void slot_release(T (&pool)[N], T* p) noexcept
{
    (void)pool;
    if (p != nullptr)
    {
        p->used.store(false, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr osPriority osal_to_cmsis1_priority(osal::priority_t p) noexcept
{
    if (p <= 16)
        return osPriorityIdle;
    if (p <= 64)
        return osPriorityLow;
    if (p <= 96)
        return osPriorityBelowNormal;
    if (p <= 160)
        return osPriorityNormal;
    if (p <= 192)
        return osPriorityAboveNormal;
    if (p <= 224)
        return osPriorityHigh;
    return osPriorityRealtime;
}

constexpr uint32_t to_cmsis1_timeout(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
        return osWaitForever;
    return static_cast<uint32_t>(t);
}

// ---------------------------------------------------------------------------
// Timer trampoline
// ---------------------------------------------------------------------------
void cmsis1_timer_callback(void const* arg) noexcept
{
    auto* slot = const_cast<cmsis1_timer_slot*>(static_cast<const cmsis1_timer_slot*>(arg));
    if (slot != nullptr && slot->fn != nullptr)
    {
        if (!slot->auto_reload)
        {
            slot->active.store(false, std::memory_order_release);
        }
        slot->fn(slot->arg);
    }
}

}  // namespace

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(osKernelSysTick()) * 1000LL / osKernelSysTickFrequency;
    }

    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();
    }

    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(osal_clock_monotonic_ms());
    }

    // CMSIS-RTOS v1 timeouts are expressed in milliseconds, so this returns 1000.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return 1000U;  // CMSIS-RTOS v1 timeouts are in ms
    }

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t   priority, osal::affinity_t /*affinity*/, void* /*stack*/,
                                    osal::stack_size_t stack_bytes, const char* /*name*/) noexcept
    {
        if (handle == nullptr || entry == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        auto* slot = slot_acquire(cmsis1_threads);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        osThreadDef_t def = {};
        def.pthread       = reinterpret_cast<os_pthread>(entry);
        def.tpriority     = osal_to_cmsis1_priority(priority);
        def.stacksize     = static_cast<uint32_t>(stack_bytes);
        def.instances     = 0U;

        slot->id = osThreadCreate(&def, arg);
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_threads, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return osal::error_code::not_supported;
    }

    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis1_thread_slot*>(handle->native);
        slot_release(cmsis1_threads, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_thread_slot*>(handle->native);
        const osStatus s    = osThreadSetPriority(slot->id, osal_to_cmsis1_priority(priority));
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
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
        osThreadYield();
    }

    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        osDelay(ms);
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis1_mutexes);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        osMutexDef_t def = {};
        slot->id         = osMutexCreate(&def);
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_mutexes, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis1_mutex_slot*>(handle->native);
        osMutexDelete(slot->id);
        slot_release(cmsis1_mutexes, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_mutex_slot*>(handle->native);
        const osStatus s    = osMutexWait(slot->id, to_cmsis1_timeout(timeout_ticks));
        if (s == osOK)
        {
            return osal::ok();
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            return osal::error_code::would_block;
        }
        return osal::error_code::timeout;
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
        auto*          slot = static_cast<cmsis1_mutex_slot*>(handle->native);
        const osStatus s    = osMutexRelease(slot->id);
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    // ---------------------------------------------------------------------------
    // Semaphore
    // ---------------------------------------------------------------------------

    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        if (handle == nullptr || max_count == 0U || initial_count > max_count ||
            max_count > static_cast<unsigned>(INT32_MAX))
        {
            return osal::error_code::invalid_argument;
        }
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis1_sems);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        osSemaphoreDef_t def = {};
        slot->maximum        = max_count;
        slot->count.store(initial_count, std::memory_order_relaxed);
        slot->id = osSemaphoreCreate(&def, static_cast<int32_t>(initial_count));
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_sems, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis1_sem_slot*>(handle->native);
        if (osSemaphoreDelete(slot->id) != osOK)
        {
            return osal::error_code::unknown;
        }
        slot_release(cmsis1_sems, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*      slot     = static_cast<cmsis1_sem_slot*>(handle->native);
        const auto reserved = slot->reserve();
        if (!reserved.ok())
        {
            return reserved;
        }
        const osStatus s = osSemaphoreRelease(slot->id);
        if (s != osOK)
        {
            slot->count.fetch_sub(1U, std::memory_order_relaxed);
        }
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // CMSIS-RTOS v1 Release is ISR-safe
    }

    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*         slot   = static_cast<cmsis1_sem_slot*>(handle->native);
        const int32_t tokens = osSemaphoreWait(slot->id, to_cmsis1_timeout(timeout_ticks));
        if (tokens > 0)
        {
            slot->count.fetch_sub(1U, std::memory_order_relaxed);
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (osMessageQ)
    // ---------------------------------------------------------------------------

    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        if (handle == nullptr || item_size == 0U || capacity == 0U || capacity > 0xffffffffULL)
        {
            return osal::error_code::invalid_argument;
        }
        if (item_size > sizeof(std::uint32_t))
        {
            return osal::error_code::not_supported;
        }

        auto* slot = cmsis1_queue_acquire(item_size, capacity);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        if (slot->id != nullptr)
        {
            cmsis1_queue_drain(slot);
            handle->native = static_cast<void*>(slot);
            return osal::ok();
        }

        osMessageQDef_t def = {};
        def.queue_sz        = static_cast<uint32_t>(capacity);
        def.item_sz         = static_cast<uint32_t>(item_size);

        slot->id = osMessageCreate(&def, nullptr);
        if (slot->id == nullptr)
        {
            slot->item_size = 0U;
            slot->capacity  = 0U;
            slot->used      = false;
            slot->active.store(false, std::memory_order_release);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    // CMSIS-RTOS v1 has no portable queue-delete operation. The
    // native queue is drained and retained for compatible reuse.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis1_queue_slot*>(handle->native);
        cmsis1_queue_drain(slot);
        slot->active.store(false, std::memory_order_release);
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
        auto* slot = static_cast<cmsis1_queue_slot*>(handle->native);
        if (item == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        uint32_t val = 0U;
        std::memcpy(&val, item, slot->item_size);
        const auto start = osKernelSysTick();
        for (;;)
        {
            // Reserve before publication: a woken receiver must see its count.
            std::size_t count = slot->count.load(std::memory_order_relaxed);
            if (count < slot->capacity &&
                slot->count.compare_exchange_strong(count, count + 1U, std::memory_order_relaxed))
            {
                if (osMessagePut(slot->id, val, 0U) == osOK)
                {
                    return osal::ok();
                }
                slot->count.fetch_sub(1U, std::memory_order_relaxed);
            }
            if (timeout_ticks == osal::NO_WAIT)
            {
                return osal::error_code::would_block;
            }
            const auto elapsed = static_cast<std::uint32_t>(osKernelSysTick() - start);
            if (timeout_ticks != osal::WAIT_FOREVER &&
                static_cast<std::uint64_t>(elapsed) * 1000U / osKernelSysTickFrequency >= timeout_ticks)
            {
                return osal::error_code::timeout;
            }
            if (osDelay(1U) != osOK)
            {
                return osal::error_code::unknown;
            }
        }
    }

    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);  // ISR-safe in CMSIS-RTOS v1
    }

    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis1_queue_slot*>(handle->native);
        if (item == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        osEvent ev = osMessageGet(slot->id, to_cmsis1_timeout(timeout_ticks));
        if (ev.status == osEventMessage)
        {
            uint32_t val = ev.value.v;
            std::memcpy(item, &val, slot->item_size);
            slot->count.fetch_sub(1U, std::memory_order_relaxed);
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* /*handle*/, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        return osal::error_code::not_supported;  // CMSIS-RTOS v1 has no peek
    }

    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot = static_cast<const cmsis1_queue_slot*>(handle->native);
        return slot->count.load(std::memory_order_relaxed);
    }

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot  = static_cast<const cmsis1_queue_slot*>(handle->native);
        const auto  count = slot->count.load(std::memory_order_relaxed);
        return (count >= slot->capacity) ? 0U : (slot->capacity - count);
    }

    // ---------------------------------------------------------------------------
    // Timer (osTimer)
    // ---------------------------------------------------------------------------

    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        if (handle == nullptr || callback == nullptr || period_ticks == 0U || period_ticks == osal::WAIT_FOREVER ||
            period_ticks > UINT32_MAX)
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        auto* slot = slot_acquire(cmsis1_timers);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->fn          = callback;
        slot->arg         = arg;
        slot->period      = period_ticks;
        slot->auto_reload = auto_reload;
        slot->ever_started.store(false, std::memory_order_relaxed);
        slot->active.store(false, std::memory_order_relaxed);

        osTimerDef_t def = {};
        def.ptimer       = cmsis1_timer_callback;

        slot->id = osTimerCreate(&def, auto_reload ? osTimerPeriodic : osTimerOnce, slot);
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_timers, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis1_timer_slot*>(handle->native);
        // CMSIS v1 does not specify a queued-callback reclamation barrier.
        if (slot->ever_started.load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        if (osTimerDelete(slot->id) != osOK)
        {
            return osal::error_code::unknown;
        }
        slot->fn = nullptr;
        slot_release(cmsis1_timers, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis1_timer_slot*>(handle->native);
        slot->ever_started.store(true, std::memory_order_release);
        slot->active.store(true, std::memory_order_release);
        const osStatus s = osTimerStart(slot->id, static_cast<uint32_t>(slot->period));
        if (s != osOK)
        {
            slot->active.store(false, std::memory_order_release);
        }
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_timer_slot*>(handle->native);
        const osStatus s    = osTimerStop(slot->id);
        if (s == osOK)
        {
            slot->active.store(false, std::memory_order_release);
        }
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis1_timer_slot*>(handle->native);
        if (new_period_ticks == 0U || new_period_ticks == osal::WAIT_FOREVER || new_period_ticks > UINT32_MAX)
        {
            return osal::error_code::invalid_argument;
        }
        if (slot->active.load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        slot->period = new_period_ticks;
        // Will take effect on next start
        return osal::ok();
    }

    // CMSIS-RTOS v1 has no osTimerIsRunning; track start/stop and one-shot expiry.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        return handle != nullptr && handle->native != nullptr &&
               static_cast<const cmsis1_timer_slot*>(handle->native)->active.load(std::memory_order_acquire);
    }

    // ---------------------------------------------------------------------------
    // Event flags (emulated — shared OSAL mutex + per-waiter semaphores)
    // ---------------------------------------------------------------------------
#include "../common/emulated_event_flags.inl"

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
    // Memory pool (native — osPool API)
    // ---------------------------------------------------------------------------

    osal::result osal_memory_pool_create(osal::active_traits::memory_pool_handle_t* handle, void* /*buffer*/,
                                         std::size_t /*buf_bytes*/, std::size_t block_size, std::size_t block_count,
                                         const char* /*name*/) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis1_pools);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->block_size  = block_size;
        slot->block_count = block_count;

        osPoolDef_t def = {};
        def.pool_sz     = static_cast<uint32_t>(block_count);
        def.item_sz     = static_cast<uint32_t>(block_size);

        slot->id = osPoolCreate(&def);
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_pools, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    // CMSIS-RTOS v1 has no osPoolDelete; only the tracking slot is freed.
    osal::result osal_memory_pool_destroy(osal::active_traits::memory_pool_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis1_pool_slot*>(handle->native);
        // CMSIS-RTOS v1 has no osPoolDelete; release tracking slot
        slot_release(cmsis1_pools, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    void* osal_memory_pool_allocate(osal::active_traits::memory_pool_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return nullptr;
        }
        auto* slot = static_cast<cmsis1_pool_slot*>(handle->native);
        return osPoolAlloc(slot->id);
    }

    // Timeout is ignored; delegates to osal_memory_pool_allocate().
    void* osal_memory_pool_allocate_timed(osal::active_traits::memory_pool_handle_t* handle,
                                          osal::tick_t /*timeout*/) noexcept
    {
        // CMSIS-RTOS v1 osPoolAlloc is non-blocking only
        return osal_memory_pool_allocate(handle);
    }

    osal::result osal_memory_pool_deallocate(osal::active_traits::memory_pool_handle_t* handle, void* block) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_pool_slot*>(handle->native);
        const osStatus s    = osPoolFree(slot->id, block);
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    std::size_t osal_memory_pool_available(const osal::active_traits::memory_pool_handle_t* /*handle*/) noexcept
    {
        // CMSIS-RTOS v1 has no pool count query
        return 0U;
    }

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
