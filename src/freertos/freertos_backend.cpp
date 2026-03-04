// SPDX-License-Identifier: Apache-2.0
/// @file freertos_backend.cpp
/// @brief FreeRTOS implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the corresponding FreeRTOS API.
///
///          Minimum required version: FreeRTOS 10.x
///          The backend uses static allocation APIs (xTaskCreateStatic,
///          xSemaphoreCreateMutexStatic, xEventGroupCreateStatic, etc.)
///          that require configSUPPORT_STATIC_ALLOCATION == 1.  These APIs
///          are stable across the entire 10.x and 11.x series.
///
///          FreeRTOS header requirements (ensure your include path provides):
///          - FreeRTOS.h
///          - task.h, semphr.h, queue.h, timers.h, event_groups.h
///
///          Static allocation strategy:
///          - All primitives use the *Static family (xTaskCreateStatic,
///            xSemaphoreCreateMutexStatic, etc.).  This requires
///            configSUPPORT_STATIC_ALLOCATION == 1 in FreeRTOSConfig.h.
///          - Dynamic allocation paths are provided under
///            OSAL_FREERTOS_DYNAMIC_ALLOC for completeness but are NOT
///            recommended for MISRA/safety use.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_FREERTOS
#define OSAL_BACKEND_FREERTOS
#endif
#include <osal/osal.hpp>

// Pull in FreeRTOS headers.  These must be on the include path.
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "timers.h"
#include "event_groups.h"
#include "stream_buffer.h"
#include "message_buffer.h"

#include <cassert>
#include <cstring>
#include <new>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// @brief Converts OSAL priority (0–255) to FreeRTOS priority.
/// @param p OSAL priority value in the range [PRIORITY_LOWEST, PRIORITY_HIGHEST].
/// @return Equivalent FreeRTOS priority in [0, configMAX_PRIORITIES-1].
static constexpr UBaseType_t osal_to_freertos_priority(osal::priority_t p) noexcept
{
    // FreeRTOS priority range: 0 (idle) to configMAX_PRIORITIES-1.
    constexpr osal::priority_t OSAL_MAX = osal::PRIORITY_HIGHEST;
    const UBaseType_t          fr_max   = static_cast<UBaseType_t>(configMAX_PRIORITIES - 1U);
    return static_cast<UBaseType_t>((static_cast<std::uint32_t>(p) * fr_max) / static_cast<std::uint32_t>(OSAL_MAX));
}

/// @brief Converts an OSAL tick count to a FreeRTOS @c TickType_t, mapping @c WAIT_FOREVER to @c portMAX_DELAY.
/// @param t OSAL tick count or @c osal::WAIT_FOREVER.
/// @return Equivalent @c TickType_t for use with FreeRTOS blocking APIs.
static constexpr TickType_t to_freertos_ticks(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return portMAX_DELAY;
    }
    return static_cast<TickType_t>(t);
}

// ---------------------------------------------------------------------------
// Timer callback pair pool (used in both static and dynamic allocation modes)
// ---------------------------------------------------------------------------
#ifndef OSAL_FREERTOS_MAX_TIMERS
#define OSAL_FREERTOS_MAX_TIMERS 8
#endif

namespace
{
struct fr_cb_pair
{
    osal_timer_callback_t fn{nullptr};
    void*                 a{nullptr};
};
static fr_cb_pair fr_cb_pairs[OSAL_FREERTOS_MAX_TIMERS];
}  // namespace

// ---------------------------------------------------------------------------
// Static kernel-object pools (FreeRTOS static allocation)
// ---------------------------------------------------------------------------
// These pools allow multiple concurrent objects to be created without heap.
// Adjust pool sizes via macros before including this file if needed.

#if !defined(OSAL_FREERTOS_DYNAMIC_ALLOC)

#ifndef OSAL_FREERTOS_MAX_MUTEXES
#define OSAL_FREERTOS_MAX_MUTEXES 16
#endif
#ifndef OSAL_FREERTOS_MAX_SEMAPHORES
#define OSAL_FREERTOS_MAX_SEMAPHORES 16
#endif
#ifndef OSAL_FREERTOS_MAX_TIMERS
#define OSAL_FREERTOS_MAX_TIMERS 8
#endif
#ifndef OSAL_FREERTOS_MAX_EVENT_GROUPS
#define OSAL_FREERTOS_MAX_EVENT_GROUPS 8
#endif
#ifndef OSAL_FREERTOS_MAX_STREAM_BUFFERS
#define OSAL_FREERTOS_MAX_STREAM_BUFFERS 8
#endif
#ifndef OSAL_FREERTOS_MAX_MESSAGE_BUFFERS
#define OSAL_FREERTOS_MAX_MESSAGE_BUFFERS 8
#endif

static StaticSemaphore_t fr_mutex_pool[OSAL_FREERTOS_MAX_MUTEXES];
static bool              fr_mutex_used[OSAL_FREERTOS_MAX_MUTEXES];

static StaticSemaphore_t fr_sem_pool[OSAL_FREERTOS_MAX_SEMAPHORES];
static bool              fr_sem_used[OSAL_FREERTOS_MAX_SEMAPHORES];

static StaticTimer_t fr_timer_pool[OSAL_FREERTOS_MAX_TIMERS];
static bool          fr_timer_used[OSAL_FREERTOS_MAX_TIMERS];

static StaticEventGroup_t fr_event_pool[OSAL_FREERTOS_MAX_EVENT_GROUPS];
static bool               fr_event_used[OSAL_FREERTOS_MAX_EVENT_GROUPS];

static StaticStreamBuffer_t fr_sbuf_pool[OSAL_FREERTOS_MAX_STREAM_BUFFERS];
static bool                 fr_sbuf_used[OSAL_FREERTOS_MAX_STREAM_BUFFERS];

// Note: MessageBufferHandle_t == StreamBufferHandle_t in FreeRTOS; we use a
// separate pool of StaticStreamBuffer_t for message buffers so the pool slots
// remain disjoint (prevents accidental aliasing between the two object types).
static StaticStreamBuffer_t fr_mbuf_pool[OSAL_FREERTOS_MAX_MESSAGE_BUFFERS];
static bool                 fr_mbuf_used[OSAL_FREERTOS_MAX_MESSAGE_BUFFERS];

/// @brief Acquire a free slot from a static pool.
template<typename T, std::size_t N>
static T* fr_pool_acquire(T (&pool)[N], bool (&used)[N]) noexcept
{
    for (std::size_t i = 0U; i < N; ++i)
    {
        if (!used[i])
        {
            used[i] = true;
            return &pool[i];
        }
    }
    return nullptr;
}

/// @brief Release a slot back to a static pool.
template<typename T, std::size_t N>
static void fr_pool_release(T (&pool)[N], bool (&used)[N], T* p) noexcept
{
    for (std::size_t i = 0U; i < N; ++i)
    {
        if (&pool[i] == p)
        {
            used[i] = false;
            return;
        }
    }
}

#endif  // !OSAL_FREERTOS_DYNAMIC_ALLOC

// ---------------------------------------------------------------------------
// Thread join context (dynamic-alloc path only)
// ---------------------------------------------------------------------------
/// @brief Per-thread context allocated on the heap for join-capable tasks.
struct fr_thread_ctx_t
{
    void (*user_entry)(void*){nullptr};   ///< Original OSAL entry function.
    void*             user_arg{nullptr};  ///< Original argument.
    SemaphoreHandle_t done_sem{nullptr};  ///< Signalled when entry returns.
};

/// @brief FreeRTOS task body: runs user_entry then signals done_sem.
static void fr_thread_wrapper_fn(void* arg) noexcept
{
    auto* ctx = static_cast<fr_thread_ctx_t*>(arg);
    ctx->user_entry(ctx->user_arg);
    xSemaphoreGive(ctx->done_sem);
    vTaskDelete(nullptr);  // self-delete
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

extern "C"
{
    /// @brief Returns the monotonic tick-based time in milliseconds since scheduler start.
    /// @details Uses @c xTaskGetTickCount() scaled by @c configTICK_RATE_HZ.
    /// @return Milliseconds since the FreeRTOS scheduler started.
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        const TickType_t ticks = xTaskGetTickCount();
        return static_cast<std::int64_t>(ticks) * (1000 / configTICK_RATE_HZ);
    }

    /// @brief Returns the wall-clock time in milliseconds.
    /// @details FreeRTOS has no real-time wall clock; delegates to @c osal_clock_monotonic_ms().
    /// @return Same value as @c osal_clock_monotonic_ms().
    std::int64_t osal_clock_system_ms() noexcept
    {
        // FreeRTOS does not have a system (wall) clock; fall back to monotonic.
        return osal_clock_monotonic_ms();
    }

    /// @brief Returns the current FreeRTOS tick count cast to @c osal::tick_t.
    /// @return Current scheduler tick count.
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(xTaskGetTickCount());
    }

    /// @brief Returns the duration of one scheduler tick in microseconds.
    /// @details Computed as @c 1,000,000 / @c configTICK_RATE_HZ.
    /// @return Tick period in microseconds.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return static_cast<std::uint32_t>(1'000'000U / configTICK_RATE_HZ);
    }

    // ---------------------------------------------------------------------------
    // Thread (Task)
    // ---------------------------------------------------------------------------

    /// @brief Creates and starts a new RTOS task.
    /// @details On the static-alloc path the @p stack buffer must be large enough to hold a
    ///          @c StaticTask_t followed by the actual stack words.  On the dynamic-alloc path
    ///          (@c OSAL_FREERTOS_DYNAMIC_ALLOC) a heap-allocated join context is created and
    ///          the provided @p stack is ignored.
    /// @param handle   Output handle populated on success.
    /// @param entry    Task entry function; must not return (or return is caught by wrapper).
    /// @param arg      Passed verbatim to @p entry.
    /// @param priority OSAL priority in [PRIORITY_LOWEST, PRIORITY_HIGHEST].
    /// @param stack       Pointer to caller-supplied stack/TCB storage (ignored with dynamic alloc).
    /// @param stack_bytes Total byte size of @p stack (must be >= @c default_stack_bytes).
    /// @param name     Optional null-terminated task name (may be @c nullptr).
    /// @return @c osal::ok() on success; @c error_code::out_of_resources if the kernel object
    ///         could not be allocated.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority,
                                    osal::affinity_t /* affinity — not supported in single-core FreeRTOS */,
                                    void* stack, osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        assert(handle != nullptr);
        assert(entry != nullptr);
        assert(stack != nullptr);
        assert(stack_bytes >= osal::active_traits::default_stack_bytes);

        // FreeRTOS static TCB storage — caller must provide stack only; we
        // keep the StaticTask_t inside the handle padding.  We use a side-channel
        // in the handle payload to store the TCB pointer for simplicity.
        //
        // NOTE: In a real integration the StaticTask_t must outlive the task.
        // Here we require the caller to provide a StaticTask_t via 'arg' layout
        // or extend thread_handle_t.  For clarity we use dynamic task creation
        // gated on OSAL_FREERTOS_DYNAMIC_ALLOC (acceptable for host/sim builds).

        const UBaseType_t   fr_prio     = osal_to_freertos_priority(priority);
        const std::uint32_t stack_words = static_cast<std::uint32_t>(stack_bytes / sizeof(StackType_t));

#if defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
        // Allocate a join context so join() can wait for task completion.
        auto* ctx = new (std::nothrow) fr_thread_ctx_t{};
        if (ctx == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        ctx->user_entry = entry;
        ctx->user_arg   = arg;
        ctx->done_sem   = xSemaphoreCreateBinary();
        if (ctx->done_sem == nullptr)
        {
            delete ctx;
            return osal::error_code::out_of_resources;
        }
        TaskHandle_t     th = nullptr;
        const BaseType_t rc = xTaskCreate(fr_thread_wrapper_fn, (name != nullptr) ? name : "osal",
                                          static_cast<configSTACK_DEPTH_TYPE>(stack_words), ctx, fr_prio, &th);
        if (rc != pdPASS || th == nullptr)
        {
            vSemaphoreDelete(ctx->done_sem);
            delete ctx;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(ctx);
        return osal::ok();
#else
        // Static allocation path.
        auto* tcb = static_cast<StaticTask_t*>(stack);  // first sizeof(StaticTask_t) bytes
        auto* stk = reinterpret_cast<StackType_t*>(static_cast<std::uint8_t*>(stack) + sizeof(StaticTask_t));
        const std::uint32_t real_words = (stack_bytes - sizeof(StaticTask_t)) / sizeof(StackType_t);

        TaskHandle_t th = xTaskCreateStatic(reinterpret_cast<TaskFunction_t>(entry), (name != nullptr) ? name : "osal",
                                            real_words, arg, fr_prio, stk, tcb);
        if (th == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(th);
        return osal::ok();
#endif
    }

    /// @brief Waits for a task to finish and releases its resources.
    /// @details With @c OSAL_FREERTOS_DYNAMIC_ALLOC the call blocks until the task's done
    ///          semaphore is signalled, then frees the join context.  On the static-alloc path
    ///          the task is deleted immediately (no blocking join).
    /// @param handle Handle of the task to join; set to null on return.
    /// @return Always @c osal::ok().
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle,
                                  osal::tick_t /* timeout_ticks */) noexcept
    {
        if (handle != nullptr && handle->native != nullptr)
        {
#if defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
            // Wait for the wrapper task to signal completion, then clean up.
            auto* ctx = static_cast<fr_thread_ctx_t*>(handle->native);
            xSemaphoreTake(ctx->done_sem, portMAX_DELAY);
            vSemaphoreDelete(ctx->done_sem);
            delete ctx;
#else
            // No join support in static-alloc path — just delete the task.
            vTaskDelete(static_cast<TaskHandle_t>(handle->native));
#endif
            handle->native = nullptr;
        }
        return osal::ok();
    }

    /// @brief Detaches a task handle so the task manages its own lifecycle.
    /// @details Clears the native handle field; the underlying FreeRTOS task must call
    ///          @c vTaskDelete(NULL) when it finishes.
    /// @param handle Handle to detach; native field set to @c nullptr.
    /// @return Always @c osal::ok().
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        // In FreeRTOS, tasks manage their own lifecycle via vTaskDelete(NULL).
        if (handle != nullptr)
        {
            handle->native = nullptr;
        }
        return osal::ok();
    }

    /// @brief Changes the priority of a running task.
    /// @param handle   Handle of the target task.
    /// @param priority New OSAL priority; converted via @c osal_to_freertos_priority().
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        vTaskPrioritySet(static_cast<TaskHandle_t>(handle->native), osal_to_freertos_priority(priority));
        return osal::ok();
    }

    /// @brief Sets the CPU affinity of a task.
    /// @note  Single-core FreeRTOS does not support affinity; always returns
    ///        @c error_code::not_supported.
    /// @return @c error_code::not_supported.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /* handle */,
                                          osal::affinity_t /* affinity */) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Suspends a task, preventing it from being scheduled.
    /// @details Requires @c INCLUDE_vTaskSuspend == 1 in @c FreeRTOSConfig.h.
    /// @param handle Handle of the task to suspend.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null;
    ///         @c error_code::not_supported if @c INCLUDE_vTaskSuspend is disabled.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
#if defined(INCLUDE_vTaskSuspend) && (INCLUDE_vTaskSuspend == 1)
        vTaskSuspend(static_cast<TaskHandle_t>(handle->native));
        return osal::ok();
#else
        return osal::error_code::not_supported;
#endif
    }

    /// @brief Resumes a previously suspended task.
    /// @details Requires @c INCLUDE_vTaskSuspend == 1 in @c FreeRTOSConfig.h.
    /// @param handle Handle of the task to resume.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null;
    ///         @c error_code::not_supported if @c INCLUDE_vTaskSuspend is disabled.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
#if defined(INCLUDE_vTaskSuspend) && (INCLUDE_vTaskSuspend == 1)
        vTaskResume(static_cast<TaskHandle_t>(handle->native));
        return osal::ok();
#else
        return osal::error_code::not_supported;
#endif
    }

    /// @brief Yields the calling task, allowing other equal-or-higher-priority tasks to run.
    void osal_thread_yield() noexcept
    {
        taskYIELD();
    }

    /// @brief Blocks the calling task for at least @p ms milliseconds.
    /// @details Converts milliseconds to ticks using @c portTICK_PERIOD_MS; enforces a minimum
    ///          delay of one tick so the call always yields.
    /// @param ms Sleep duration in milliseconds.
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        const TickType_t ticks = static_cast<TickType_t>(ms / portTICK_PERIOD_MS);
        vTaskDelay(ticks > 0U ? ticks : 1U);
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

    /// @brief Creates a FreeRTOS mutex or recursive mutex.
    /// @details Uses static or dynamic allocation depending on @c OSAL_FREERTOS_DYNAMIC_ALLOC.
    /// @param handle    Output handle populated on success.
    /// @param recursive If @c true, creates a recursive mutex (xSemaphoreCreateRecursiveMutex);
    ///                  otherwise a standard mutex.
    /// @return @c osal::ok() on success; @c error_code::out_of_resources if no pool slot is
    ///         available or the kernel object could not be allocated.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool recursive) noexcept
    {
        assert(handle != nullptr);
        SemaphoreHandle_t sem;
        if (recursive)
        {
#if defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
            sem = xSemaphoreCreateRecursiveMutex();
#else
            auto* stor = fr_pool_acquire(fr_mutex_pool, fr_mutex_used);
            if (stor == nullptr)
            {
                return osal::error_code::out_of_resources;
            }
            sem = xSemaphoreCreateRecursiveMutexStatic(stor);
#endif
        }
        else
        {
#if defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
            sem = xSemaphoreCreateMutex();
#else
            auto* stor = fr_pool_acquire(fr_mutex_pool, fr_mutex_used);
            if (stor == nullptr)
            {
                return osal::error_code::out_of_resources;
            }
            sem = xSemaphoreCreateMutexStatic(stor);
#endif
        }
        if (sem == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(sem);
        return osal::ok();
    }

    /// @brief Deletes a mutex and, on the static path, releases its pool slot.
    /// @param handle Handle to destroy; native field set to @c nullptr on return.
    /// @return Always @c osal::ok().
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle != nullptr && handle->native != nullptr)
        {
#if !defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
            // Recover the StaticSemaphore_t that backs this handle.
            // FreeRTOS static mutexes: the SemaphoreHandle_t points to the
            // StaticSemaphore_t storage.  Release the pool slot.
            fr_pool_release(fr_mutex_pool, fr_mutex_used, reinterpret_cast<StaticSemaphore_t*>(handle->native));
#endif
            vSemaphoreDelete(static_cast<SemaphoreHandle_t>(handle->native));
            handle->native = nullptr;
        }
        return osal::ok();
    }

    /// @brief Acquires the mutex, blocking until @p timeout_ticks elapses.
    /// @param handle        Handle of the mutex to lock.
    /// @param timeout_ticks Maximum ticks to wait; use @c osal::WAIT_FOREVER to block indefinitely.
    /// @return @c osal::ok() if the mutex was acquired; @c error_code::timeout if the wait
    ///         expired; @c error_code::not_initialized if the handle is null.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const BaseType_t rc =
            xSemaphoreTake(static_cast<SemaphoreHandle_t>(handle->native), to_freertos_ticks(timeout_ticks));
        return (rc == pdTRUE) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Attempts to acquire the mutex without blocking.
    /// @param handle Handle of the mutex.
    /// @return @c osal::ok() if acquired immediately; @c error_code::timeout if not available.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Releases a previously acquired mutex.
    /// @param handle Handle of the mutex to unlock.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(handle->native));
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Semaphore
    // ---------------------------------------------------------------------------

    /// @brief Creates a FreeRTOS counting or binary semaphore.
    /// @details A @p max_count of 1 creates a binary semaphore; greater values create a counting
    ///          semaphore with the specified ceiling.  Uses static or dynamic allocation according
    ///          to @c OSAL_FREERTOS_DYNAMIC_ALLOC.
    /// @param handle        Output handle populated on success.
    /// @param initial_count Initial signal count placed on the semaphore.
    /// @param max_count     Maximum count the semaphore can reach.
    /// @return @c osal::ok() on success; @c error_code::out_of_resources on allocation failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        assert(handle != nullptr);
        SemaphoreHandle_t sem;
#if defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
        if (max_count == 1U)
        {
            sem = xSemaphoreCreateBinary();
            if (sem != nullptr && initial_count > 0U)
            {
                xSemaphoreGive(sem);
            }
        }
        else
        {
            sem =
                xSemaphoreCreateCounting(static_cast<UBaseType_t>(max_count), static_cast<UBaseType_t>(initial_count));
        }
#else
        auto* stor = fr_pool_acquire(fr_sem_pool, fr_sem_used);
        if (stor == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        if (max_count == 1U)
        {
            sem = xSemaphoreCreateBinaryStatic(stor);
            if (sem != nullptr && initial_count > 0U)
            {
                xSemaphoreGive(sem);
            }
        }
        else
        {
            sem = xSemaphoreCreateCountingStatic(static_cast<UBaseType_t>(max_count),
                                                 static_cast<UBaseType_t>(initial_count), stor);
        }
#endif
        if (sem == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(sem);
        return osal::ok();
    }

    /// @brief Destroys a semaphore and releases its pool slot on the static-alloc path.
    /// @param handle Handle to destroy; native field cleared on return.
    /// @return Always @c osal::ok().
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle != nullptr && handle->native != nullptr)
        {
#if !defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
            fr_pool_release(fr_sem_pool, fr_sem_used, reinterpret_cast<StaticSemaphore_t*>(handle->native));
#endif
            vSemaphoreDelete(static_cast<SemaphoreHandle_t>(handle->native));
            handle->native = nullptr;
        }
        return osal::ok();
    }

    /// @brief Signals (increments) the semaphore from a task context.
    /// @param handle Handle of the semaphore.
    /// @return @c osal::ok() on success; @c error_code::overflow if already at max count;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const BaseType_t rc = xSemaphoreGive(static_cast<SemaphoreHandle_t>(handle->native));
        return (rc == pdTRUE) ? osal::ok() : osal::error_code::overflow;
    }

    /// @brief Signals the semaphore from an ISR context.
    /// @details Calls @c xSemaphoreGiveFromISR and invokes @c portYIELD_FROM_ISR if a
    ///          higher-priority task was unblocked.
    /// @param handle Handle of the semaphore.
    /// @return @c osal::ok() on success; @c error_code::overflow if already at max count;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        BaseType_t       higher_prio_woken = pdFALSE;
        const BaseType_t rc = xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(handle->native), &higher_prio_woken);
        portYIELD_FROM_ISR(higher_prio_woken);
        return (rc == pdTRUE) ? osal::ok() : osal::error_code::overflow;
    }

    /// @brief Waits for the semaphore count to become non-zero, then decrements it.
    /// @param handle        Handle of the semaphore.
    /// @param timeout_ticks Maximum ticks to wait; @c osal::WAIT_FOREVER to block indefinitely.
    /// @return @c osal::ok() if the semaphore was taken; @c error_code::timeout on expiry;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const BaseType_t rc =
            xSemaphoreTake(static_cast<SemaphoreHandle_t>(handle->native), to_freertos_ticks(timeout_ticks));
        return (rc == pdTRUE) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Non-blocking attempt to take the semaphore.
    /// @param handle Handle of the semaphore.
    /// @return @c osal::ok() if taken immediately; @c error_code::timeout if not available.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue
    // ---------------------------------------------------------------------------

    /// @brief Creates a FreeRTOS queue backed by caller-supplied storage.
    /// @details On the static-alloc path, @p buffer must contain a @c StaticQueue_t followed by
    ///          @c capacity * @p item_size bytes of data storage.  On the dynamic path @p buffer
    ///          is ignored and the queue is heap-allocated.
    /// @param handle    Output handle populated on success.
    /// @param buffer    Pointer to @c StaticQueue_t + data storage (ignored with dynamic alloc).
    /// @param item_size Size in bytes of each queue element.
    /// @param capacity  Maximum number of elements the queue can hold.
    /// @return @c osal::ok() on success; @c error_code::out_of_resources on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        assert(handle != nullptr);
        assert(buffer != nullptr);
#if defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
        (void)buffer;
        QueueHandle_t q = xQueueCreate(static_cast<UBaseType_t>(capacity), static_cast<UBaseType_t>(item_size));
#else
        // Static queue — needs StaticQueue_t; we embed it before the data buffer.
        auto*         sq   = static_cast<StaticQueue_t*>(buffer);
        auto*         data = reinterpret_cast<std::uint8_t*>(sq) + sizeof(StaticQueue_t);
        QueueHandle_t q =
            xQueueCreateStatic(static_cast<UBaseType_t>(capacity), static_cast<UBaseType_t>(item_size), data, sq);
#endif
        if (q == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(q);
        return osal::ok();
    }

    /// @brief Destroys a queue and frees its resources.
    /// @param handle Handle to destroy; native field cleared on return.
    /// @return Always @c osal::ok().
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle != nullptr && handle->native != nullptr)
        {
            vQueueDelete(static_cast<QueueHandle_t>(handle->native));
            handle->native = nullptr;
        }
        return osal::ok();
    }

    /// @brief Posts an item to the back of the queue, blocking if full.
    /// @param handle        Handle of the target queue.
    /// @param item          Pointer to the item data to copy into the queue.
    /// @param timeout_ticks Maximum ticks to block when the queue is full.
    /// @return @c osal::ok() on success; @c error_code::timeout if the queue remained full;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const BaseType_t rc =
            xQueueSend(static_cast<QueueHandle_t>(handle->native), item, to_freertos_ticks(timeout_ticks));
        return (rc == pdTRUE) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Posts an item to the queue from an ISR context.
    /// @details Calls @c xQueueSendFromISR and yields if a higher-priority task was unblocked.
    /// @param handle Handle of the target queue.
    /// @param item   Pointer to the item data to copy.
    /// @return @c osal::ok() on success; @c error_code::overflow if the queue was full;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        BaseType_t       higher_prio_woken = pdFALSE;
        const BaseType_t rc = xQueueSendFromISR(static_cast<QueueHandle_t>(handle->native), item, &higher_prio_woken);
        portYIELD_FROM_ISR(higher_prio_woken);
        return (rc == pdTRUE) ? osal::ok() : osal::error_code::overflow;
    }

    /// @brief Receives (removes) an item from the front of the queue, blocking if empty.
    /// @param handle        Handle of the source queue.
    /// @param item          Buffer to copy the received item into.
    /// @param timeout_ticks Maximum ticks to wait for an item to become available.
    /// @return @c osal::ok() on success; @c error_code::timeout on expiry;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const BaseType_t rc =
            xQueueReceive(static_cast<QueueHandle_t>(handle->native), item, to_freertos_ticks(timeout_ticks));
        return (rc == pdTRUE) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Receives an item from the queue from an ISR context.
    /// @details Calls @c xQueueReceiveFromISR and yields if a higher-priority task was unblocked.
    /// @param handle Handle of the source queue.
    /// @param item   Buffer to copy the received item into.
    /// @return @c osal::ok() on success; @c error_code::underflow if the queue was empty;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        BaseType_t       higher_prio_woken = pdFALSE;
        const BaseType_t rc =
            xQueueReceiveFromISR(static_cast<QueueHandle_t>(handle->native), item, &higher_prio_woken);
        portYIELD_FROM_ISR(higher_prio_woken);
        return (rc == pdTRUE) ? osal::ok() : osal::error_code::underflow;
    }

    /// @brief Reads the front item without removing it from the queue.
    /// @param handle        Handle of the queue.
    /// @param item          Buffer to copy the peeked item into.
    /// @param timeout_ticks Maximum ticks to wait if the queue is currently empty.
    /// @return @c osal::ok() on success; @c error_code::would_block if no item is available;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const BaseType_t rc =
            xQueuePeek(static_cast<QueueHandle_t>(handle->native), item, to_freertos_ticks(timeout_ticks));
        return (rc == pdTRUE) ? osal::ok() : osal::error_code::would_block;
    }

    /// @brief Returns the number of items currently waiting in the queue.
    /// @param handle Read-only handle of the queue.
    /// @return Item count, or 0 if the handle is null.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<std::size_t>(uxQueueMessagesWaiting(static_cast<QueueHandle_t>(handle->native)));
    }

    /// @brief Returns the number of free slots remaining in the queue.
    /// @param handle Read-only handle of the queue.
    /// @return Free-slot count, or 0 if the handle is null.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<std::size_t>(uxQueueSpacesAvailable(static_cast<QueueHandle_t>(handle->native)));
    }

    // ---------------------------------------------------------------------------
    // Timer
    // ---------------------------------------------------------------------------

    /// @brief Creates a FreeRTOS software timer.
    /// @details A small static callback-pair pool (@c OSAL_FREERTOS_MAX_TIMERS slots) is used to
    ///          bind the OSAL (@p callback, @p arg) pair to the FreeRTOS @c TimerHandle_t.
    ///          Uses static or dynamic timer allocation depending on @c OSAL_FREERTOS_DYNAMIC_ALLOC.
    /// @param handle        Output handle populated on success.
    /// @param name          Optional null-terminated timer name.
    /// @param callback      Function invoked when the timer fires; must not be @c nullptr.
    /// @param arg           Opaque argument forwarded to @p callback.
    /// @param period_ticks  Timer period in scheduler ticks; must be > 0.
    /// @param auto_reload   If @c true the timer restarts automatically after firing.
    /// @return @c osal::ok() on success; @c error_code::out_of_resources if the callback pair
    ///         pool or static pool is exhausted, or the kernel object could not be created.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr);
        assert(callback != nullptr);
        assert(period_ticks > 0U);

        // Wrap the OSAL callback: FreeRTOS timer callback receives TimerHandle_t.
        // We use pvTimerGetTimerID to recover the arg pointer.
        // The actual OSAL callback pointer is stored as the timer ID.
        // This requires encoding both callback+arg — we use a small static pair.
        fr_cb_pair* p = nullptr;
        for (auto& slot : fr_cb_pairs)
        {
            if (slot.fn == nullptr)
            {
                p = &slot;
                break;
            }
        }
        if (p == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        p->fn = callback;
        p->a  = arg;

        auto fr_callback = [](TimerHandle_t th)
        {
            auto* pair = static_cast<fr_cb_pair*>(pvTimerGetTimerID(th));
            if (pair != nullptr && pair->fn != nullptr)
            {
                pair->fn(pair->a);
            }
        };

#if defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
        TimerHandle_t t = xTimerCreate((name != nullptr) ? name : "osal", static_cast<TickType_t>(period_ticks),
                                       auto_reload ? pdTRUE : pdFALSE, static_cast<void*>(p), fr_callback);
#else
        auto* stor = fr_pool_acquire(fr_timer_pool, fr_timer_used);
        if (stor == nullptr)
        {
            p->fn = nullptr;
            return osal::error_code::out_of_resources;
        }
        TimerHandle_t t = xTimerCreateStatic((name != nullptr) ? name : "osal", static_cast<TickType_t>(period_ticks),
                                             auto_reload ? pdTRUE : pdFALSE, static_cast<void*>(p), fr_callback, stor);
#endif
        if (t == nullptr)
        {
            p->fn = nullptr;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(t);
        return osal::ok();
    }

    /// @brief Stops and deletes a software timer, releasing its callback pair slot.
    /// @param handle Handle to destroy; native field cleared on return.
    /// @return Always @c osal::ok().
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle != nullptr && handle->native != nullptr)
        {
            auto th = static_cast<TimerHandle_t>(handle->native);
            // Release the callback pair slot.
            auto* pair = static_cast<fr_cb_pair*>(pvTimerGetTimerID(th));
            if (pair != nullptr)
            {
                pair->fn = nullptr;
                pair->a  = nullptr;
            }
#if !defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
            fr_pool_release(fr_timer_pool, fr_timer_used, reinterpret_cast<StaticTimer_t*>(handle->native));
#endif
            xTimerDelete(th, 0U);
            handle->native = nullptr;
        }
        return osal::ok();
    }

    /// @brief Starts (or restarts) a software timer.
    /// @param handle Handle of the timer.
    /// @return @c osal::ok() on success; @c error_code::unknown if the command could not be
    ///         queued to the timer daemon; @c error_code::not_initialized if the handle is null.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const BaseType_t rc = xTimerStart(static_cast<TimerHandle_t>(handle->native), 0U);
        return (rc == pdPASS) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Stops a running software timer without deleting it.
    /// @param handle Handle of the timer.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        xTimerStop(static_cast<TimerHandle_t>(handle->native), 0U);
        return osal::ok();
    }

    /// @brief Resets the timer, restarting its period from now.
    /// @param handle Handle of the timer.
    /// @return @c osal::ok() on success; @c error_code::unknown if the daemon queue is full;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const BaseType_t rc = xTimerReset(static_cast<TimerHandle_t>(handle->native), 0U);
        return (rc == pdPASS) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Changes the period of an existing timer.
    /// @details The change is posted to the timer daemon and takes effect at the next expiry.
    /// @param handle          Handle of the timer.
    /// @param new_period_ticks New period in scheduler ticks; must be > 0.
    /// @return @c osal::ok() on success; @c error_code::unknown if the daemon queue is full;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const BaseType_t rc = xTimerChangePeriod(static_cast<TimerHandle_t>(handle->native),
                                                 static_cast<TickType_t>(new_period_ticks), 0U);
        return (rc == pdPASS) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Queries whether a timer is currently running.
    /// @param handle Read-only handle of the timer.
    /// @return @c true if the timer is active (counting down); @c false otherwise or if null.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        return xTimerIsTimerActive(static_cast<TimerHandle_t>(handle->native)) != pdFALSE;
    }

    // ---------------------------------------------------------------------------
    // Event flags (EventGroup)
    // ---------------------------------------------------------------------------

    /// @brief Creates a FreeRTOS event group.
    /// @details Uses static or dynamic allocation depending on @c OSAL_FREERTOS_DYNAMIC_ALLOC.
    /// @param handle Output handle populated on success.
    /// @return @c osal::ok() on success; @c error_code::out_of_resources on failure.
    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        assert(handle != nullptr);
#if defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
        EventGroupHandle_t eg = xEventGroupCreate();
#else
        auto* stor = fr_pool_acquire(fr_event_pool, fr_event_used);
        if (stor == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        EventGroupHandle_t eg = xEventGroupCreateStatic(stor);
#endif
        if (eg == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(eg);
        return osal::ok();
    }

    /// @brief Destroys an event group and releases its pool slot on the static-alloc path.
    /// @param handle Handle to destroy; native field cleared on return.
    /// @return Always @c osal::ok().
    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle != nullptr && handle->native != nullptr)
        {
#if !defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
            fr_pool_release(fr_event_pool, fr_event_used, reinterpret_cast<StaticEventGroup_t*>(handle->native));
#endif
            vEventGroupDelete(static_cast<EventGroupHandle_t>(handle->native));
            handle->native = nullptr;
        }
        return osal::ok();
    }

    /// @brief Atomically sets one or more bits in the event group.
    /// @param handle Handle of the event group.
    /// @param bits   Bitmask of bits to set.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        xEventGroupSetBits(static_cast<EventGroupHandle_t>(handle->native), static_cast<EventBits_t>(bits));
        return osal::ok();
    }

    /// @brief Atomically clears one or more bits in the event group.
    /// @param handle Handle of the event group.
    /// @param bits   Bitmask of bits to clear.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        xEventGroupClearBits(static_cast<EventGroupHandle_t>(handle->native), static_cast<EventBits_t>(bits));
        return osal::ok();
    }

    /// @brief Returns the current value of the event group bits without modifying them.
    /// @param handle Read-only handle of the event group.
    /// @return Current bit pattern, or 0 if the handle is null.
    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<osal::event_bits_t>(xEventGroupGetBits(static_cast<EventGroupHandle_t>(handle->native)));
    }

    /// @brief Waits until at least one of the specified bits becomes set.
    /// @param handle        Handle of the event group.
    /// @param wait_bits     Bitmask of bits to watch for.
    /// @param actual_bits   Optional output: bit pattern at the time of return.
    /// @param clear_on_exit If @c true, matched bits are atomically cleared before returning.
    /// @param timeout_ticks Maximum ticks to wait.
    /// @return @c osal::ok() if any watched bit was set; @c error_code::timeout on expiry;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual_bits,
                                           bool clear_on_exit, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const EventBits_t got =
            xEventGroupWaitBits(static_cast<EventGroupHandle_t>(handle->native), static_cast<EventBits_t>(wait_bits),
                                clear_on_exit ? pdTRUE : pdFALSE,
                                pdFALSE,  // wait for ANY
                                to_freertos_ticks(timeout_ticks));
        if (actual_bits != nullptr)
        {
            *actual_bits = static_cast<osal::event_bits_t>(got);
        }
        return ((got & static_cast<EventBits_t>(wait_bits)) != 0U) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Waits until all of the specified bits are simultaneously set.
    /// @param handle        Handle of the event group.
    /// @param wait_bits     Bitmask — every bit must be set before the call returns.
    /// @param actual_bits   Optional output: bit pattern at the time of return.
    /// @param clear_on_exit If @c true, all watched bits are cleared atomically on exit.
    /// @param timeout_ticks Maximum ticks to wait.
    /// @return @c osal::ok() if all watched bits were set; @c error_code::timeout on expiry;
    ///         @c error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual_bits,
                                           bool clear_on_exit, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const EventBits_t got =
            xEventGroupWaitBits(static_cast<EventGroupHandle_t>(handle->native), static_cast<EventBits_t>(wait_bits),
                                clear_on_exit ? pdTRUE : pdFALSE,
                                pdTRUE,  // wait for ALL
                                to_freertos_ticks(timeout_ticks));
        if (actual_bits != nullptr)
        {
            *actual_bits = static_cast<osal::event_bits_t>(got);
        }
        return ((got & static_cast<EventBits_t>(wait_bits)) == static_cast<EventBits_t>(wait_bits))
                   ? osal::ok()
                   : osal::error_code::timeout;
    }

    /// @brief Sets event-group bits from an ISR context.
    /// @details Calls @c xEventGroupSetBitsFromISR and yields if a higher-priority task was unblocked.
    /// @param handle Handle of the event group.
    /// @param bits   Bitmask of bits to set.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        BaseType_t higher_prio_woken = pdFALSE;
        xEventGroupSetBitsFromISR(static_cast<EventGroupHandle_t>(handle->native), static_cast<EventBits_t>(bits),
                                  &higher_prio_woken);
        portYIELD_FROM_ISR(higher_prio_woken);
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Wait-set — not natively supported on FreeRTOS
    // ---------------------------------------------------------------------------

    /// @brief Wait-set creation — not supported on FreeRTOS.
    /// @return Always @c error_code::not_supported.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t* /* handle */) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait-set destruction — not supported on FreeRTOS.
    /// @return Always @c error_code::not_supported.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t* /* handle */) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Add a descriptor to a wait-set — not supported on FreeRTOS.
    /// @return Always @c error_code::not_supported.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t* /* h */, int /* fd */,
                                   std::uint32_t /* ev */) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Remove a descriptor from a wait-set — not supported on FreeRTOS.
    /// @return Always @c error_code::not_supported.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t* /* h */, int /* fd */) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait for events on a wait-set — not supported on FreeRTOS.
    /// @param n Set to 0 if non-null.
    /// @return Always @c error_code::not_supported.
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t* /* h */, int* /* ids */,
                                    std::size_t /* max */, std::size_t* n, osal::tick_t /* timeout */) noexcept
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
    // Stream buffer (native — xStreamBuffer*)
    // ---------------------------------------------------------------------------

    /// @brief Creates a FreeRTOS stream buffer backed by caller-supplied storage.
    /// @details On the static-alloc path the @p buffer is used for ring-storage and a pool slot
    ///          provides the @c StaticStreamBuffer_t control block.  On the dynamic path
    ///          @p buffer is ignored.
    /// @param handle        Output handle populated on success.
    /// @param buffer        Pointer to @p capacity bytes of raw storage (ignored with dynamic alloc).
    /// @param capacity      Byte capacity of the stream buffer.
    /// @param trigger_level Minimum bytes that must be available before a blocked receiver wakes
    ///                      (clamped to 1 if 0).
    /// @return @c osal::ok() on success; @c error_code::invalid_argument for bad parameters;
    ///         @c error_code::out_of_resources if the pool or kernel allocation failed.
    osal::result osal_stream_buffer_create(osal::active_traits::stream_buffer_handle_t* handle, void* buffer,
                                           std::size_t capacity, std::size_t trigger_level) noexcept
    {
        if (!handle || !buffer || capacity == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        const std::size_t trig = (trigger_level == 0U) ? 1U : trigger_level;

#if !defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
        StaticStreamBuffer_t* scb = fr_pool_acquire(fr_sbuf_pool, fr_sbuf_used);
        if (!scb)
        {
            return osal::error_code::out_of_resources;
        }
        // storage must be capacity+1 bytes (one sentinel slot for the SPSC ring).
        StreamBufferHandle_t h = xStreamBufferCreateStatic(static_cast<std::size_t>(capacity), trig,
                                                           static_cast<std::uint8_t*>(buffer), scb);
        if (!h)
        {
            fr_pool_release(fr_sbuf_pool, fr_sbuf_used, scb);
            return osal::error_code::out_of_resources;
        }
#else
        (void)buffer;
        StreamBufferHandle_t h = xStreamBufferCreate(static_cast<std::size_t>(capacity), trig);
        if (!h)
        {
            return osal::error_code::out_of_resources;
        }
#endif
        handle->native = h;
        return osal::ok();
    }

    /// @brief Destroys a stream buffer and releases its pool slot on the static-alloc path.
    /// @param handle Handle to destroy; native field cleared on return.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null.
    osal::result osal_stream_buffer_destroy(osal::active_traits::stream_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* h = static_cast<StreamBufferHandle_t>(handle->native);
#if !defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
        fr_pool_release(fr_sbuf_pool, fr_sbuf_used, reinterpret_cast<StaticStreamBuffer_t*>(handle->native));
#endif
        vStreamBufferDelete(h);  // no-op for static buffers; frees heap for dynamic
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Writes bytes into the stream buffer, blocking if insufficient space is available.
    /// @param handle        Handle of the stream buffer.
    /// @param data          Pointer to the data to send.
    /// @param len           Number of bytes to send.
    /// @param timeout_ticks Maximum ticks to wait for space.
    /// @return @c osal::ok() if all bytes were written; @c error_code::timeout if not all bytes
    ///         could be sent; @c error_code::invalid_argument for bad parameters.
    osal::result osal_stream_buffer_send(osal::active_traits::stream_buffer_handle_t* handle, const void* data,
                                         std::size_t len, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native || !data || len == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        auto*             h    = static_cast<StreamBufferHandle_t>(handle->native);
        const std::size_t sent = xStreamBufferSend(h, data, len, to_freertos_ticks(timeout_ticks));
        return (sent == len) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Writes bytes into the stream buffer from an ISR context.
    /// @details Calls @c xStreamBufferSendFromISR and yields if a higher-priority task was unblocked.
    /// @param handle Handle of the stream buffer.
    /// @param data   Pointer to the data to send.
    /// @param len    Number of bytes to send.
    /// @return @c osal::ok() if all @p len bytes were sent; @c error_code::timeout if fewer were
    ///         accepted; @c error_code::invalid_argument for bad parameters.
    osal::result osal_stream_buffer_send_isr(osal::active_traits::stream_buffer_handle_t* handle, const void* data,
                                             std::size_t len) noexcept
    {
        if (!handle || !handle->native || !data || len == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        BaseType_t        higher_prio_woken = pdFALSE;
        const std::size_t sent =
            xStreamBufferSendFromISR(static_cast<StreamBufferHandle_t>(handle->native), data, len, &higher_prio_woken);
        portYIELD_FROM_ISR(higher_prio_woken);
        return (sent == len) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Reads up to @p max_len bytes from the stream buffer, blocking if empty.
    /// @param handle        Handle of the stream buffer.
    /// @param buf           Destination buffer.
    /// @param max_len       Maximum bytes to read.
    /// @param timeout_ticks Maximum ticks to wait for data.
    /// @return Number of bytes actually read, or 0 if the handle is null or no data arrived.
    std::size_t osal_stream_buffer_receive(osal::active_traits::stream_buffer_handle_t* handle, void* buf,
                                           std::size_t max_len, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native || !buf || max_len == 0U)
        {
            return 0U;
        }
        return xStreamBufferReceive(static_cast<StreamBufferHandle_t>(handle->native), buf, max_len,
                                    to_freertos_ticks(timeout_ticks));
    }

    /// @brief Reads bytes from the stream buffer in an ISR context.
    /// @details Calls @c xStreamBufferReceiveFromISR and yields if a higher-priority task was unblocked.
    /// @param handle  Handle of the stream buffer.
    /// @param buf     Destination buffer.
    /// @param max_len Maximum bytes to read.
    /// @return Number of bytes actually read, or 0 on invalid parameters.
    std::size_t osal_stream_buffer_receive_isr(osal::active_traits::stream_buffer_handle_t* handle, void* buf,
                                               std::size_t max_len) noexcept
    {
        if (!handle || !handle->native || !buf || max_len == 0U)
        {
            return 0U;
        }
        BaseType_t        higher_prio_woken = pdFALSE;
        const std::size_t n = xStreamBufferReceiveFromISR(static_cast<StreamBufferHandle_t>(handle->native), buf,
                                                          max_len, &higher_prio_woken);
        portYIELD_FROM_ISR(higher_prio_woken);
        return n;
    }

    /// @brief Returns the number of bytes currently stored in the stream buffer.
    /// @param handle Read-only handle of the stream buffer.
    /// @return Bytes available, or 0 if the handle is null.
    std::size_t osal_stream_buffer_available(const osal::active_traits::stream_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        return xStreamBufferBytesAvailable(static_cast<StreamBufferHandle_t>(handle->native));
    }

    /// @brief Returns the number of bytes that can still be written to the stream buffer.
    /// @param handle Read-only handle of the stream buffer.
    /// @return Free bytes, or 0 if the handle is null.
    std::size_t osal_stream_buffer_free_space(const osal::active_traits::stream_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        return xStreamBufferSpacesAvailable(static_cast<StreamBufferHandle_t>(handle->native));
    }

    /// @brief Resets a stream buffer to its empty state.
    /// @details Any data remaining in the buffer is discarded.  Should only be called when no
    ///          tasks are blocked on the buffer.
    /// @param handle Handle of the stream buffer.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null.
    osal::result osal_stream_buffer_reset(osal::active_traits::stream_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        xStreamBufferReset(static_cast<StreamBufferHandle_t>(handle->native));
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Message buffer (native — xMessageBuffer*)
    // ---------------------------------------------------------------------------

    /// @brief Creates a FreeRTOS message buffer backed by caller-supplied storage.
    /// @details Message buffers store variable-length, length-prefixed messages.  On the
    ///          static-alloc path @p buffer provides the ring storage and a pool slot provides
    ///          the control block.  On the dynamic path @p buffer is ignored.
    /// @param handle   Output handle populated on success.
    /// @param buffer   Pointer to @p capacity bytes of raw storage (ignored with dynamic alloc).
    /// @param capacity Byte capacity of the message buffer (total, including length fields).
    /// @return @c osal::ok() on success; @c error_code::invalid_argument for bad parameters;
    ///         @c error_code::out_of_resources on allocation failure.
    osal::result osal_message_buffer_create(osal::active_traits::message_buffer_handle_t* handle, void* buffer,
                                            std::size_t capacity) noexcept
    {
        if (!handle || !buffer || capacity == 0U)
        {
            return osal::error_code::invalid_argument;
        }

#if !defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
        StaticStreamBuffer_t* scb = fr_pool_acquire(fr_mbuf_pool, fr_mbuf_used);
        if (!scb)
        {
            return osal::error_code::out_of_resources;
        }
        MessageBufferHandle_t h =
            xMessageBufferCreateStatic(static_cast<std::size_t>(capacity), static_cast<std::uint8_t*>(buffer), scb);
        if (!h)
        {
            fr_pool_release(fr_mbuf_pool, fr_mbuf_used, scb);
            return osal::error_code::out_of_resources;
        }
#else
        (void)buffer;
        MessageBufferHandle_t h = xMessageBufferCreate(static_cast<std::size_t>(capacity));
        if (!h)
        {
            return osal::error_code::out_of_resources;
        }
#endif
        handle->native = h;
        return osal::ok();
    }

    /// @brief Destroys a message buffer and releases its pool slot on the static-alloc path.
    /// @param handle Handle to destroy; native field cleared on return.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null.
    osal::result osal_message_buffer_destroy(osal::active_traits::message_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* h = static_cast<MessageBufferHandle_t>(handle->native);
#if !defined(OSAL_FREERTOS_DYNAMIC_ALLOC)
        fr_pool_release(fr_mbuf_pool, fr_mbuf_used, reinterpret_cast<StaticStreamBuffer_t*>(handle->native));
#endif
        vMessageBufferDelete(h);  // no-op for static; frees heap for dynamic
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Sends a discrete message into the message buffer, blocking if insufficient space.
    /// @details The entire message (up to @p len bytes) is written atomically with a length prefix.
    /// @param handle        Handle of the message buffer.
    /// @param msg           Pointer to the message data.
    /// @param len           Number of bytes in the message.
    /// @param timeout_ticks Maximum ticks to wait for space.
    /// @return @c osal::ok() if the full message was written; @c error_code::timeout if not;
    ///         @c error_code::invalid_argument for bad parameters.
    osal::result osal_message_buffer_send(osal::active_traits::message_buffer_handle_t* handle, const void* msg,
                                          std::size_t len, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native || !msg || len == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        auto*             h    = static_cast<MessageBufferHandle_t>(handle->native);
        const std::size_t sent = xMessageBufferSend(h, msg, len, to_freertos_ticks(timeout_ticks));
        return (sent == len) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Sends a message into the message buffer from an ISR context.
    /// @details Calls @c xMessageBufferSendFromISR and yields if a higher-priority task was unblocked.
    /// @param handle Handle of the message buffer.
    /// @param msg    Pointer to the message data.
    /// @param len    Number of bytes in the message.
    /// @return @c osal::ok() if the full message was written; @c error_code::timeout if not;
    ///         @c error_code::invalid_argument for bad parameters.
    osal::result osal_message_buffer_send_isr(osal::active_traits::message_buffer_handle_t* handle, const void* msg,
                                              std::size_t len) noexcept
    {
        if (!handle || !handle->native || !msg || len == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        BaseType_t        higher_prio_woken = pdFALSE;
        const std::size_t sent =
            xMessageBufferSendFromISR(static_cast<MessageBufferHandle_t>(handle->native), msg, len, &higher_prio_woken);
        portYIELD_FROM_ISR(higher_prio_woken);
        return (sent == len) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Receives the next message from the message buffer, blocking if empty.
    /// @param handle        Handle of the message buffer.
    /// @param buf           Destination buffer; must be large enough for the next message.
    /// @param max_len       Size of @p buf in bytes.
    /// @param timeout_ticks Maximum ticks to wait for a message.
    /// @return Number of bytes in the received message, or 0 if the handle is null / no message arrived.
    std::size_t osal_message_buffer_receive(osal::active_traits::message_buffer_handle_t* handle, void* buf,
                                            std::size_t max_len, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native || !buf || max_len == 0U)
        {
            return 0U;
        }
        return xMessageBufferReceive(static_cast<MessageBufferHandle_t>(handle->native), buf, max_len,
                                     to_freertos_ticks(timeout_ticks));
    }

    /// @brief Receives a message from the message buffer in an ISR context.
    /// @details Calls @c xMessageBufferReceiveFromISR and yields if a higher-priority task was unblocked.
    /// @param handle  Handle of the message buffer.
    /// @param buf     Destination buffer.
    /// @param max_len Size of @p buf in bytes.
    /// @return Number of bytes in the received message, or 0 on invalid parameters.
    std::size_t osal_message_buffer_receive_isr(osal::active_traits::message_buffer_handle_t* handle, void* buf,
                                                std::size_t max_len) noexcept
    {
        if (!handle || !handle->native || !buf || max_len == 0U)
        {
            return 0U;
        }
        BaseType_t        higher_prio_woken = pdFALSE;
        const std::size_t n = xMessageBufferReceiveFromISR(static_cast<MessageBufferHandle_t>(handle->native), buf,
                                                           max_len, &higher_prio_woken);
        portYIELD_FROM_ISR(higher_prio_woken);
        return n;
    }

    /// @brief Returns the byte count of the next queued message (0 if the buffer is empty).
    /// @details Uses @c xStreamBufferNextMessageLengthBytes internally.
    /// @param handle Read-only handle of the message buffer.
    /// @return Byte count of the next message, or 0 if the buffer is empty or the handle is null.
    std::size_t osal_message_buffer_available(const osal::active_traits::message_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        // xStreamBufferNextMessageLengthBytes returns the exact byte-count of the
        // next queued message (0 if the buffer is empty).
        return xStreamBufferNextMessageLengthBytes(static_cast<StreamBufferHandle_t>(handle->native));
    }

    /// @brief Returns the number of bytes that can still be written to the message buffer.
    /// @param handle Read-only handle of the message buffer.
    /// @return Free bytes, or 0 if the handle is null.
    std::size_t osal_message_buffer_free_space(const osal::active_traits::message_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        return xMessageBufferSpacesAvailable(static_cast<MessageBufferHandle_t>(handle->native));
    }

    /// @brief Resets a message buffer to its empty state, discarding all pending messages.
    /// @details Should only be called when no tasks are blocked on the buffer.
    /// @param handle Handle of the message buffer.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if the handle is null.
    osal::result osal_message_buffer_reset(osal::active_traits::message_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        xMessageBufferReset(static_cast<MessageBufferHandle_t>(handle->native));
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Spinlock (stub — not natively supported on this backend)
    // ---------------------------------------------------------------------------
#include "../common/emulated_spinlock.inl"

    // ---------------------------------------------------------------------------
    // Barrier (stub — not natively supported on this backend)
    // ---------------------------------------------------------------------------
#include "../common/emulated_barrier.inl"

    // ---------------------------------------------------------------------------
    // Task notification (native — xTaskNotify / xTaskNotifyFromISR / xTaskNotifyWait)
    // ---------------------------------------------------------------------------

    /// @brief Sends a direct-to-task notification via @c xTaskNotify.
    /// @details Uses @c eSetValueWithOverwrite so the value is always accepted.
    /// @param handle Handle of the target task.
    /// @param value  32-bit notification value.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if null.
    osal::result osal_task_notify(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        xTaskNotify(static_cast<TaskHandle_t>(handle->native), value, eSetValueWithOverwrite);
        return osal::ok();
    }

    /// @brief Sends a direct-to-task notification from ISR context via @c xTaskNotifyFromISR.
    /// @details Yields to a higher-priority task if one was unblocked.
    /// @param handle Handle of the target task.
    /// @param value  32-bit notification value.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if null.
    osal::result osal_task_notify_isr(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        BaseType_t higher_prio_woken = pdFALSE;
        xTaskNotifyFromISR(static_cast<TaskHandle_t>(handle->native), value, eSetValueWithOverwrite,
                           &higher_prio_woken);
        portYIELD_FROM_ISR(higher_prio_woken);
        return osal::ok();
    }

    /// @brief Waits for a direct-to-task notification on the calling task via @c xTaskNotifyWait.
    /// @param clear_on_entry  Bits to clear in the notification value before waiting.
    /// @param clear_on_exit   Bits to clear in the notification value after a successful receive.
    /// @param value_out       Receives the 32-bit notification value (may be @c nullptr).
    /// @param timeout_ticks   Maximum ticks to wait; @c osal::WAIT_FOREVER to block indefinitely.
    /// @return @c osal::ok() if notified; @c error_code::timeout if the wait expired.
    osal::result osal_task_notify_wait(std::uint32_t clear_on_entry, std::uint32_t clear_on_exit,
                                       std::uint32_t* value_out, osal::tick_t timeout_ticks) noexcept
    {
        const BaseType_t rc =
            xTaskNotifyWait(clear_on_entry, clear_on_exit, value_out, to_freertos_ticks(timeout_ticks));
        return (rc == pdTRUE) ? osal::ok() : osal::error_code::timeout;
    }

}  // extern "C"

// ms_to_ticks is now inline via clock_utils in clock.hpp — no per-class definitions needed.
