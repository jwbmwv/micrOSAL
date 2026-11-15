// SPDX-License-Identifier: Apache-2.0
/// @file vxworks_backend.cpp
/// @brief VxWorks kernel-mode implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the native VxWorks kernel API:
///          - thread   → taskSpawn / taskDelete
///          - mutex    → semMCreate (SEM_Q_PRIORITY | SEM_INVERSION_SAFE)
///          - semaphore→ semCCreate / semBCreate
///          - queue    → msgQCreate / msgQSend / msgQReceive
///          - timer    → wdCreate / wdStart / wdCancel
///          - event_flags → eventSend / eventReceive (VxWorks native events)
///          - wait_set → select() on fd_set (VxWorks I/O system)
///
///          This backend targets VxWorks kernel mode (RTP applications
///          should use the POSIX backend instead).
///
///          Includes required: <vxWorks.h>, <taskLib.h>, <semLib.h>,
///                             <msgQLib.h>, <wdLib.h>, <sysLib.h>,
///                             <eventLib.h>, <tickLib.h>, <selectLib.h>
///          Build macro:       OSAL_BACKEND_VXWORKS
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_VXWORKS
#define OSAL_BACKEND_VXWORKS
#endif
#include <osal/osal.hpp>

#include <vxWorks.h>
#include <taskLib.h>
#include <semLib.h>
#include <msgQLib.h>
#include <wdLib.h>
#include <sysLib.h>
#include <eventLib.h>
#include <tickLib.h>
#include <selectLib.h>
#include <condVarLib.h>
#include <ctime>
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>

namespace
{
struct vx_sem_slot
{
    std::atomic<bool>     used{};
    std::atomic<SEM_ID>   id{};
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
static_assert(std::atomic<SEM_ID>::is_always_lock_free);
#ifndef OSAL_VX_MAX_SEMS
#define OSAL_VX_MAX_SEMS 64
#endif
vx_sem_slot vx_sems[OSAL_VX_MAX_SEMS];

vx_sem_slot* vx_sem_find(SEM_ID id) noexcept
{
    for (auto& slot : vx_sems)
    {
        if (slot.id.load(std::memory_order_acquire) == id)
        {
            return &slot;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr int osal_to_vx_priority(osal::priority_t p) noexcept
{
    return 255 - static_cast<int>(p);
}

int to_vx_ticks(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return WAIT_FOREVER;
    }
    if (t == osal::NO_WAIT)
    {
        return NO_WAIT;
    }
    // OSAL timeouts already use the same tick unit as tickGet().
    return t > static_cast<osal::tick_t>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max()
                                                                          : static_cast<int>(t);
}

// ---------------------------------------------------------------------------
// Timer callback context
// ---------------------------------------------------------------------------
#define OSAL_VX_MAX_TIMERS 16
#define OSAL_VX_MAX_QUEUES 8

struct vx_timer_ctx
{
    osal_timer_callback_t fn{};
    void*                 arg{};
    WDOG_ID               wd{};
    int                   period_ticks{};  ///< 0 = one-shot
    bool                  auto_reload{};
    std::atomic<bool>     used{};
    std::atomic<bool>     ever_started{};
    std::atomic<bool>     active{};
    std::atomic<bool>     callback_gate{};
};
vx_timer_ctx vx_timer_ctxs[OSAL_VX_MAX_TIMERS];

struct vx_queue_slot
{
    MSG_Q_ID          id{};
    std::size_t       item_size{};
    std::size_t       capacity{};
    std::atomic<bool> used{};
};
vx_queue_slot vx_queues[OSAL_VX_MAX_QUEUES];
static_assert(std::atomic<bool>::is_always_lock_free);

vx_queue_slot* vx_queue_acquire() noexcept
{
    for (auto& queue : vx_queues)
    {
        if (!queue.used.exchange(true, std::memory_order_acquire))
        {
            return &queue;
        }
    }
    return nullptr;
}

void vx_queue_release(vx_queue_slot* queue) noexcept
{
    if (queue == nullptr)
    {
        return;
    }

    queue->id        = nullptr;
    queue->item_size = 0U;
    queue->capacity  = 0U;
    queue->used.store(false, std::memory_order_release);
}

void vx_timer_expiry(long ctx_idx) noexcept
{
    auto& ctx = vx_timer_ctxs[ctx_idx];
    if (ctx.callback_gate.exchange(true, std::memory_order_acquire))
    {
        return;
    }
    if (!ctx.active.load(std::memory_order_acquire))
    {
        ctx.callback_gate.store(false, std::memory_order_release);
        return;
    }
    if (!ctx.auto_reload)
    {
        ctx.active.store(false, std::memory_order_release);
    }
    if (ctx.fn != nullptr)
    {
        ctx.fn(ctx.arg);
    }
    if (ctx.auto_reload && ctx.wd != nullptr && ctx.period_ticks > 0)
    {
        wdStart(ctx.wd, ctx.period_ticks, reinterpret_cast<FUNCPTR>(vx_timer_expiry), ctx_idx);
    }
    ctx.callback_gate.store(false, std::memory_order_release);
}

}  // namespace

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        struct timespec ts
        {
        };
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::int64_t>(ts.tv_sec) * 1000LL + static_cast<std::int64_t>(ts.tv_nsec) / 1'000'000LL;
    }

    std::int64_t osal_clock_system_ms() noexcept
    {
        struct timespec ts
        {
        };
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<std::int64_t>(ts.tv_sec) * 1000LL + static_cast<std::int64_t>(ts.tv_nsec) / 1'000'000LL;
    }

    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(tickGet());
    }

    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        const int rate = sysClkRateGet();
        return (rate > 0) ? static_cast<std::uint32_t>(1'000'000U / static_cast<unsigned>(rate)) : 1000U;
    }

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t   priority, osal::affinity_t /*affinity*/, void* /*stack*/,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        if (handle == nullptr || entry == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        const int tid = taskSpawn(const_cast<char*>(name != nullptr ? name : "osal"), osal_to_vx_priority(priority), 0,
                                  static_cast<int>(stack_bytes), reinterpret_cast<FUNCPTR>(entry),
                                  reinterpret_cast<_Vx_usr_arg_t>(arg), 0, 0, 0, 0, 0, 0, 0, 0, 0);
        if (tid == ERROR)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = reinterpret_cast<void*>(static_cast<intptr_t>(tid));
        return osal::ok();
    }

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        // VxWorks does not have taskJoin; delete the task.
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int tid = static_cast<int>(reinterpret_cast<intptr_t>(handle->native));
        taskDelete(tid);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle != nullptr)
        {
            handle->native = nullptr;
        }
        return osal::ok();
    }

    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int tid = static_cast<int>(reinterpret_cast<intptr_t>(handle->native));
        return (taskPrioritySet(tid, osal_to_vx_priority(priority)) == OK) ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
    {
        // VxWorks SMP supports taskCpuAffinitySet, but not all configs.
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
        taskDelay(0);
    }

    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        const int rate  = sysClkRateGet();
        int       ticks = static_cast<int>((static_cast<long long>(ms) * rate) / 1000LL);
        if (ticks <= 0)
        {
            ticks = 1;
        }
        taskDelay(ticks);
    }

    // ---------------------------------------------------------------------------
    // Mutex (semMCreate — priority inheritance + recursive)
    // ---------------------------------------------------------------------------

    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        // semMCreate with SEM_INVERSION_SAFE enables priority inheritance.
        // SEM_Q_PRIORITY + SEM_DELETE_SAFE + SEM_INVERSION_SAFE.
        SEM_ID sem = semMCreate(SEM_Q_PRIORITY | SEM_INVERSION_SAFE | SEM_DELETE_SAFE);
        if (sem == SEM_ID_NULL)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(sem);
        return osal::ok();
    }

    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        semDelete(static_cast<SEM_ID>(handle->native));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const STATUS rc = semTake(static_cast<SEM_ID>(handle->native), to_vx_ticks(timeout_ticks));
        return (rc == OK) ? osal::ok() : osal::error_code::timeout;
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
        return (semGive(static_cast<SEM_ID>(handle->native)) == OK) ? osal::ok() : osal::error_code::not_owner;
    }

    // ---------------------------------------------------------------------------
    // Semaphore (semCCreate — counting semaphore)
    // ---------------------------------------------------------------------------

    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        if (handle == nullptr || max_count == 0U || initial_count > max_count ||
            max_count > static_cast<unsigned>(std::numeric_limits<int>::max()))
        {
            return osal::error_code::invalid_argument;
        }
        vx_sem_slot* slot = nullptr;
        for (auto& candidate : vx_sems)
        {
            if (!candidate.used.exchange(true, std::memory_order_acquire))
            {
                slot = &candidate;
                break;
            }
        }
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        SEM_ID sem = semCCreate(SEM_Q_PRIORITY, static_cast<int>(initial_count));
        if (sem == SEM_ID_NULL)
        {
            slot->used.store(false, std::memory_order_release);
            return osal::error_code::out_of_resources;
        }
        slot->maximum = max_count;
        slot->count.store(initial_count, std::memory_order_relaxed);
        slot->id.store(sem, std::memory_order_release);
        handle->native = static_cast<void*>(sem);
        return osal::ok();
    }

    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = vx_sem_find(static_cast<SEM_ID>(handle->native));
        if (slot == nullptr || semDelete(static_cast<SEM_ID>(handle->native)) != OK)
        {
            return osal::error_code::unknown;
        }
        slot->id.store(SEM_ID_NULL, std::memory_order_release);
        slot->used.store(false, std::memory_order_release);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = vx_sem_find(static_cast<SEM_ID>(handle->native));
        if (slot == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const auto reserved = slot->reserve();
        if (!reserved.ok())
        {
            return reserved;
        }
        if (semGive(static_cast<SEM_ID>(handle->native)) != OK)
        {
            slot->count.fetch_sub(1U, std::memory_order_relaxed);
            return osal::error_code::unknown;
        }
        return osal::ok();
    }

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // semGive is ISR-safe in VxWorks
    }

    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const STATUS rc = semTake(static_cast<SEM_ID>(handle->native), to_vx_ticks(timeout_ticks));
        if (rc == OK)
        {
            vx_sem_find(static_cast<SEM_ID>(handle->native))->count.fetch_sub(1U, std::memory_order_relaxed);
        }
        return (rc == OK) ? osal::ok() : osal::error_code::timeout;
    }

    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (msgQCreate)
    // ---------------------------------------------------------------------------

    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        if (handle == nullptr || item_size == 0U || capacity == 0U ||
            item_size > static_cast<std::size_t>(std::numeric_limits<UINT>::max()) ||
            capacity > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            return osal::error_code::invalid_argument;
        }

        auto* slot = vx_queue_acquire();
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        // VxWorks msgQ allocates its native payload storage internally; the
        // wrapper slot keeps the requested item geometry for every operation.
        MSG_Q_ID q = msgQCreate(static_cast<int>(capacity), static_cast<int>(item_size), MSG_Q_FIFO);
        if (q == MSG_Q_ID_NULL)
        {
            vx_queue_release(slot);
            return osal::error_code::out_of_resources;
        }
        slot->id        = q;
        slot->item_size = item_size;
        slot->capacity  = capacity;
        handle->native  = static_cast<void*>(slot);
        return osal::ok();
    }

    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<vx_queue_slot*>(handle->native);
        (void)msgQDelete(slot->id);
        vx_queue_release(slot);
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
        auto*        slot = static_cast<vx_queue_slot*>(handle->native);
        const STATUS rc   = msgQSend(slot->id, const_cast<char*>(static_cast<const char*>(item)),
                                     static_cast<UINT>(slot->item_size), to_vx_ticks(timeout_ticks), MSG_PRI_NORMAL);
        return (rc == OK) ? osal::ok() : osal::error_code::timeout;
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
        auto*     slot = static_cast<vx_queue_slot*>(handle->native);
        const int rc   = msgQReceive(slot->id, static_cast<char*>(item), static_cast<UINT>(slot->item_size),
                                     to_vx_ticks(timeout_ticks));
        return (rc != ERROR) ? osal::ok() : osal::error_code::timeout;
    }

    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;  // VxWorks msgQ has no peek
    }

    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot  = static_cast<const vx_queue_slot*>(handle->native);
        const int   count = msgQNumMsgs(slot->id);
        return (count > 0) ? static_cast<std::size_t>(count) : 0U;
    }

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot  = static_cast<const vx_queue_slot*>(handle->native);
        const int   count = msgQNumMsgs(slot->id);
        if (count <= 0)
        {
            return slot->capacity;
        }
        const std::size_t used = static_cast<std::size_t>(count);
        return (used >= slot->capacity) ? 0U : (slot->capacity - used);
    }

    // ---------------------------------------------------------------------------
    // Timer (watchdog — wdCreate / wdStart / wdCancel)
    // ---------------------------------------------------------------------------

    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        if (handle == nullptr || callback == nullptr || period_ticks == 0U || period_ticks == osal::WAIT_FOREVER ||
            period_ticks > static_cast<osal::tick_t>(std::numeric_limits<int>::max()))
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        // Find a free context slot.
        long idx = -1;
        for (long i = 0; i < OSAL_VX_MAX_TIMERS; ++i)
        {
            if (!vx_timer_ctxs[i].used.exchange(true, std::memory_order_acquire))
            {
                idx = i;
                break;
            }
        }
        if (idx < 0)
        {
            return osal::error_code::out_of_resources;
        }

        WDOG_ID wd = wdCreate();
        if (wd == nullptr)
        {
            vx_timer_ctxs[idx].used.store(false, std::memory_order_release);
            return osal::error_code::out_of_resources;
        }

        auto& ctx        = vx_timer_ctxs[idx];
        ctx.fn           = callback;
        ctx.arg          = arg;
        ctx.wd           = wd;
        ctx.auto_reload  = auto_reload;
        ctx.period_ticks = to_vx_ticks(period_ticks);
        ctx.ever_started.store(false, std::memory_order_relaxed);
        ctx.active.store(false, std::memory_order_relaxed);
        ctx.callback_gate.store(false, std::memory_order_relaxed);

        handle->native = reinterpret_cast<void*>(static_cast<intptr_t>(idx + 1));
        return osal::ok();
    }

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native)) - 1;
        if (idx >= 0 && idx < OSAL_VX_MAX_TIMERS)
        {
            auto& ctx = vx_timer_ctxs[idx];
            // Watchdog deletion is not a verified SMP callback lifetime barrier.
            if (ctx.ever_started.load(std::memory_order_acquire))
            {
                return osal::error_code::not_supported;
            }
            if (ctx.wd != nullptr)
            {
                wdCancel(ctx.wd);
                wdDelete(ctx.wd);
            }
            ctx.fn = nullptr;
            ctx.wd = nullptr;
            ctx.used.store(false, std::memory_order_release);
        }
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native)) - 1;
        if (idx < 0 || idx >= OSAL_VX_MAX_TIMERS || vx_timer_ctxs[idx].wd == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto& ctx = vx_timer_ctxs[idx];
        if (ctx.ever_started.exchange(true, std::memory_order_acq_rel))
        {
            return osal::error_code::not_supported;
        }
        ctx.active.store(true, std::memory_order_release);
        const STATUS rc = wdStart(ctx.wd, ctx.period_ticks, reinterpret_cast<FUNCPTR>(vx_timer_expiry), idx);
        if (rc != OK)
        {
            ctx.active.store(false, std::memory_order_release);
        }
        return (rc == OK) ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native)) - 1;
        if (idx < 0 || idx >= OSAL_VX_MAX_TIMERS || vx_timer_ctxs[idx].wd == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto& ctx = vx_timer_ctxs[idx];
        if (ctx.callback_gate.exchange(true, std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        ctx.active.store(false, std::memory_order_release);
        const STATUS rc = wdCancel(ctx.wd);
        ctx.callback_gate.store(false, std::memory_order_release);
        if (rc != OK)
        {
            return osal::error_code::unknown;
        }
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
        if (handle == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native)) - 1;
        if (idx < 0 || idx >= OSAL_VX_MAX_TIMERS)
        {
            return osal::error_code::not_initialized;
        }
        if (vx_timer_ctxs[idx].active.load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        if (new_period_ticks == 0U || new_period_ticks == osal::WAIT_FOREVER ||
            new_period_ticks > static_cast<osal::tick_t>(std::numeric_limits<int>::max()))
        {
            return osal::error_code::invalid_argument;
        }
        if (vx_timer_ctxs[idx].callback_gate.exchange(true, std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        vx_timer_ctxs[idx].period_ticks = to_vx_ticks(new_period_ticks);
        vx_timer_ctxs[idx].callback_gate.store(false, std::memory_order_release);
        return osal::ok();
    }

    // Note: A shadow flag tracks explicit start/stop and one-shot expiry.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return false;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native)) - 1;
        return idx >= 0 && idx < OSAL_VX_MAX_TIMERS && vx_timer_ctxs[idx].active.load(std::memory_order_acquire);
    }

    // ---------------------------------------------------------------------------
    // Event flags (emulated — shared OSAL mutex + per-waiter semaphores)
    // ---------------------------------------------------------------------------
#include "../common/emulated_event_flags.inl"

    // ---------------------------------------------------------------------------
    // Wait-set (select() on VxWorks I/O)
    // ---------------------------------------------------------------------------

    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        (void)handle;
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
    // Condition variable (native — VxWorks condVarLib)
    // ---------------------------------------------------------------------------

    osal::result osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        COND_VAR_ID cv = condVarCreate(0);
        if (cv == NULL)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(cv);
        return osal::ok();
    }

    osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        condVarDelete(static_cast<COND_VAR_ID>(handle->native));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t* handle,
                                   osal::active_traits::mutex_handle_t* mutex, osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr || mutex == nullptr || mutex->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        COND_VAR_ID  cv  = static_cast<COND_VAR_ID>(handle->native);
        SEM_ID       mtx = static_cast<SEM_ID>(mutex->native);
        const STATUS rc  = condVarWait(cv, mtx, to_vx_ticks(timeout));
        return (rc == OK) ? osal::ok() : osal::error_code::timeout;
    }

    osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        condVarSignal(static_cast<COND_VAR_ID>(handle->native));
        return osal::ok();
    }

    osal::result osal_condvar_notify_all(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        condVarBroadcast(static_cast<COND_VAR_ID>(handle->native));
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Work queue (emulated fallback — TODO: replace with native jobQueueLib)
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
