// SPDX-License-Identifier: Apache-2.0
/// @file osal_c.cpp
/// @brief C interface bridge — translates pure-C calls to the C++ backend API.
/// @details This file is compiled as C++17 and linked into the microsal library.
///          It provides thin extern "C" wrappers that convert between the C-side
///          types (osal_mutex_handle, osal_result_t, etc.) and the C++ types
///          (osal::active_traits::mutex_handle_t, osal::result, etc.).
///
///          The C and C++ handle types are layout-compatible (both are
///          `struct { void* native; }`), so conversion is a reinterpret_cast.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#include <osal/osal.hpp>
#include <osal/osal_c.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Handle conversion helpers (layout-compatible: struct { void* native; })
// ---------------------------------------------------------------------------
#define OSAL_C_CAST(CppHandleType, c_ptr) reinterpret_cast<CppHandleType*>(c_ptr)

#define OSAL_C_CAST_CONST(CppHandleType, c_ptr) reinterpret_cast<const CppHandleType*>(c_ptr)

// Convert osal::result → osal_result_t (int32_t)
static inline osal_result_t to_c(osal::result r) noexcept
{
    return static_cast<osal_result_t>(r.code());
}

// ---------------------------------------------------------------------------
// Shorthand for the active-backend handle types
// ---------------------------------------------------------------------------
using mtx_h = osal::active_traits::mutex_handle_t;
using sem_h = osal::active_traits::semaphore_handle_t;
using que_h = osal::active_traits::queue_handle_t;
using thr_h = osal::active_traits::thread_handle_t;
using tmr_h = osal::active_traits::timer_handle_t;
using ef_h  = osal::active_traits::event_flags_handle_t;
using cv_h  = osal::active_traits::condvar_handle_t;
using wq_h  = osal::active_traits::work_queue_handle_t;
using mp_h  = osal::active_traits::memory_pool_handle_t;
using rw_h  = osal::active_traits::rwlock_handle_t;
using sb_h  = osal::active_traits::stream_buffer_handle_t;
using mb_h  = osal::active_traits::message_buffer_handle_t;

// ============================================================================
// Clock
// ============================================================================

/// @brief Return the monotonic time in milliseconds.
/// @return Milliseconds since an arbitrary but fixed epoch (never goes backwards).
extern "C" int64_t osal_c_clock_monotonic_ms(void)
{
    return osal_clock_monotonic_ms();
}

/// @brief Return the current wall-clock (system) time in milliseconds since the Unix epoch.
/// @return Milliseconds since 1970-01-01T00:00:00Z.
extern "C" int64_t osal_c_clock_system_ms(void)
{
    return osal_clock_system_ms();
}

/// @brief Return the raw backend tick counter.
/// @return Current tick value; wraps at `osal_tick_t` maximum.
extern "C" osal_tick_t osal_c_clock_ticks(void)
{
    return osal_clock_ticks();
}

/// @brief Return the tick period in microseconds.
/// @return Microseconds per tick (backend-specific constant).
extern "C" uint32_t osal_c_clock_tick_period_us(void)
{
    return osal_clock_tick_period_us();
}

// ============================================================================
// Mutex
// ============================================================================

/// @brief Create a mutex; see `osal_mutex_create()` for parameter semantics.
/// @param handle    Output handle.
/// @param recursive Non-zero to create a recursive mutex.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_mutex_create(osal_mutex_handle* handle, int recursive)
{
    return to_c(osal_mutex_create(OSAL_C_CAST(mtx_h, handle), recursive != 0));
}

/// @brief Create a mutex from a config struct; see `osal_mutex_create()`.
/// @param handle Output handle.
/// @param cfg    Pointer to `osal_mutex_config` describing the mutex.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_mutex_create_with_cfg(osal_mutex_handle* handle, const osal_mutex_config* cfg)
{
    return to_c(osal_mutex_create(OSAL_C_CAST(mtx_h, handle), cfg->recursive != 0));
}

/// @brief Destroy a mutex; see `osal_mutex_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_mutex_destroy(osal_mutex_handle* handle)
{
    return to_c(osal_mutex_destroy(OSAL_C_CAST(mtx_h, handle)));
}

/// @brief Lock a mutex, blocking up to @p timeout ticks; see `osal_mutex_lock()`.
/// @param handle  Mutex handle.
/// @param timeout Ticks to wait; `OSAL_WAIT_FOREVER` to block indefinitely.
/// @return `OSAL_OK` on success, `OSAL_TIMEOUT` on expiry.
extern "C" osal_result_t osal_c_mutex_lock(osal_mutex_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_mutex_lock(OSAL_C_CAST(mtx_h, handle), timeout));
}

/// @brief Attempt to lock a mutex without blocking; see `osal_mutex_try_lock()`.
/// @param handle Mutex handle.
/// @return `OSAL_OK` if locked, `OSAL_WOULD_BLOCK` if already held.
extern "C" osal_result_t osal_c_mutex_try_lock(osal_mutex_handle* handle)
{
    return to_c(osal_mutex_try_lock(OSAL_C_CAST(mtx_h, handle)));
}

/// @brief Unlock a mutex; see `osal_mutex_unlock()`.
/// @param handle Mutex handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_mutex_unlock(osal_mutex_handle* handle)
{
    return to_c(osal_mutex_unlock(OSAL_C_CAST(mtx_h, handle)));
}

// ============================================================================
// Semaphore
// ============================================================================

/// @brief Create a counting semaphore; see `osal_semaphore_create()`.
/// @param handle        Output handle.
/// @param initial_count Initial count.
/// @param max_count     Maximum count.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_semaphore_create(osal_semaphore_handle* handle, unsigned int initial_count,
                                                 unsigned int max_count)
{
    return to_c(osal_semaphore_create(OSAL_C_CAST(sem_h, handle), initial_count, max_count));
}

/// @brief Create a semaphore from a config struct; see `osal_semaphore_create()`.
/// @param handle Output handle.
/// @param cfg    Pointer to `osal_semaphore_config`.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_semaphore_create_with_cfg(osal_semaphore_handle*       handle,
                                                          const osal_semaphore_config* cfg)
{
    return to_c(osal_semaphore_create(OSAL_C_CAST(sem_h, handle), cfg->initial_count, cfg->max_count));
}

/// @brief Destroy a semaphore; see `osal_semaphore_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_semaphore_destroy(osal_semaphore_handle* handle)
{
    return to_c(osal_semaphore_destroy(OSAL_C_CAST(sem_h, handle)));
}

/// @brief Give (signal) a semaphore from task context; see `osal_semaphore_give()`.
/// @param handle Semaphore handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_semaphore_give(osal_semaphore_handle* handle)
{
    return to_c(osal_semaphore_give(OSAL_C_CAST(sem_h, handle)));
}

/// @brief Give a semaphore from ISR context; see `osal_semaphore_give_isr()`.
/// @param handle Semaphore handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_semaphore_give_isr(osal_semaphore_handle* handle)
{
    return to_c(osal_semaphore_give_isr(OSAL_C_CAST(sem_h, handle)));
}

/// @brief Take (wait on) a semaphore, blocking up to @p timeout ticks; see `osal_semaphore_take()`.
/// @param handle  Semaphore handle.
/// @param timeout Ticks to wait; `OSAL_WAIT_FOREVER` to block indefinitely.
/// @return `OSAL_OK` on success, `OSAL_TIMEOUT` on expiry.
extern "C" osal_result_t osal_c_semaphore_take(osal_semaphore_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_semaphore_take(OSAL_C_CAST(sem_h, handle), timeout));
}

/// @brief Attempt to take a semaphore without blocking; see `osal_semaphore_try_take()`.
/// @param handle Semaphore handle.
/// @return `OSAL_OK` if taken, `OSAL_WOULD_BLOCK` if count is zero.
extern "C" osal_result_t osal_c_semaphore_try_take(osal_semaphore_handle* handle)
{
    return to_c(osal_semaphore_try_take(OSAL_C_CAST(sem_h, handle)));
}

// ============================================================================
// Queue
// ============================================================================

/// @brief Create a message queue; see `osal_queue_create()`.
/// @param handle    Output handle.
/// @param buffer    Caller-supplied backing storage.
/// @param item_size Size of each message in bytes.
/// @param capacity  Maximum number of messages.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_queue_create(osal_queue_handle* handle, void* buffer, size_t item_size, size_t capacity)
{
    return to_c(osal_queue_create(OSAL_C_CAST(que_h, handle), buffer, item_size, capacity));
}

/// @brief Create a queue from a config struct; see `osal_queue_create()`.
/// @param handle Output handle.
/// @param cfg    Pointer to `osal_queue_config`.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_queue_create_with_cfg(osal_queue_handle* handle, const osal_queue_config* cfg)
{
    return to_c(osal_queue_create(OSAL_C_CAST(que_h, handle), cfg->buffer, cfg->item_size, cfg->capacity));
}

/// @brief Destroy a queue; see `osal_queue_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_queue_destroy(osal_queue_handle* handle)
{
    return to_c(osal_queue_destroy(OSAL_C_CAST(que_h, handle)));
}

/// @brief Send a message, blocking up to @p timeout ticks; see `osal_queue_send()`.
/// @param handle  Queue handle.
/// @param item    Pointer to the message data.
/// @param timeout Ticks to wait; `OSAL_WAIT_FOREVER` for indefinite.
/// @return `OSAL_OK` on success, `OSAL_TIMEOUT` if full.
extern "C" osal_result_t osal_c_queue_send(osal_queue_handle* handle, const void* item, osal_tick_t timeout)
{
    return to_c(osal_queue_send(OSAL_C_CAST(que_h, handle), item, timeout));
}

/// @brief Send a message from ISR context (non-blocking); see `osal_queue_send_isr()`.
/// @param handle Queue handle.
/// @param item   Pointer to the message data.
/// @return `OSAL_OK` on success, `OSAL_TIMEOUT` if full.
extern "C" osal_result_t osal_c_queue_send_isr(osal_queue_handle* handle, const void* item)
{
    return to_c(osal_queue_send_isr(OSAL_C_CAST(que_h, handle), item));
}

/// @brief Receive a message, blocking up to @p timeout ticks; see `osal_queue_receive()`.
/// @param handle  Queue handle.
/// @param item    Output buffer for the received message.
/// @param timeout Ticks to wait; `OSAL_WAIT_FOREVER` for indefinite.
/// @return `OSAL_OK` on success, `OSAL_TIMEOUT` if empty.
extern "C" osal_result_t osal_c_queue_receive(osal_queue_handle* handle, void* item, osal_tick_t timeout)
{
    return to_c(osal_queue_receive(OSAL_C_CAST(que_h, handle), item, timeout));
}

/// @brief Receive a message from ISR context (non-blocking); see `osal_queue_receive_isr()`.
/// @param handle Queue handle.
/// @param item   Output buffer for the received message.
/// @return `OSAL_OK` on success, `OSAL_TIMEOUT` if empty.
extern "C" osal_result_t osal_c_queue_receive_isr(osal_queue_handle* handle, void* item)
{
    return to_c(osal_queue_receive_isr(OSAL_C_CAST(que_h, handle), item));
}

/// @brief Peek at the front message without removing it; see `osal_queue_peek()`.
/// @param handle  Queue handle.
/// @param item    Output buffer for the peeked item.
/// @param timeout Ticks to wait.
/// @return `OSAL_OK` on success, `OSAL_TIMEOUT` if empty.
extern "C" osal_result_t osal_c_queue_peek(osal_queue_handle* handle, void* item, osal_tick_t timeout)
{
    return to_c(osal_queue_peek(OSAL_C_CAST(que_h, handle), item, timeout));
}

/// @brief Return the number of messages currently in the queue.
/// @param handle Queue handle (const).
/// @return Message count.
extern "C" size_t osal_c_queue_count(const osal_queue_handle* handle)
{
    return osal_queue_count(OSAL_C_CAST_CONST(que_h, handle));
}

/// @brief Return the number of free slots in the queue.
/// @param handle Queue handle (const).
/// @return Free slot count.
extern "C" size_t osal_c_queue_free(const osal_queue_handle* handle)
{
    return osal_queue_free(OSAL_C_CAST_CONST(que_h, handle));
}

// ============================================================================
// Thread
// ============================================================================

/// @brief Create and start a thread; see `osal_thread_create()`.
/// @param handle     Output handle.
/// @param entry      Thread entry function.
/// @param arg        Argument passed to @p entry.
/// @param priority   OSAL priority [0=lowest, 255=highest].
/// @param affinity   CPU affinity mask or `OSAL_AFFINITY_ANY`.
/// @param stack      Caller-supplied stack buffer.
/// @param stack_bytes Size of @p stack in bytes.
/// @param name       Optional thread name.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_thread_create(osal_thread_handle* handle, osal_c_thread_entry_t entry, void* arg,
                                              osal_priority_t priority, osal_affinity_t affinity, void* stack,
                                              size_t stack_bytes, const char* name)
{
    return to_c(
        osal_thread_create(OSAL_C_CAST(thr_h, handle), entry, arg, priority, affinity, stack, stack_bytes, name));
}

/// @brief Create a thread from a config struct; see `osal_thread_create()`.
/// @param handle Output handle.
/// @param cfg    Pointer to `osal_thread_config`.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_thread_create_with_cfg(osal_thread_handle* handle, const osal_thread_config* cfg)
{
    return to_c(osal_thread_create(OSAL_C_CAST(thr_h, handle), cfg->entry, cfg->arg, cfg->priority, cfg->affinity,
                                   cfg->stack, cfg->stack_bytes, cfg->name));
}

/// @brief Wait for a thread to finish, blocking up to @p timeout ticks; see `osal_thread_join()`.
/// @param handle  Thread handle.
/// @param timeout Ticks to wait; `OSAL_WAIT_FOREVER` for indefinite.
/// @return `OSAL_OK` on join, `OSAL_TIMEOUT` on expiry.
extern "C" osal_result_t osal_c_thread_join(osal_thread_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_thread_join(OSAL_C_CAST(thr_h, handle), timeout));
}

/// @brief Detach a thread (fire-and-forget); see `osal_thread_detach()`.
/// @param handle Thread handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_thread_detach(osal_thread_handle* handle)
{
    return to_c(osal_thread_detach(OSAL_C_CAST(thr_h, handle)));
}

/// @brief Change the priority of a running thread; see `osal_thread_set_priority()`.
/// @param handle   Thread handle.
/// @param priority New OSAL priority [0=lowest, 255=highest].
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_thread_set_priority(osal_thread_handle* handle, osal_priority_t priority)
{
    return to_c(osal_thread_set_priority(OSAL_C_CAST(thr_h, handle), priority));
}

/// @brief Change the CPU affinity of a running thread; see `osal_thread_set_affinity()`.
/// @param handle   Thread handle.
/// @param affinity New affinity mask.
/// @return `OSAL_OK` on success, `OSAL_NOT_SUPPORTED` on backends without affinity.
extern "C" osal_result_t osal_c_thread_set_affinity(osal_thread_handle* handle, osal_affinity_t affinity)
{
    return to_c(osal_thread_set_affinity(OSAL_C_CAST(thr_h, handle), affinity));
}

/// @brief Suspend a thread; see `osal_thread_suspend()`.
/// @param handle Thread handle.
/// @return `OSAL_OK` on success, `OSAL_NOT_SUPPORTED` if the backend lacks suspend.
extern "C" osal_result_t osal_c_thread_suspend(osal_thread_handle* handle)
{
    return to_c(osal_thread_suspend(OSAL_C_CAST(thr_h, handle)));
}

/// @brief Resume a suspended thread; see `osal_thread_resume()`.
/// @param handle Thread handle.
/// @return `OSAL_OK` on success, `OSAL_NOT_SUPPORTED` if the backend lacks resume.
extern "C" osal_result_t osal_c_thread_resume(osal_thread_handle* handle)
{
    return to_c(osal_thread_resume(OSAL_C_CAST(thr_h, handle)));
}

/// @brief Voluntarily yield the CPU to another ready thread.
extern "C" void osal_c_thread_yield(void)
{
    osal_thread_yield();
}

/// @brief Sleep the current thread for @p ms milliseconds.
/// @param ms Milliseconds to sleep.
extern "C" void osal_c_thread_sleep_ms(uint32_t ms)
{
    osal_thread_sleep_ms(ms);
}

// ============================================================================
// Thread-Local Data
// ============================================================================

/// @brief Allocate a thread-local storage key.
/// @param handle Output handle; `handle->key` is set on success.
/// @return `OSAL_OK` on success, `OSAL_OUT_OF_RESOURCES` if the key table is full.
extern "C" osal_result_t osal_c_tls_key_create(osal_tls_key_handle* handle)
{
    if (handle == nullptr)
    {
        return OSAL_INVALID_ARGUMENT;
    }

    std::uint8_t key = 0U;
    if (!osal::detail::tls_registry::acquire(&key))
    {
        return OSAL_OUT_OF_RESOURCES;
    }

    handle->key   = key;
    handle->valid = 1U;
    return OSAL_OK;
}

/// @brief Release a thread-local storage key.
/// @param handle Key handle to destroy.
/// @return `OSAL_OK` on success, `OSAL_NOT_INITIALIZED` if already destroyed.
extern "C" osal_result_t osal_c_tls_key_destroy(osal_tls_key_handle* handle)
{
    if (handle == nullptr)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->valid == 0U)
    {
        return OSAL_NOT_INITIALIZED;
    }

    osal::detail::tls_registry::release(handle->key);
    handle->key   = 0U;
    handle->valid = 0U;
    return OSAL_OK;
}

/// @brief Store @p value in thread-local storage for the calling thread.
/// @param handle Key handle.
/// @param value  Value to store (opaque pointer).
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_tls_set(osal_tls_key_handle* handle, void* value)
{
    if (handle == nullptr)
    {
        return OSAL_INVALID_ARGUMENT;
    }
    if (handle->valid == 0U)
    {
        return OSAL_NOT_INITIALIZED;
    }

    osal::detail::tls_storage::set(handle->key, value);
    return OSAL_OK;
}

/// @brief Retrieve the value stored by the calling thread for @p key.
/// @param handle Key handle (const).
/// @return Previously set value, or `nullptr` if none or @p handle is invalid.
extern "C" void* osal_c_tls_get(const osal_tls_key_handle* handle)
{
    if (handle == nullptr || handle->valid == 0U)
    {
        return nullptr;
    }
    return osal::detail::tls_storage::get(handle->key);
}

// ============================================================================
// Timer
// ============================================================================

/// @brief Create a software timer; see `osal_timer_create()`.
/// @param handle       Output handle.
/// @param name         Optional timer name.
/// @param callback     Function called when the timer fires.
/// @param arg          Argument passed to @p callback.
/// @param period_ticks Period in ticks.
/// @param auto_reload  Non-zero for a periodic timer; zero for one-shot.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_timer_create(osal_timer_handle* handle, const char* name,
                                             osal_c_timer_callback_t callback, void* arg, osal_tick_t period_ticks,
                                             int auto_reload)
{
    return to_c(osal_timer_create(OSAL_C_CAST(tmr_h, handle), name, callback, arg, period_ticks, auto_reload != 0));
}

/// @brief Create a timer from a config struct; see `osal_timer_create()`.
/// @param handle Output handle.
/// @param cfg    Pointer to `osal_timer_config`.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_timer_create_with_cfg(osal_timer_handle* handle, const osal_timer_config* cfg)
{
    return to_c(osal_timer_create(OSAL_C_CAST(tmr_h, handle), cfg->name, cfg->callback, cfg->arg, cfg->period_ticks,
                                  cfg->auto_reload != 0));
}

/// @brief Destroy a timer; see `osal_timer_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_timer_destroy(osal_timer_handle* handle)
{
    return to_c(osal_timer_destroy(OSAL_C_CAST(tmr_h, handle)));
}

/// @brief Start (arm) a timer; see `osal_timer_start()`.
/// @param handle Timer handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_timer_start(osal_timer_handle* handle)
{
    return to_c(osal_timer_start(OSAL_C_CAST(tmr_h, handle)));
}

/// @brief Stop (disarm) a timer; see `osal_timer_stop()`.
/// @param handle Timer handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_timer_stop(osal_timer_handle* handle)
{
    return to_c(osal_timer_stop(OSAL_C_CAST(tmr_h, handle)));
}

/// @brief Restart a timer from zero; see `osal_timer_reset()`.
/// @param handle Timer handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_timer_reset(osal_timer_handle* handle)
{
    return to_c(osal_timer_reset(OSAL_C_CAST(tmr_h, handle)));
}

/// @brief Change a timer's period; see `osal_timer_set_period()`.
/// @param handle           Timer handle.
/// @param new_period_ticks New period in ticks.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_timer_set_period(osal_timer_handle* handle, osal_tick_t new_period_ticks)
{
    return to_c(osal_timer_set_period(OSAL_C_CAST(tmr_h, handle), new_period_ticks));
}

/// @brief Query whether a timer is currently running.
/// @param handle Timer handle (const).
/// @return 1 if the timer is active, 0 otherwise.
extern "C" int osal_c_timer_is_active(const osal_timer_handle* handle)
{
    return osal_timer_is_active(OSAL_C_CAST_CONST(tmr_h, handle)) ? 1 : 0;
}

// ============================================================================
// Event Flags
// ============================================================================

/// @brief Create an event-flags group; see `osal_event_flags_create()`.
/// @param handle Output handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_event_flags_create(osal_event_flags_handle* handle)
{
    return to_c(osal_event_flags_create(OSAL_C_CAST(ef_h, handle)));
}

/// @brief Destroy an event-flags group; see `osal_event_flags_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_event_flags_destroy(osal_event_flags_handle* handle)
{
    return to_c(osal_event_flags_destroy(OSAL_C_CAST(ef_h, handle)));
}

/// @brief Set (OR) event bits in a group; see `osal_event_flags_set()`.
/// @param handle Event-flags handle.
/// @param bits   Bit mask to OR in.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_event_flags_set(osal_event_flags_handle* handle, osal_event_bits_t bits)
{
    return to_c(osal_event_flags_set(OSAL_C_CAST(ef_h, handle), bits));
}

/// @brief Clear (AND-invert) event bits; see `osal_event_flags_clear()`.
/// @param handle Event-flags handle.
/// @param bits   Bit mask to clear.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_event_flags_clear(osal_event_flags_handle* handle, osal_event_bits_t bits)
{
    return to_c(osal_event_flags_clear(OSAL_C_CAST(ef_h, handle), bits));
}

/// @brief Read the current event bits without blocking; see `osal_event_flags_get()`.
/// @param handle Event-flags handle (const).
/// @return Current bit pattern.
extern "C" osal_event_bits_t osal_c_event_flags_get(const osal_event_flags_handle* handle)
{
    return osal_event_flags_get(OSAL_C_CAST_CONST(ef_h, handle));
}

/// @brief Wait until any of @p wait_bits are set; see `osal_event_flags_wait_any()`.
/// @param handle        Event-flags handle.
/// @param wait_bits     Bit mask to wait for (any bit sufficient).
/// @param actual_bits   If non-null, receives the bit value at wakeup.
/// @param clear_on_exit Non-zero to clear matched bits before returning.
/// @param timeout       Ticks to wait; `OSAL_WAIT_FOREVER` for indefinite.
/// @return `OSAL_OK` when matched, `OSAL_TIMEOUT` on expiry.
extern "C" osal_result_t osal_c_event_flags_wait_any(osal_event_flags_handle* handle, osal_event_bits_t wait_bits,
                                                     osal_event_bits_t* actual_bits, int clear_on_exit,
                                                     osal_tick_t timeout)
{
    return to_c(
        osal_event_flags_wait_any(OSAL_C_CAST(ef_h, handle), wait_bits, actual_bits, clear_on_exit != 0, timeout));
}

/// @brief Wait until all of @p wait_bits are set; see `osal_event_flags_wait_all()`.
/// @param handle        Event-flags handle.
/// @param wait_bits     Bit mask to wait for (all bits must be set).
/// @param actual_bits   If non-null, receives the bit value at wakeup.
/// @param clear_on_exit Non-zero to clear matched bits before returning.
/// @param timeout       Ticks to wait; `OSAL_WAIT_FOREVER` for indefinite.
/// @return `OSAL_OK` when all matched, `OSAL_TIMEOUT` on expiry.
extern "C" osal_result_t osal_c_event_flags_wait_all(osal_event_flags_handle* handle, osal_event_bits_t wait_bits,
                                                     osal_event_bits_t* actual_bits, int clear_on_exit,
                                                     osal_tick_t timeout)
{
    return to_c(
        osal_event_flags_wait_all(OSAL_C_CAST(ef_h, handle), wait_bits, actual_bits, clear_on_exit != 0, timeout));
}

/// @brief Set event bits from ISR context; see `osal_event_flags_set_isr()`.
/// @param handle Event-flags handle.
/// @param bits   Bit mask to OR in.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_event_flags_set_isr(osal_event_flags_handle* handle, osal_event_bits_t bits)
{
    return to_c(osal_event_flags_set_isr(OSAL_C_CAST(ef_h, handle), bits));
}

// ============================================================================
// Condition Variable
// ============================================================================

/// @brief Create a condition variable; see `osal_condvar_create()`.
/// @param handle Output handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_condvar_create(osal_condvar_handle* handle)
{
    return to_c(osal_condvar_create(OSAL_C_CAST(cv_h, handle)));
}

/// @brief Destroy a condition variable; see `osal_condvar_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_condvar_destroy(osal_condvar_handle* handle)
{
    return to_c(osal_condvar_destroy(OSAL_C_CAST(cv_h, handle)));
}

extern "C" osal_result_t osal_c_condvar_wait(osal_condvar_handle* handle, osal_mutex_handle* mutex, osal_tick_t timeout)
{
    return to_c(osal_condvar_wait(OSAL_C_CAST(cv_h, handle), OSAL_C_CAST(mtx_h, mutex), timeout));
}

/// @brief Wake one thread waiting on the condition variable.
/// @param handle Condvar handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_condvar_notify_one(osal_condvar_handle* handle)
{
    return to_c(osal_condvar_notify_one(OSAL_C_CAST(cv_h, handle)));
}

/// @brief Wake all threads waiting on the condition variable.
/// @param handle Condvar handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_condvar_notify_all(osal_condvar_handle* handle)
{
    return to_c(osal_condvar_notify_all(OSAL_C_CAST(cv_h, handle)));
}

// ============================================================================
// Work Queue
// ============================================================================

/// @brief Create a work queue; see `osal_work_queue_create()`.
/// @param handle      Output handle.
/// @param stack       Stack buffer for the worker thread.
/// @param stack_bytes Stack size in bytes.
/// @param depth       Maximum number of pending work items.
/// @param name        Optional queue name.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_work_queue_create(osal_work_queue_handle* handle, void* stack, size_t stack_bytes,
                                                  size_t depth, const char* name)
{
    return to_c(osal_work_queue_create(OSAL_C_CAST(wq_h, handle), stack, stack_bytes, depth, name));
}

/// @brief Create a work queue from a config struct; see `osal_work_queue_create()`.
/// @param handle Output handle.
/// @param cfg    Pointer to `osal_work_queue_config`.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_work_queue_create_with_cfg(osal_work_queue_handle*       handle,
                                                           const osal_work_queue_config* cfg)
{
    return to_c(osal_work_queue_create(OSAL_C_CAST(wq_h, handle), cfg->stack, cfg->stack_bytes, cfg->depth, cfg->name));
}

/// @brief Destroy a work queue; see `osal_work_queue_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_work_queue_destroy(osal_work_queue_handle* handle)
{
    return to_c(osal_work_queue_destroy(OSAL_C_CAST(wq_h, handle)));
}

/// @brief Submit a work item from task context; see `osal_work_queue_submit()`.
/// @param handle Work-queue handle.
/// @param func   Function to execute on the queue.
/// @param arg    Argument passed to @p func.
/// @return `OSAL_OK` if submitted, `OSAL_QUEUE_FULL` if queue full.
extern "C" osal_result_t osal_c_work_queue_submit(osal_work_queue_handle* handle, osal_c_work_func_t func, void* arg)
{
    return to_c(osal_work_queue_submit(OSAL_C_CAST(wq_h, handle), func, arg));
}

/// @brief Submit a work item from ISR context; see `osal_work_queue_submit_from_isr()`.
/// @param handle Work-queue handle.
/// @param func   Function to execute.
/// @param arg    Argument passed to @p func.
/// @return `OSAL_OK` if submitted, `OSAL_QUEUE_FULL` if queue full.
extern "C" osal_result_t osal_c_work_queue_submit_from_isr(osal_work_queue_handle* handle, osal_c_work_func_t func,
                                                           void* arg)
{
    return to_c(osal_work_queue_submit_from_isr(OSAL_C_CAST(wq_h, handle), func, arg));
}

/// @brief Block until all currently queued items have been processed.
/// @param handle  Work-queue handle.
/// @param timeout Ticks to wait.
/// @return `OSAL_OK` when drained, `OSAL_TIMEOUT` on expiry.
extern "C" osal_result_t osal_c_work_queue_flush(osal_work_queue_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_work_queue_flush(OSAL_C_CAST(wq_h, handle), timeout));
}

/// @brief Discard all pending work items without executing them.
/// @param handle Work-queue handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_work_queue_cancel_all(osal_work_queue_handle* handle)
{
    return to_c(osal_work_queue_cancel_all(OSAL_C_CAST(wq_h, handle)));
}

/// @brief Query how many items are pending in the work queue.
/// @param handle Work-queue handle (const).
/// @return Number of pending items.
extern "C" size_t osal_c_work_queue_pending(const osal_work_queue_handle* handle)
{
    return osal_work_queue_pending(OSAL_C_CAST_CONST(wq_h, handle));
}

// ============================================================================
// Memory Pool
// ============================================================================

/// @brief Create a fixed-block memory pool; see `osal_memory_pool_create()`.
/// @param handle      Output handle.
/// @param buffer      Caller-supplied backing memory.
/// @param buf_bytes   Total size of @p buffer in bytes.
/// @param block_size  Size in bytes of each block.
/// @param block_count Number of blocks in the pool.
/// @param name        Optional pool name.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_memory_pool_create(osal_memory_pool_handle* handle, void* buffer, size_t buf_bytes,
                                                   size_t block_size, size_t block_count, const char* name)
{
    return to_c(osal_memory_pool_create(OSAL_C_CAST(mp_h, handle), buffer, buf_bytes, block_size, block_count, name));
}

/// @brief Create a memory pool from a config struct; see `osal_memory_pool_create()`.
/// @param handle Output handle.
/// @param cfg    Pointer to `osal_memory_pool_config`.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_memory_pool_create_with_cfg(osal_memory_pool_handle*       handle,
                                                            const osal_memory_pool_config* cfg)
{
    return to_c(osal_memory_pool_create(OSAL_C_CAST(mp_h, handle), cfg->buffer, cfg->buf_bytes, cfg->block_size,
                                        cfg->block_count, cfg->name));
}

/// @brief Destroy a memory pool; see `osal_memory_pool_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_memory_pool_destroy(osal_memory_pool_handle* handle)
{
    return to_c(osal_memory_pool_destroy(OSAL_C_CAST(mp_h, handle)));
}

/// @brief Allocate one block; blocks indefinitely until a block is free.
/// @param handle Pool handle.
/// @return Pointer to the allocated block, or `nullptr` on error.
extern "C" void* osal_c_memory_pool_allocate(osal_memory_pool_handle* handle)
{
    return osal_memory_pool_allocate(OSAL_C_CAST(mp_h, handle));
}

/// @brief Allocate one block with a timeout; see `osal_memory_pool_allocate_timed()`.
/// @param handle  Pool handle.
/// @param timeout Ticks to wait if the pool is empty.
/// @return Pointer to the block, or `nullptr` on timeout/error.
extern "C" void* osal_c_memory_pool_allocate_timed(osal_memory_pool_handle* handle, osal_tick_t timeout)
{
    return osal_memory_pool_allocate_timed(OSAL_C_CAST(mp_h, handle), timeout);
}

/// @brief Return a previously allocated block to the pool.
/// @param handle Pool handle.
/// @param block  Block pointer obtained from `osal_c_memory_pool_allocate`.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_memory_pool_deallocate(osal_memory_pool_handle* handle, void* block)
{
    return to_c(osal_memory_pool_deallocate(OSAL_C_CAST(mp_h, handle), block));
}

/// @brief Query the number of free blocks remaining in the pool.
/// @param handle Pool handle (const).
/// @return Number of available blocks.
extern "C" size_t osal_c_memory_pool_available(const osal_memory_pool_handle* handle)
{
    return osal_memory_pool_available(OSAL_C_CAST_CONST(mp_h, handle));
}

// ============================================================================
// Read-Write Lock
// ============================================================================

/// @brief Create a reader-writer lock; see `osal_rwlock_create()`.
/// @param handle Output handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_rwlock_create(osal_rwlock_handle* handle)
{
    return to_c(osal_rwlock_create(OSAL_C_CAST(rw_h, handle)));
}

/// @brief Destroy a reader-writer lock; see `osal_rwlock_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_rwlock_destroy(osal_rwlock_handle* handle)
{
    return to_c(osal_rwlock_destroy(OSAL_C_CAST(rw_h, handle)));
}

/// @brief Acquire the lock for shared (read) access.
/// @param handle  RWLock handle.
/// @param timeout Ticks to wait.
/// @return `OSAL_OK` on success, `OSAL_TIMEOUT` on expiry.
extern "C" osal_result_t osal_c_rwlock_read_lock(osal_rwlock_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_rwlock_read_lock(OSAL_C_CAST(rw_h, handle), timeout));
}

/// @brief Release shared (read) access.
/// @param handle RWLock handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_rwlock_read_unlock(osal_rwlock_handle* handle)
{
    return to_c(osal_rwlock_read_unlock(OSAL_C_CAST(rw_h, handle)));
}

/// @brief Acquire the lock for exclusive (write) access.
/// @param handle  RWLock handle.
/// @param timeout Ticks to wait.
/// @return `OSAL_OK` on success, `OSAL_TIMEOUT` on expiry.
extern "C" osal_result_t osal_c_rwlock_write_lock(osal_rwlock_handle* handle, osal_tick_t timeout)
{
    return to_c(osal_rwlock_write_lock(OSAL_C_CAST(rw_h, handle), timeout));
}

/// @brief Release exclusive (write) access.
/// @param handle RWLock handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_rwlock_write_unlock(osal_rwlock_handle* handle)
{
    return to_c(osal_rwlock_write_unlock(OSAL_C_CAST(rw_h, handle)));
}

// ============================================================================
// Stream Buffer
// ============================================================================

/// @brief Create a byte-stream buffer; see `osal_stream_buffer_create()`.
/// @param handle        Output handle.
/// @param buffer        Caller-supplied backing memory.
/// @param capacity      Total buffer capacity in bytes.
/// @param trigger_level Minimum bytes before a blocked receiver wakes.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_stream_buffer_create(osal_stream_buffer_handle* handle, void* buffer, size_t capacity,
                                                     size_t trigger_level)
{
    return to_c(osal_stream_buffer_create(OSAL_C_CAST(sb_h, handle), buffer, capacity, trigger_level));
}

/// @brief Create a stream buffer from a config struct.
/// @param handle Output handle.
/// @param cfg    Pointer to `osal_stream_buffer_config`.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_stream_buffer_create_with_cfg(osal_stream_buffer_handle*       handle,
                                                              const osal_stream_buffer_config* cfg)
{
    return to_c(osal_stream_buffer_create(OSAL_C_CAST(sb_h, handle), cfg->buffer, cfg->capacity, cfg->trigger_level));
}

/// @brief Destroy a stream buffer; see `osal_stream_buffer_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_stream_buffer_destroy(osal_stream_buffer_handle* handle)
{
    return to_c(osal_stream_buffer_destroy(OSAL_C_CAST(sb_h, handle)));
}

/// @brief Write bytes into the stream buffer from task context.
/// @param handle  Stream-buffer handle.
/// @param data    Data to write.
/// @param len     Number of bytes to write.
/// @param timeout Ticks to wait if there is insufficient free space.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_stream_buffer_send(osal_stream_buffer_handle* handle, const void* data, size_t len,
                                                   osal_tick_t timeout)
{
    return to_c(osal_stream_buffer_send(OSAL_C_CAST(sb_h, handle), data, len, timeout));
}

/// @brief Write bytes into the stream buffer from ISR context.
/// @param handle Stream-buffer handle.
/// @param data   Data to write.
/// @param len    Number of bytes to write.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_stream_buffer_send_isr(osal_stream_buffer_handle* handle, const void* data, size_t len)
{
    return to_c(osal_stream_buffer_send_isr(OSAL_C_CAST(sb_h, handle), data, len));
}
/// @brief Read bytes from the stream buffer from task context.
/// @param handle  Stream-buffer handle.
/// @param data    Destination buffer.
/// @param size    Maximum bytes to read.
/// @param timeout Ticks to wait if the buffer is empty.
/// @return Number of bytes actually read.
extern "C" size_t osal_c_stream_buffer_receive(osal_stream_buffer_handle* handle, void* buf, size_t max_len,
                                               osal_tick_t timeout)
{
    return osal_stream_buffer_receive(OSAL_C_CAST(sb_h, handle), buf, max_len, timeout);
}

/// @brief Read bytes from the stream buffer from ISR context.
/// @param handle Stream-buffer handle.
/// @param buf    Destination buffer.
/// @param max_len Maximum bytes to read.
/// @return Number of bytes actually read.
extern "C" size_t osal_c_stream_buffer_receive_isr(osal_stream_buffer_handle* handle, void* buf, size_t max_len)
{
    return osal_stream_buffer_receive_isr(OSAL_C_CAST(sb_h, handle), buf, max_len);
}

/// @brief Query bytes available to read in the stream buffer.
/// @param handle Stream-buffer handle (const).
/// @return Number of readable bytes.
extern "C" size_t osal_c_stream_buffer_available(const osal_stream_buffer_handle* handle)
{
    return osal_stream_buffer_available(OSAL_C_CAST_CONST(sb_h, handle));
}

/// @brief Query free space remaining in the stream buffer.
/// @param handle Stream-buffer handle (const).
/// @return Number of bytes that can still be written without blocking.
extern "C" size_t osal_c_stream_buffer_free_space(const osal_stream_buffer_handle* handle)
{
    return osal_stream_buffer_free_space(OSAL_C_CAST_CONST(sb_h, handle));
}

/// @brief Reset the stream buffer to empty, discarding all contents.
/// @param handle Stream-buffer handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_stream_buffer_reset(osal_stream_buffer_handle* handle)
{
    return to_c(osal_stream_buffer_reset(OSAL_C_CAST(sb_h, handle)));
}

// ============================================================================
// Message Buffer
// ============================================================================

/// @brief Create a framed message buffer; see `osal_message_buffer_create()`.
/// @param handle   Output handle.
/// @param buffer   Caller-supplied backing memory.
/// @param capacity Total buffer capacity in bytes (framing overhead included).
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_message_buffer_create(osal_message_buffer_handle* handle, void* buffer, size_t capacity)
{
    return to_c(osal_message_buffer_create(OSAL_C_CAST(mb_h, handle), buffer, capacity));
}

/// @brief Create a message buffer from a config struct.
/// @param handle Output handle.
/// @param cfg    Pointer to `osal_message_buffer_config`.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_message_buffer_create_with_cfg(osal_message_buffer_handle*       handle,
                                                               const osal_message_buffer_config* cfg)
{
    return to_c(osal_message_buffer_create(OSAL_C_CAST(mb_h, handle), cfg->buffer, cfg->capacity));
}

/// @brief Destroy a message buffer; see `osal_message_buffer_destroy()`.
/// @param handle Handle to destroy.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_message_buffer_destroy(osal_message_buffer_handle* handle)
{
    return to_c(osal_message_buffer_destroy(OSAL_C_CAST(mb_h, handle)));
}

/// @brief Write one complete message from task context.
/// @param handle  Message-buffer handle.
/// @param msg     Message payload.
/// @param len     Payload size in bytes.
/// @param timeout Ticks to wait if there is insufficient space.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_message_buffer_send(osal_message_buffer_handle* handle, const void* msg, size_t len,
                                                    osal_tick_t timeout)
{
    return to_c(osal_message_buffer_send(OSAL_C_CAST(mb_h, handle), msg, len, timeout));
}

/// @brief Write one complete message from ISR context.
/// @param handle Message-buffer handle.
/// @param msg    Message payload.
/// @param len    Payload size in bytes.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_message_buffer_send_isr(osal_message_buffer_handle* handle, const void* msg, size_t len)
{
    return to_c(osal_message_buffer_send_isr(OSAL_C_CAST(mb_h, handle), msg, len));
}
/// @brief Read one complete message from task context.
/// @param handle  Message-buffer handle.
/// @param data    Destination buffer.
/// @param size    Maximum bytes to read (should be ≥ largest message).
/// @param timeout Ticks to wait if the buffer is empty.
/// @return Number of bytes in the received message (0 on timeout/error).
extern "C" size_t osal_c_message_buffer_receive(osal_message_buffer_handle* handle, void* buf, size_t max_len,
                                                osal_tick_t timeout)
{
    return osal_message_buffer_receive(OSAL_C_CAST(mb_h, handle), buf, max_len, timeout);
}

/// @brief Read one complete message from ISR context.
/// @param handle  Message-buffer handle.
/// @param buf     Destination buffer.
/// @param max_len Maximum bytes to read.
/// @return Number of bytes in the received message (0 if no complete message).
extern "C" size_t osal_c_message_buffer_receive_isr(osal_message_buffer_handle* handle, void* buf, size_t max_len)
{
    return osal_message_buffer_receive_isr(OSAL_C_CAST(mb_h, handle), buf, max_len);
}

/// @brief Query bytes available to read in the message buffer.
/// @param handle Message-buffer handle (const).
/// @return Number of readable bytes (includes framing).
extern "C" size_t osal_c_message_buffer_available(const osal_message_buffer_handle* handle)
{
    return osal_message_buffer_available(OSAL_C_CAST_CONST(mb_h, handle));
}

/// @brief Query free space remaining in the message buffer.
/// @param handle Message-buffer handle (const).
/// @return Number of bytes that can still be written without blocking.
extern "C" size_t osal_c_message_buffer_free_space(const osal_message_buffer_handle* handle)
{
    return osal_message_buffer_free_space(OSAL_C_CAST_CONST(mb_h, handle));
}

/// @brief Reset the message buffer to empty, discarding all messages.
/// @param handle Message-buffer handle.
/// @return `OSAL_OK` on success.
extern "C" osal_result_t osal_c_message_buffer_reset(osal_message_buffer_handle* handle)
{
    return to_c(osal_message_buffer_reset(OSAL_C_CAST(mb_h, handle)));
}
