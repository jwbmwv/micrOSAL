// SPDX-License-Identifier: Apache-2.0
/// @file chibios_backend.cpp
/// @brief ChibiOS/RT implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to ChibiOS/RT native API:
///          - thread   → chThdCreateStatic / chThdWait / chThdTerminate
///          - mutex    → chMtxInit / chMtxLock / chMtxUnlock (priority inheritance)
///          - semaphore→ chSemInit / chSemWaitTimeout / chSemSignal
///          - queue    → chMBObjectInit / chMBPostTimeout / chMBFetchTimeout (mailbox)
///          - timer    → chVTSet / chVTReset (virtual-timer)
///          - event_flags→ chEvtBroadcastFlags / chEvtWaitAnyTimeout / chEvtWaitAllTimeout
///
///          All kernel objects are held in static pools.
///          Dynamic allocation is never used.
///
///          Includes required: <ch.h> (ChibiOS/RT master header)
///          Build macro:       OSAL_BACKEND_CHIBIOS
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_CHIBIOS
#define OSAL_BACKEND_CHIBIOS
#endif
#include <osal/osal.hpp>

#include <ch.h>  ///< ChibiOS/RT header
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Pool sizes
// ---------------------------------------------------------------------------
#define OSAL_CH_MAX_THREADS 8
#define OSAL_CH_MAX_MUTEXES 16
#define OSAL_CH_MAX_SEMS 16
#define OSAL_CH_MAX_QUEUES 8
#define OSAL_CH_MAX_TIMERS 8
#define OSAL_CH_MAX_FLAGS 8
#ifndef OSAL_CH_MAILBOX_DEPTH
#define OSAL_CH_MAILBOX_DEPTH 16  // maximum queue depth per static slot
#endif

// ---------------------------------------------------------------------------
// Static pools
// ---------------------------------------------------------------------------
namespace
{

mutex_t           ch_mutexes[OSAL_CH_MAX_MUTEXES];
std::atomic<bool> ch_mutex_used[OSAL_CH_MAX_MUTEXES];
semaphore_t       ch_sems[OSAL_CH_MAX_SEMS];
unsigned          ch_sem_max[OSAL_CH_MAX_SEMS];
std::atomic<bool> ch_sem_used[OSAL_CH_MAX_SEMS];

// ChibiOS thread descriptors
struct ch_thread_slot
{
    thread_t*         tp = nullptr;
    std::atomic<bool> used{false};
};
ch_thread_slot ch_threads[OSAL_CH_MAX_THREADS];

// ChibiOS queue slots. Payloads remain in the caller's backing buffer; the
// mailboxes transport free/ready buffer indices rather than payload bytes.
struct ch_mailbox_slot
{
    mailbox_t         ready{};
    mailbox_t         free{};
    msg_t             ready_buffer[OSAL_CH_MAILBOX_DEPTH]{};
    msg_t             free_buffer[OSAL_CH_MAILBOX_DEPTH]{};
    std::uint8_t*     storage   = nullptr;
    std::size_t       item_size = 0U;
    std::size_t       capacity  = 0U;
    std::atomic<bool> used{false};
};
ch_mailbox_slot ch_mailboxes[OSAL_CH_MAX_QUEUES];

// ChibiOS virtual-timer slots
struct ch_vt_slot
{
    virtual_timer_t       vt{};
    osal_timer_callback_t fn          = nullptr;
    void*                 arg         = nullptr;
    sysinterval_t         period      = 0;
    bool                  auto_reload = false;
    std::atomic<bool>     used{false};
    std::atomic<bool>     callback_gate{false};
    std::atomic<bool>     ever_started{false};
    std::atomic<bool>     active{false};
};
ch_vt_slot ch_timers[OSAL_CH_MAX_TIMERS];

// ChibiOS event source slots (for event flags)
struct ch_event_slot
{
    event_source_t    src{};
    eventflags_t      flags = 0U;  // shadow register for get
    std::atomic<bool> used{false};
};
ch_event_slot ch_events[OSAL_CH_MAX_FLAGS];
static_assert(std::atomic<bool>::is_always_lock_free);

struct ch_irq_guard
{
    syssts_t state = chSysGetStatusAndLockX();
    ~ch_irq_guard() { chSysRestoreStatusX(state); }
};

// ---------------------------------------------------------------------------
// Generic pool helpers
// ---------------------------------------------------------------------------
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

template<typename T, std::size_t N>
T* pool_simple_acquire(T (&pool)[N], std::atomic<bool> (&used)[N]) noexcept
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
void pool_simple_release(T (&pool)[N], std::atomic<bool> (&used)[N], T* p) noexcept
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

constexpr tprio_t osal_to_ch_priority(osal::priority_t p) noexcept
{
    const tprio_t range = static_cast<tprio_t>(HIGHPRIO - LOWPRIO);
    return LOWPRIO + static_cast<tprio_t>((static_cast<std::uint32_t>(p) * range) / static_cast<std::uint32_t>(255));
}

constexpr sysinterval_t to_ch_ticks(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return TIME_INFINITE;
    }
    if (t == osal::NO_WAIT)
    {
        return TIME_IMMEDIATE;
    }
    return static_cast<sysinterval_t>(
        t);  // NOLINT(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
}

// ---------------------------------------------------------------------------
// Timer callback (virtual-timer callback runs in ISR context)
// ---------------------------------------------------------------------------
void ch_vt_callback(virtual_timer_t* vtp, void* arg) noexcept
{
    auto* slot = static_cast<ch_vt_slot*>(arg);
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
        chSysLockFromISR();
        chVTSetI(vtp, slot->period, ch_vt_callback, arg);
        chSysUnlockFromISR();
    }
    slot->callback_gate.store(false, std::memory_order_release);
}

osal::result ch_event_wait_impl(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                osal::event_bits_t* actual, bool clear_on_exit, bool all, osal::tick_t timeout) noexcept
{
    if (handle == nullptr || handle->native == nullptr)
    {
        return osal::error_code::not_initialized;
    }
    if (bits == 0U)
    {
        return osal::error_code::invalid_argument;
    }
    auto*           slot   = static_cast<ch_event_slot*>(handle->native);
    const auto      target = static_cast<eventflags_t>(bits);
    const systime_t start  = chVTGetSystemTimeX();
    for (;;)
    {
        {
            ch_irq_guard       guard;
            const eventflags_t got = slot->flags & target;
            if (all ? got == target : got != 0U)
            {
                if (actual != nullptr)
                {
                    *actual = static_cast<osal::event_bits_t>(slot->flags);
                }
                if (clear_on_exit)
                {
                    slot->flags &= ~got;
                }
                return osal::ok();
            }
        }
        if (timeout == osal::NO_WAIT)
        {
            return osal::error_code::would_block;
        }
        if (timeout != osal::WAIT_FOREVER && static_cast<systime_t>(chVTGetSystemTimeX() - start) >= timeout)
        {
            return osal::error_code::timeout;
        }
        chThdSleep(1);
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
        return static_cast<std::int64_t>(chVTGetSystemTimeX()) * 1000LL / CH_CFG_ST_FREQUENCY;
    }

    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();  // no wall clock on ChibiOS
    }

    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(chVTGetSystemTimeX());
    }

    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return static_cast<std::uint32_t>(1'000'000U / CH_CFG_ST_FREQUENCY);
    }

    // ---------------------------------------------------------------------------
    // Thread
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
        auto* slot = pool_acquire_by_used(ch_threads);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        // ChibiOS entry signature: THD_FUNCTION(name, arg) => void name(void *arg)
        slot->tp = chThdCreateStatic(stack, static_cast<size_t>(stack_bytes), osal_to_ch_priority(priority),
                                     reinterpret_cast<tfunc_t>(entry), arg);
        if (slot->tp == nullptr)
        {
            pool_release_by_used(ch_threads, slot);
            return osal::error_code::out_of_resources;
        }
#if CH_CFG_USE_REGISTRY == TRUE
        if (name != nullptr)
        {
            slot->tp->name = name;
        }
#endif
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_thread_slot*>(handle->native);
#if CH_CFG_USE_WAITEXIT == TRUE
        chThdWait(slot->tp);
#else
        chThdTerminate(slot->tp);
#endif
        pool_release_by_used(ch_threads, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        pool_release_by_used(ch_threads, static_cast<ch_thread_slot*>(handle->native));
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
        auto* slot = static_cast<ch_thread_slot*>(handle->native);
        if (slot->tp != chThdGetSelfX())
        {
            return osal::error_code::not_supported;
        }
        chThdSetPriority(osal_to_ch_priority(priority));
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
        chThdYield();
    }

    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        chThdSleepMilliseconds(ms);
    }

    // ---------------------------------------------------------------------------
    // Mutex (built-in priority inheritance)
    // ---------------------------------------------------------------------------

    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        mutex_t* m = pool_simple_acquire(ch_mutexes, ch_mutex_used);
        if (m == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        chMtxObjectInit(m);
        handle->native = static_cast<void*>(m);
        return osal::ok();
    }

    // ChibiOS has no mutex destructor; the slot is simply marked unused.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* m = static_cast<mutex_t*>(handle->native);
        pool_simple_release(ch_mutexes, ch_mutex_used, m);
        handle->native = nullptr;
        return osal::ok();
    }

    // ChibiOS does not provide a timed mutex lock; any finite timeout uses try-lock
    // and returns osal::error_code::timeout if the lock cannot be taken immediately.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* m = static_cast<mutex_t*>(handle->native);

        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            chMtxLock(m);
            return osal::ok();
        }
        // ChibiOS mutexes don't have a timed lock in all configurations;
        // try-lock is the only non-blocking option.
        if (chMtxTryLock(m))
        {
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
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
        chMtxUnlock(static_cast<mutex_t*>(handle->native));
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Semaphore (counting semaphore)
    // ---------------------------------------------------------------------------

    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        if (handle == nullptr || max_count == 0U || initial_count > max_count ||
            max_count > static_cast<unsigned>(std::numeric_limits<cnt_t>::max()))
        {
            return osal::error_code::invalid_argument;
        }
        semaphore_t* s = pool_simple_acquire(ch_sems, ch_sem_used);
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        chSemObjectInit(s, static_cast<cnt_t>(initial_count));
        ch_sem_max[s - ch_sems] = max_count;
        handle->native          = static_cast<void*>(s);
        return osal::ok();
    }

    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* s = static_cast<semaphore_t*>(handle->native);
        chSemReset(s, 0);
        pool_simple_release(ch_sems, ch_sem_used, s);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<semaphore_t*>(handle->native);
        chSysLock();
        if (chSemGetCounterI(s) >= static_cast<cnt_t>(ch_sem_max[s - ch_sems]))
        {
            chSysUnlock();
            return osal::error_code::overflow;
        }
        chSemSignalI(s);
        chSchRescheduleS();
        chSysUnlock();
        return osal::ok();
    }

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        chSysLockFromISR();
        auto* s = static_cast<semaphore_t*>(handle->native);
        if (chSemGetCounterI(s) >= static_cast<cnt_t>(ch_sem_max[s - ch_sems]))
        {
            chSysUnlockFromISR();
            return osal::error_code::overflow;
        }
        chSemSignalI(s);
        chSysUnlockFromISR();
        return osal::ok();
    }

    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const msg_t r = chSemWaitTimeout(static_cast<semaphore_t*>(handle->native), to_ch_ticks(timeout_ticks));
        if (r == MSG_OK)
        {
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (caller storage plus ChibiOS mailbox indices)
    // ---------------------------------------------------------------------------

    // Two native mailboxes transport indices into buffer: one
    // tracks free entries and the other tracks FIFO-ready entries.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        if (handle == nullptr || buffer == nullptr || item_size == 0U || capacity == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        if (capacity > OSAL_CH_MAILBOX_DEPTH)
        {
            return osal::error_code::not_supported;
        }
        auto* slot = pool_acquire_by_used(ch_mailboxes);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        slot->storage   = static_cast<std::uint8_t*>(buffer);
        slot->item_size = item_size;
        slot->capacity  = capacity;
        chMBObjectInit(&slot->ready, slot->ready_buffer, capacity);
        chMBObjectInit(&slot->free, slot->free_buffer, capacity);
        for (std::size_t index = 0U; index < capacity; ++index)
        {
            if (chMBPostTimeout(&slot->free, static_cast<msg_t>(index), TIME_IMMEDIATE) != MSG_OK)
            {
                chMBReset(&slot->ready);
                chMBReset(&slot->free);
                pool_release_by_used(ch_mailboxes, slot);
                return osal::error_code::out_of_resources;
            }
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<ch_mailbox_slot*>(handle->native);
        chMBReset(&slot->ready);
        chMBReset(&slot->free);
        slot->storage   = nullptr;
        slot->item_size = 0U;
        slot->capacity  = 0U;
        pool_release_by_used(ch_mailboxes, slot);
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
        auto*       slot = static_cast<ch_mailbox_slot*>(handle->native);
        msg_t       index_message;
        const msg_t fetch_result = chMBFetchTimeout(&slot->free, &index_message, to_ch_ticks(timeout_ticks));
        if (fetch_result != MSG_OK)
        {
            return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
        }
        const std::size_t index = static_cast<std::size_t>(index_message);
        if (index >= slot->capacity)
        {
            (void)chMBPostTimeout(&slot->free, index_message, TIME_IMMEDIATE);
            return osal::error_code::not_initialized;
        }
        std::memcpy(slot->storage + (index * slot->item_size), item, slot->item_size);
        if (chMBPostTimeout(&slot->ready, index_message, TIME_IMMEDIATE) == MSG_OK)
        {
            return osal::ok();
        }
        (void)chMBPostTimeout(&slot->free, index_message, TIME_IMMEDIATE);
        return osal::error_code::out_of_resources;
    }

    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (item == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        auto* slot = static_cast<ch_mailbox_slot*>(handle->native);
        msg_t index_message;
        chSysLockFromISR();
        const msg_t fetch_result = chMBFetchI(&slot->free, &index_message);
        if (fetch_result == MSG_OK)
        {
            const std::size_t index = static_cast<std::size_t>(index_message);
            if (index < slot->capacity)
            {
                std::memcpy(slot->storage + (index * slot->item_size), item, slot->item_size);
                if (chMBPostI(&slot->ready, index_message) == MSG_OK)
                {
                    chSysUnlockFromISR();
                    return osal::ok();
                }
            }
            (void)chMBPostI(&slot->free, index_message);
        }
        chSysUnlockFromISR();
        return osal::error_code::would_block;
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
        auto*       slot = static_cast<ch_mailbox_slot*>(handle->native);
        msg_t       index_message;
        const msg_t fetch_result = chMBFetchTimeout(&slot->ready, &index_message, to_ch_ticks(timeout_ticks));
        if (fetch_result != MSG_OK)
        {
            return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
        }
        const std::size_t index = static_cast<std::size_t>(index_message);
        if (index >= slot->capacity)
        {
            return osal::error_code::not_initialized;
        }
        std::memcpy(item, slot->storage + (index * slot->item_size), slot->item_size);
        if (chMBPostTimeout(&slot->free, index_message, TIME_IMMEDIATE) == MSG_OK)
        {
            return osal::ok();
        }
        return osal::error_code::out_of_resources;
    }

    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (item == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        auto* slot = static_cast<ch_mailbox_slot*>(handle->native);
        msg_t index_message;
        chSysLockFromISR();
        const msg_t fetch_result = chMBFetchI(&slot->ready, &index_message);
        if (fetch_result == MSG_OK)
        {
            const std::size_t index = static_cast<std::size_t>(index_message);
            if (index < slot->capacity)
            {
                std::memcpy(item, slot->storage + (index * slot->item_size), slot->item_size);
                if (chMBPostI(&slot->free, index_message) == MSG_OK)
                {
                    chSysUnlockFromISR();
                    return osal::ok();
                }
            }
        }
        chSysUnlockFromISR();
        return osal::error_code::would_block;
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
        const auto* slot = static_cast<const ch_mailbox_slot*>(handle->native);
        chSysLock();
        const std::size_t count = static_cast<std::size_t>(chMBGetUsedCountI(&slot->ready));
        chSysUnlock();
        return count;
    }

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot = static_cast<const ch_mailbox_slot*>(handle->native);
        chSysLock();
        const std::size_t count = static_cast<std::size_t>(chMBGetUsedCountI(&slot->free));
        chSysUnlock();
        return count;
    }

    // ---------------------------------------------------------------------------
    // Timer (virtual timer — callback in ISR context)
    // ---------------------------------------------------------------------------

    // The timer is created but not started; call osal_timer_start() to arm it.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        if (handle == nullptr || callback == nullptr || period_ticks == 0U || period_ticks == osal::WAIT_FOREVER ||
            period_ticks > std::numeric_limits<sysinterval_t>::max())
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        auto* slot = pool_acquire_by_used(ch_timers);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        chVTObjectInit(&slot->vt);
        slot->fn          = callback;
        slot->arg         = arg;
        slot->period      = static_cast<sysinterval_t>(period_ticks);
        slot->auto_reload = auto_reload;
        slot->callback_gate.store(false, std::memory_order_release);
        slot->ever_started.store(false, std::memory_order_relaxed);
        slot->active.store(false, std::memory_order_relaxed);
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<ch_vt_slot*>(handle->native);
        // Reset does not establish a portable barrier for already-dispatched callbacks.
        if (slot->ever_started.load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        if (slot->callback_gate.exchange(true, std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        chVTReset(&slot->vt);
        slot->fn          = nullptr;
        slot->auto_reload = false;
        // Keep the gate closed until a subsequent create initializes this slot.
        pool_release_by_used(ch_timers, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_vt_slot*>(handle->native);
        if (slot->ever_started.exchange(true, std::memory_order_acq_rel))
        {
            return osal::error_code::not_supported;
        }
        slot->active = true;
        chVTSet(&slot->vt, slot->period, ch_vt_callback, slot);
        return osal::ok();
    }

    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_vt_slot*>(handle->native);
        if (slot->callback_gate.exchange(true, std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        slot->active = false;
        chVTReset(&slot->vt);
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
        auto* slot = static_cast<ch_vt_slot*>(handle->native);
        if (slot->active.load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        if (new_period_ticks == 0U || new_period_ticks == osal::WAIT_FOREVER ||
            new_period_ticks > std::numeric_limits<sysinterval_t>::max())
        {
            return osal::error_code::invalid_argument;
        }
        if (slot->callback_gate.exchange(true, std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        slot->period = static_cast<sysinterval_t>(new_period_ticks);
        slot->callback_gate.store(false, std::memory_order_release);
        return osal::ok();
    }

    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        return chVTIsArmed(&static_cast<const ch_vt_slot*>(handle->native)->vt);
    }

    // ---------------------------------------------------------------------------
    // Event flags — ChibiOS event source + listener pattern
    // ---------------------------------------------------------------------------
    // ChibiOS events are based on event sources (broadcast) and event listeners.
    // For simplicity, we use the direct thread event flags approach with a
    // shadow flags register stored per OSAL event object.

    // A shadow flags register is maintained for osal_event_flags_get().
    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        auto* slot = pool_acquire_by_used(ch_events);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        chEvtObjectInit(&slot->src);
        slot->flags    = 0U;
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<ch_event_slot*>(handle->native);
        pool_release_by_used(ch_events, slot);
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
        auto*        slot = static_cast<ch_event_slot*>(handle->native);
        ch_irq_guard guard;
        slot->flags |= static_cast<eventflags_t>(bits);
        chEvtBroadcastFlagsI(&slot->src, static_cast<eventflags_t>(bits));
        return osal::ok();
    }

    // No broadcast is issued for cleared flags; waiting threads are not woken.
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*        slot = static_cast<ch_event_slot*>(handle->native);
        ch_irq_guard guard;
        slot->flags &= ~static_cast<eventflags_t>(bits);
        return osal::ok();
    }

    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        ch_irq_guard guard;
        return static_cast<osal::event_bits_t>(static_cast<const ch_event_slot*>(handle->native)->flags);
    }

    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        return ch_event_wait_impl(handle, bits, actual, clear_on_exit, false, timeout);
    }

    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        return ch_event_wait_impl(handle, bits, actual, clear_on_exit, true, timeout);
    }

    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_event_slot*>(handle->native);
        chSysLockFromISR();
        slot->flags |= static_cast<eventflags_t>(bits);
        chEvtBroadcastFlagsI(&slot->src, static_cast<eventflags_t>(bits));
        chSysUnlockFromISR();
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Condition variable (native — ChibiOS condition_variable_t)
    // ---------------------------------------------------------------------------

#define OSAL_CH_MAX_CONDVARS 8

    namespace
    {

    struct ch_condvar_slot
    {
        condition_variable_t cv{};
        std::atomic<bool>    used{false};
    };
    ch_condvar_slot ch_condvars[OSAL_CH_MAX_CONDVARS];

    }  // namespace

    osal::result osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        auto* slot = pool_acquire_by_used(ch_condvars);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        chCondObjectInit(&slot->cv);
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<ch_condvar_slot*>(handle->native);
        pool_release_by_used(ch_condvars, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    // ChibiOS implicitly unlocks the top-of-stack mutex during the wait.
    osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t* handle,
                                   osal::active_traits::mutex_handle_t* mutex, osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr || mutex == nullptr || mutex->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_condvar_slot*>(handle->native);
        // ChibiOS condvar implicitly unlocks/re-locks the top-of-stack mutex.
        // The caller must have locked the OSAL mutex (via chMtxLock) before calling.
        (void)mutex;
        msg_t r;
        if (timeout == osal::WAIT_FOREVER)
        {
            r = chCondWait(&slot->cv);
        }
        else
        {
            r = chCondWaitTimeout(&slot->cv, to_ch_ticks(timeout));
        }
        return (r == MSG_OK) ? osal::ok() : osal::error_code::timeout;
    }

    osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_condvar_slot*>(handle->native);
        chCondSignal(&slot->cv);
        return osal::ok();
    }

    osal::result osal_condvar_notify_all(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_condvar_slot*>(handle->native);
        chCondBroadcast(&slot->cv);
        return osal::ok();
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
