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
#define OSAL_CH_MAILBOX_DEPTH 16  // per-mailbox message slots

// ---------------------------------------------------------------------------
// Static pools
// ---------------------------------------------------------------------------
namespace
{

mutex_t     ch_mutexes[OSAL_CH_MAX_MUTEXES];
bool        ch_mutex_used[OSAL_CH_MAX_MUTEXES];
semaphore_t ch_sems[OSAL_CH_MAX_SEMS];
bool        ch_sem_used[OSAL_CH_MAX_SEMS];

// ChibiOS thread descriptors
struct ch_thread_slot
{
    thread_t* tp   = nullptr;
    bool      used = false;
};
ch_thread_slot ch_threads[OSAL_CH_MAX_THREADS];

// ChibiOS mailbox (queue) slots
struct ch_mailbox_slot
{
    mailbox_t mb{};
    msg_t     buf[OSAL_CH_MAILBOX_DEPTH]{};
    bool      used = false;
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
    bool                  used        = false;
};
ch_vt_slot ch_timers[OSAL_CH_MAX_TIMERS];

// ChibiOS event source slots (for event flags)
struct ch_event_slot
{
    event_source_t src{};
    eventflags_t   flags = 0U;  // shadow register for get
    bool           used  = false;
};
ch_event_slot ch_events[OSAL_CH_MAX_FLAGS];

// ---------------------------------------------------------------------------
// Generic pool helpers
// ---------------------------------------------------------------------------
template<typename T, std::size_t N>
T* pool_acquire_by_used(T (&pool)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!pool[i].used)
        {
            pool[i].used = true;
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
            pool[i].used = false;
            return;
        }
    }
}

template<typename T, std::size_t N>
T* pool_simple_acquire(T (&pool)[N], bool (&used)[N]) noexcept
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
void pool_simple_release(T (&pool)[N], bool (&used)[N], T* p) noexcept
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

/// @brief Map OSAL priority [0=lowest..255=highest] to ChibiOS [LOWPRIO..HIGHPRIO].
constexpr tprio_t osal_to_ch_priority(osal::priority_t p) noexcept
{
    const tprio_t range = static_cast<tprio_t>(HIGHPRIO - LOWPRIO);
    return LOWPRIO + static_cast<tprio_t>((static_cast<std::uint32_t>(p) * range) / static_cast<std::uint32_t>(255));
}

/// @brief Convert OSAL ticks to ChibiOS sysinterval_t.
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
    if (slot != nullptr && slot->fn != nullptr)
    {
        slot->fn(slot->arg);
    }
    // Auto-reload: re-arm from ISR context
    if (slot != nullptr && slot->auto_reload)
    {
        chSysLockFromISR();
        chVTSetI(vtp, slot->period, ch_vt_callback, arg);
        chSysUnlockFromISR();
    }
}

}  // namespace

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    /// @brief Return monotonic time in milliseconds via chVTGetSystemTimeX() / TIME_I2MS().
    /// @return Milliseconds elapsed since kernel start.
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(TIME_I2MS(chVTGetSystemTimeX()));
    }

    /// @brief Return wall-clock time in milliseconds (aliased to monotonic — no wall clock on ChibiOS).
    /// @return Milliseconds since kernel start.
    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();  // no wall clock on ChibiOS
    }

    /// @brief Return the raw ChibiOS system tick counter.
    /// @return Current chVTGetSystemTimeX() value.
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(chVTGetSystemTimeX());
    }

    /// @brief Return the tick period in microseconds derived from CH_CFG_ST_FREQUENCY.
    /// @return Microseconds per tick.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return static_cast<std::uint32_t>(1'000'000U / CH_CFG_ST_FREQUENCY);
    }

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    /// @brief Create a ChibiOS static thread via chThdCreateStatic().
    /// @param handle      Output handle populated with the ch_thread_slot pointer.
    /// @param entry       Thread entry function.
    /// @param arg         Opaque argument forwarded to @p entry.
    /// @param priority    OSAL priority [0=lowest, 255=highest]; mapped to ChibiOS tprio_t.
    /// @param stack       Pointer to a static stack buffer supplied by the caller.
    /// @param stack_bytes Size of @p stack in bytes.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        assert(handle != nullptr && entry != nullptr && stack != nullptr);
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

    /// @brief Wait for a thread to exit via chThdWait() (or chThdTerminate() if waitexit disabled).
    /// @param handle Thread handle to join.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
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

    /// @brief Detach a thread, releasing its pool slot without waiting for exit.
    /// @param handle Thread handle to detach.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
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

    /// @brief Change the calling thread's priority via chThdSetPriority().
    /// @param handle   Thread handle (used to verify validity; ChibiOS sets current thread's priority).
    /// @param priority New OSAL priority mapped to tprio_t.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_thread_slot*>(handle->native);
        chThdSetPriority(osal_to_ch_priority(priority));
        (void)slot;
        return osal::ok();
    }

    /// @brief Set thread CPU affinity (not supported on ChibiOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Suspend a thread (not supported through this OSAL on ChibiOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended thread (not supported through this OSAL on ChibiOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Yield the current thread's time slice via chThdYield().
    void osal_thread_yield() noexcept
    {
        chThdYield();
    }

    /// @brief Sleep for at least @p ms milliseconds via chThdSleepMilliseconds().
    /// @param ms Delay in milliseconds.
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        chThdSleepMilliseconds(ms);
    }

    // ---------------------------------------------------------------------------
    // Mutex (built-in priority inheritance)
    // ---------------------------------------------------------------------------

    /// @brief Acquire a pool slot and initialise a ChibiOS mutex via chMtxObjectInit().
    /// @param handle Output handle pointing to the initialised mutex_t.
    /// @return osal::ok() on success; osal::error_code::out_of_resources if the pool is full.
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

    /// @brief Release a mutex slot back to the pool.
    /// @details ChibiOS has no mutex destructor; the slot is simply marked unused.
    /// @param handle Mutex handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
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

    /// @brief Acquire a mutex via chMtxLock() (blocking) or chMtxTryLock() (non-blocking).
    /// @details ChibiOS does not provide a timed mutex lock; any finite timeout uses try-lock
    ///          and returns osal::error_code::timeout if the lock cannot be taken immediately.
    /// @param handle        Mutex handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks (only WAIT_FOREVER is truly blocking).
    /// @return osal::ok() if acquired; osal::error_code::would_block or ::timeout otherwise.
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

    /// @brief Try to acquire a mutex without blocking.
    /// @param handle Mutex handle.
    /// @return osal::ok() if acquired; osal::error_code::would_block if unavailable.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release a mutex via chMtxUnlock().
    /// @param handle Mutex handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
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

    /// @brief Acquire a pool slot and initialise a ChibiOS semaphore via chSemObjectInit().
    /// @param handle        Output handle pointing to the initialised semaphore_t.
    /// @param initial_count Initial token count.
    /// @return osal::ok() on success; osal::error_code::out_of_resources if the pool is full.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned /*max_count*/) noexcept
    {
        semaphore_t* s = pool_simple_acquire(ch_sems, ch_sem_used);
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        chSemObjectInit(s, static_cast<cnt_t>(initial_count));
        handle->native = static_cast<void*>(s);
        return osal::ok();
    }

    /// @brief Reset and release a semaphore slot back to the pool.
    /// @param handle Semaphore handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
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

    /// @brief Increment (signal) a semaphore via chSemSignal().
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        chSemSignal(static_cast<semaphore_t*>(handle->native));
        return osal::ok();
    }

    /// @brief Increment a semaphore from ISR context via chSemSignalI() (syscall locked).
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        chSysLockFromISR();
        chSemSignalI(static_cast<semaphore_t*>(handle->native));
        chSysUnlockFromISR();
        return osal::ok();
    }

    /// @brief Decrement (wait on) a semaphore via chSemWaitTimeout().
    /// @param handle        Semaphore handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks (mapped to ChibiOS sysinterval_t).
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout on failure.
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

    /// @brief Try to decrement a semaphore without blocking.
    /// @param handle Semaphore handle.
    /// @return osal::ok() if a token was available; osal::error_code::would_block otherwise.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (ChibiOS Mailbox — pointer-sized messages)
    // ---------------------------------------------------------------------------

    /// @brief Initialise a ChibiOS mailbox slot via chMBObjectInit().
    /// @details The mailbox depth is fixed at OSAL_CH_MAILBOX_DEPTH; item_size and capacity
    ///          parameters are accepted for API compatibility but the mailbox stores msg_t values.
    /// @param handle Output handle pointing to the ch_mailbox_slot.
    /// @return osal::ok() on success; osal::error_code::out_of_resources if no slot is available.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/,
                                   std::size_t /*item_size*/, std::size_t /*capacity*/) noexcept
    {
        auto* slot = pool_acquire_by_used(ch_mailboxes);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        chMBObjectInit(&slot->mb, slot->buf, OSAL_CH_MAILBOX_DEPTH);
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Reset a mailbox and release its slot via chMBReset().
    /// @param handle Queue handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<ch_mailbox_slot*>(handle->native);
        chMBReset(&slot->mb);
        pool_release_by_used(ch_mailboxes, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Post a msg_t-sized message to the mailbox via chMBPostTimeout().
    /// @details The first sizeof(msg_t) bytes of @p item are copied into a msg_t value.
    /// @param handle        Queue handle.
    /// @param item          Pointer to the message data.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the mailbox is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_mailbox_slot*>(handle->native);
        msg_t msg;
        std::memcpy(&msg, item, sizeof(msg_t));
        const msg_t r = chMBPostTimeout(&slot->mb, msg, to_ch_ticks(timeout_ticks));
        if (r == MSG_OK)
        {
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Post a message from ISR context via chMBPostI() (syscall locked).
    /// @param handle Queue handle.
    /// @param item   Pointer to the message data.
    /// @return osal::ok() on success; osal::error_code::would_block if the mailbox is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_mailbox_slot*>(handle->native);
        msg_t msg;
        std::memcpy(&msg, item, sizeof(msg_t));
        chSysLockFromISR();
        const msg_t r = chMBPostI(&slot->mb, msg);
        chSysUnlockFromISR();
        return (r == MSG_OK) ? osal::ok() : osal::error_code::would_block;
    }

    /// @brief Fetch a message from the mailbox via chMBFetchTimeout().
    /// @param handle        Queue handle.
    /// @param item          Buffer to receive the dequeued message into.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the mailbox is empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*       slot = static_cast<ch_mailbox_slot*>(handle->native);
        msg_t       msg;
        const msg_t r = chMBFetchTimeout(&slot->mb, &msg, to_ch_ticks(timeout_ticks));
        if (r == MSG_OK)
        {
            std::memcpy(item, &msg, sizeof(msg_t));
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Fetch a message from ISR context via chMBFetchI() (syscall locked).
    /// @param handle Queue handle.
    /// @param item   Buffer to receive the dequeued message into.
    /// @return osal::ok() on success; osal::error_code::would_block if the mailbox is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_mailbox_slot*>(handle->native);
        msg_t msg;
        chSysLockFromISR();
        const msg_t r = chMBFetchI(&slot->mb, &msg);
        chSysUnlockFromISR();
        if (r == MSG_OK)
        {
            std::memcpy(item, &msg, sizeof(msg_t));
            return osal::ok();
        }
        return osal::error_code::would_block;
    }

    /// @brief Peek at the front message without removing it (not supported on ChibiOS mailbox).
    /// @return osal::error_code::not_supported always.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* /*handle*/, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Return the number of messages in the mailbox via chMBGetUsedCountI().
    /// @param handle Queue handle.
    /// @return Message count, or 0 if the handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot = static_cast<const ch_mailbox_slot*>(handle->native);
        return static_cast<std::size_t>(chMBGetUsedCountI(&slot->mb));
    }

    /// @brief Return the number of free slots in the mailbox via chMBGetFreeCountI().
    /// @param handle Queue handle.
    /// @return Free slot count, or 0 if the handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot = static_cast<const ch_mailbox_slot*>(handle->native);
        return static_cast<std::size_t>(chMBGetFreeCountI(&slot->mb));
    }

    // ---------------------------------------------------------------------------
    // Timer (virtual timer — callback in ISR context)
    // ---------------------------------------------------------------------------

    /// @brief Initialise a ChibiOS virtual timer slot via chVTObjectInit().
    /// @details The timer is created but not started; call osal_timer_start() to arm it.
    /// @param handle       Output handle pointing to the ch_vt_slot.
    /// @param callback     Callback invoked on each expiry (runs in ISR context).
    /// @param arg          Opaque argument forwarded to @p callback.
    /// @param period_ticks Expiry period in OSAL ticks (stored as sysinterval_t).
    /// @param auto_reload  If true, the virtual timer is re-armed inside the callback.
    /// @return osal::ok() on success; osal::error_code::out_of_resources if no slot is available.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr && callback != nullptr);
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
        handle->native    = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Cancel the virtual timer via chVTReset() and release the slot.
    /// @param handle Timer handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<ch_vt_slot*>(handle->native);
        chVTReset(&slot->vt);
        slot->fn = nullptr;
        pool_release_by_used(ch_timers, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Arm the virtual timer via chVTSet().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_vt_slot*>(handle->native);
        chVTSet(&slot->vt, slot->period, ch_vt_callback, slot);
        return osal::ok();
    }

    /// @brief Disarm the virtual timer via chVTReset().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        chVTReset(&static_cast<ch_vt_slot*>(handle->native)->vt);
        return osal::ok();
    }

    /// @brief Stop and immediately restart the virtual timer.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; forwarded error from osal_timer_start().
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Update the stored period; takes effect on next chVTSet().
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
        static_cast<ch_vt_slot*>(handle->native)->period = static_cast<sysinterval_t>(new_period_ticks);
        return osal::ok();
    }

    /// @brief Query whether the virtual timer is currently armed via chVTIsArmed().
    /// @param handle Timer handle.
    /// @return True if the timer is armed; false otherwise.
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

    /// @brief Initialise a ChibiOS event source slot via chEvtObjectInit().
    /// @details A shadow flags register is maintained for osal_event_flags_get().
    /// @param handle Output handle pointing to the ch_event_slot.
    /// @return osal::ok() on success; osal::error_code::out_of_resources if no slot is available.
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

    /// @brief Release the event source slot back to the pool.
    /// @param handle Event-flags handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
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

    /// @brief Set (OR) flags and broadcast via chEvtBroadcastFlags().
    /// @param handle Event-flags handle.
    /// @param bits   Bitmask of flags to set.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_event_slot*>(handle->native);
        slot->flags |= static_cast<eventflags_t>(bits);
        chEvtBroadcastFlags(&slot->src, static_cast<eventflags_t>(bits));
        return osal::ok();
    }

    /// @brief Clear (AND NOT) flags in the shadow register.
    /// @details No broadcast is issued for cleared flags; waiting threads are not woken.
    /// @param handle Event-flags handle.
    /// @param bits   Bitmask of flags to clear.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_event_slot*>(handle->native);
        slot->flags &= ~static_cast<eventflags_t>(bits);
        return osal::ok();
    }

    /// @brief Read the current shadow flags register without modifying it.
    /// @param handle Event-flags handle.
    /// @return Current flag bitmask, or 0 if the handle is invalid.
    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<osal::event_bits_t>(static_cast<const ch_event_slot*>(handle->native)->flags);
    }

    /// @brief Register a listener on the event source and wait for any of @p bits via chEvtWaitAnyTimeout().
    /// @param handle        Event-flags handle.
    /// @param bits          Bitmask of flags to watch.
    /// @param actual        Output for the flags that woke the caller; may be null.
    /// @param clear_on_exit If true, matched flags are cleared in the shadow register on return.
    /// @param timeout       Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout if no matching flags arrived.
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_event_slot*>(handle->native);

        // Register listener on slot's event source
        event_listener_t el;
        chEvtRegisterMask(&slot->src, &el, EVENT_MASK(0));

        eventmask_t evt = chEvtWaitAnyTimeout(EVENT_MASK(0), to_ch_ticks(timeout));
        if (evt == 0U)
        {
            chEvtUnregister(&slot->src, &el);
            return osal::error_code::timeout;
        }
        eventflags_t got = chEvtGetAndClearFlags(&el);
        chEvtUnregister(&slot->src, &el);

        if ((got & static_cast<eventflags_t>(bits)) == 0U)
        {
            return osal::error_code::timeout;
        }
        if (actual != nullptr)
        {
            *actual = static_cast<osal::event_bits_t>(got & static_cast<eventflags_t>(bits));
        }
        if (clear_on_exit)
        {
            slot->flags &= ~(got & static_cast<eventflags_t>(bits));
        }
        return osal::ok();
    }

    /// @brief Register a listener and spin until all bits in @p bits are collected.
    /// @param handle        Event-flags handle.
    /// @param bits          Bitmask of flags that must all be received.
    /// @param actual        Output for the collected flags; may be null.
    /// @param clear_on_exit If true, matched flags are cleared in the shadow register on return.
    /// @param timeout       Maximum wait in OSAL ticks.
    /// @return osal::ok() when all flags have been received; osal::error_code::timeout otherwise.
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*            slot = static_cast<ch_event_slot*>(handle->native);
        event_listener_t el;
        chEvtRegisterMask(&slot->src, &el, EVENT_MASK(0));

        const eventflags_t target    = static_cast<eventflags_t>(bits);
        eventflags_t       collected = 0U;
        const systime_t    deadline  = chVTGetSystemTimeX() + to_ch_ticks(timeout);

        while ((collected & target) != target)
        {
            sysinterval_t remaining = TIME_IMMEDIATE;
            if (timeout == osal::WAIT_FOREVER)
            {
                remaining = TIME_INFINITE;
            }
            else if (timeout != osal::NO_WAIT)
            {
                systime_t now = chVTGetSystemTimeX();
                if (chVTIsSystemTimeWithin(chVTGetSystemTimeX(), deadline))
                {
                    remaining = static_cast<sysinterval_t>(deadline - now);
                }
                else
                {
                    break;
                }
            }
            eventmask_t evt = chEvtWaitAnyTimeout(EVENT_MASK(0), remaining);
            if (evt == 0U)
            {
                break;
            }
            collected |= chEvtGetAndClearFlags(&el);
            if (timeout == osal::NO_WAIT)
            {
                break;
            }
        }
        chEvtUnregister(&slot->src, &el);

        if ((collected & target) == target)
        {
            if (actual != nullptr)
            {
                *actual = static_cast<osal::event_bits_t>(collected & target);
            }
            if (clear_on_exit)
            {
                slot->flags &= ~target;
            }
            return osal::ok();
        }
        return osal::error_code::timeout;
    }

    /// @brief Set flags from ISR context via chEvtBroadcastFlagsI() (syscall locked).
    /// @param handle Event-flags handle.
    /// @param bits   Bitmask of flags to set.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<ch_event_slot*>(handle->native);
        slot->flags |= static_cast<eventflags_t>(bits);
        chSysLockFromISR();
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
        bool                 used = false;
    };
    ch_condvar_slot ch_condvars[OSAL_CH_MAX_CONDVARS];

    }  // namespace

    /// @brief Initialise a ChibiOS condition variable via chCondObjectInit().
    /// @param handle Output handle pointing to the ch_condvar_slot.
    /// @return osal::ok() on success; osal::error_code::out_of_resources if no slot is available.
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

    /// @brief Release the condition variable slot back to the pool.
    /// @param handle Condvar handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
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

    /// @brief Atomically release @p mutex and block on the condition via chCondWaitTimeout().
    /// @details ChibiOS implicitly unlocks the top-of-stack mutex during the wait.
    /// @param handle  Condvar handle.
    /// @param mutex   Mutex that must be held by the caller before this call.
    /// @param timeout Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout if @p timeout expired.
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

    /// @brief Wake one thread waiting on the condition via chCondSignal().
    /// @param handle Condvar handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
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

    /// @brief Wake all threads waiting on the condition via chCondBroadcast().
    /// @param handle Condvar handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
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

    /// @brief Create a wait-set (not supported on ChibiOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Destroy a wait-set (not supported on ChibiOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Add an object to a wait-set (not supported on ChibiOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Remove an object from a wait-set (not supported on ChibiOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait on a wait-set for any registered object to signal (not supported on ChibiOS).
    /// @param n Sets *n to 0.
    /// @return osal::error_code::not_supported always.
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
