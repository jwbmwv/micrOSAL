// SPDX-License-Identifier: Apache-2.0
/// @file capabilities.hpp
/// @brief Compile-time capability detection for OSAL backends
/// @details Each backend specialises osal::capabilities<Backend> to advertise
///          what primitives and operations it exposes through MicrOSAL. Flags
///          without the @c native_ prefix may be satisfied by shared emulations.
///          Client code can conditionally enable features with @c if constexpr, while the
///          C++20 concept @c osal::backend_capabilities_spec validates that each
///          backend exposes the full flag set with short diagnostics.
///
///          Example:
///          @code
///          if constexpr (osal::capabilities<osal::active_backend>::has_recursive_mutex) {
///              // use native recursive mutex
///          } else {
///              // fall back to emulated version
///          }
///          @endcode
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_core
#pragma once

#include <cstdint>

namespace osal
{

/// @defgroup osal_capabilities OSAL Capabilities
/// @brief Compile-time feature flags per backend.
/// @{

// ---------------------------------------------------------------------------
// Backend tag types (forward declarations)
// ---------------------------------------------------------------------------

/// @brief Tag for the FreeRTOS backend.
struct backend_freertos
{
};
/// @brief Tag for the Zephyr RTOS backend.
struct backend_zephyr
{
};
/// @brief Tag for the ThreadX/Azure RTOS backend.
struct backend_threadx
{
};
/// @brief Tag for the PX5 RTOS backend.
struct backend_px5
{
};
/// @brief Tag for the POSIX (pthreads) backend.
struct backend_posix
{
};
/// @brief Tag for the Linux native API backend.
struct backend_linux
{
};
/// @brief Tag for the bare-metal (no OS) backend.
struct backend_baremetal
{
};
/// @brief Tag for the VxWorks backend.
struct backend_vxworks
{
};
/// @brief Tag for the NuttX RTOS backend.
struct backend_nuttx
{
};
/// @brief Tag for the Micrium µC/OS-III backend.
struct backend_micrium
{
};
/// @brief Tag for the ChibiOS/RT backend.
struct backend_chibios
{
};
/// @brief Tag for the SEGGER embOS backend.
struct backend_embos
{
};
/// @brief Tag for the QNX Neutrino backend.
struct backend_qnx
{
};
/// @brief Tag for the RTEMS backend.
struct backend_rtems
{
};
/// @brief Tag for the INTEGRITY RTOS backend.
struct backend_integrity
{
};
/// @brief Tag for the CMSIS-RTOS v1 backend.
struct backend_cmsis_rtos
{
};
/// @brief Tag for the CMSIS-RTOS2 (v2) backend.
struct backend_cmsis_rtos2
{
};

// ---------------------------------------------------------------------------
// Native thread-local data capability trait
// ---------------------------------------------------------------------------

/// @brief Backend-native TLS capability (separate from emulated TLS support).
template<typename Backend>
struct native_thread_local_data_capability
{
    static constexpr bool value = false;
};

template<>
struct native_thread_local_data_capability<backend_posix>
{
    static constexpr bool value = true;
};

template<>
struct native_thread_local_data_capability<backend_linux>
{
    static constexpr bool value = true;
};

template<>
struct native_thread_local_data_capability<backend_nuttx>
{
    static constexpr bool value = true;
};

template<>
struct native_thread_local_data_capability<backend_qnx>
{
    static constexpr bool value = true;
};

template<>
struct native_thread_local_data_capability<backend_rtems>
{
    static constexpr bool value = true;
};

template<>
struct native_thread_local_data_capability<backend_integrity>
{
    static constexpr bool value = true;
};

template<>
struct native_thread_local_data_capability<backend_freertos>
{
#if defined(configNUM_THREAD_LOCAL_STORAGE_POINTERS) && (configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0)
    static constexpr bool value = true;
#else
    static constexpr bool value = false;
#endif
};

/// @brief True when a backend has a native TLS primitive available.
template<typename Backend>
inline constexpr bool has_native_thread_local_data = native_thread_local_data_capability<Backend>::value;

// ---------------------------------------------------------------------------
// capabilities primary template — all features disabled by default
// ---------------------------------------------------------------------------

/// @brief Compile-time capability descriptor for an OSAL backend.
/// @tparam Backend  One of the osal::backend_* tag types.
/// @details All fields are @c static constexpr bool.  Un-specialised backends
///          have all features disabled; specialisations opt-in below.
template<typename Backend>
struct capabilities
{
    // ----- mutex -----------------------------------------------------------
    /// @brief Backend provides native recursive mutex (no emulation needed).
    static constexpr bool has_recursive_mutex = false;
    /// @brief Mutex lock supports a timeout argument.
    static constexpr bool has_timed_mutex = false;
    /// @brief Mutex implementation provides priority-inheritance to prevent priority inversion.
    static constexpr bool has_priority_inheritance = false;

    // ----- semaphore -------------------------------------------------------
    /// @brief Semaphore give/take can be called from ISR context.
    static constexpr bool has_isr_semaphore = false;
    /// @brief Semaphore supports timed take (take_for / take_until).
    static constexpr bool has_timed_semaphore = false;

    // ----- thread ----------------------------------------------------------
    /// @brief Thread creation supports CPU affinity setting.
    static constexpr bool has_thread_affinity = false;
    /// @brief Thread join supports a timeout (join_for / join_until).
    static constexpr bool has_timed_join = false;
    /// @brief Backend provides native thread suspend/resume operations.
    static constexpr bool has_thread_suspend_resume = false;
    /// @brief Backend supports thread-local key/value storage.
    static constexpr bool has_thread_local_data = true;
    /// @brief Thread priority can be changed at runtime.
    static constexpr bool has_dynamic_thread_priority = false;
    /// @brief Backend provides direct-to-task lightweight notifications (no queue or semaphore).
    static constexpr bool has_task_notification = false;
    /// @brief Backend provides runtime stack-overflow detection (canary or watermark).
    static constexpr bool has_stack_overflow_detection = false;
    /// @brief Backend exposes per-task CPU-load / run-time statistics.
    static constexpr bool has_cpu_load_stats = false;

    // ----- queue -----------------------------------------------------------
    /// @brief Queue send/receive can be called from ISR context.
    static constexpr bool has_isr_queue = false;
    /// @brief Queue supports timed send/receive.
    static constexpr bool has_timed_queue = false;

    // ----- timer -----------------------------------------------------------
    /// @brief Backend provides native software timers.
    static constexpr bool has_timer = false;
    /// @brief Timer callback can be configured as periodic (auto-reload).
    static constexpr bool has_periodic_timer = false;
    /// @brief Timer expiry callbacks may run in ISR/interrupt context.
    static constexpr bool timer_callbacks_may_run_in_isr = false;

    // ----- event flags -----------------------------------------------------
    /// @brief Backend provides native event flag groups.
    static constexpr bool has_native_event_flags = false;
    /// @brief Event flags support ISR-safe set/clear.
    static constexpr bool has_isr_event_flags = false;
    /// @brief Event flags support timed wait.
    static constexpr bool has_timed_event_flags = false;

    // ----- wait-set --------------------------------------------------------
    /// @brief Backend provides a native wait-set / poll mechanism.
    static constexpr bool has_wait_set = false;

    // ----- condition variable -----------------------------------------------
    /// @brief Backend provides a native condition variable.
    static constexpr bool has_native_condvar = false;

    // ----- work queue ------------------------------------------------------
    /// @brief Backend provides a native work queue primitive.
    static constexpr bool has_native_work_queue = false;
    /// @brief Work queue submit supports ISR-safe submission.
    static constexpr bool has_isr_work_queue_submit = false;

    // ----- memory pool -----------------------------------------------------
    /// @brief Backend provides a native fixed-size block pool allocator.
    static constexpr bool has_native_memory_pool = false;

    // ----- stream / message buffer -----------------------------------------
    /// @brief Backend provides native stream buffers (e.g. FreeRTOS xStreamBuffer).
    static constexpr bool has_native_stream_buffer = false;
    /// @brief Stream/message buffer send can be called from ISR context.
    static constexpr bool has_isr_stream_buffer = false;
    /// @brief Backend provides native message buffers (length-prefixed, distinct from stream buffers).
    static constexpr bool has_native_message_buffer = false;
    /// @brief Message buffer send can be called from ISR context.
    static constexpr bool has_isr_message_buffer = false;

    // ----- read-write lock -------------------------------------------------
    /// @brief Backend provides a native read-write lock.
    static constexpr bool has_native_rwlock = false;

    // ----- spinlock --------------------------------------------------------
    /// @brief Backend provides a native spinlock primitive.
    static constexpr bool has_spinlock = false;

    // ----- barrier ---------------------------------------------------------
    /// @brief Backend provides a thread barrier / rendezvous point.
    static constexpr bool has_barrier = false;

    // ----- clock -----------------------------------------------------------
    /// @brief Backend provides a monotonic clock with at least ms resolution.
    static constexpr bool has_monotonic_clock = false;
    /// @brief Backend provides a system (wall-clock) source.
    static constexpr bool has_system_clock = false;
    /// @brief Clock tick resolution is <= 1 ms.
    static constexpr bool has_high_resolution = false;
};

// ---------------------------------------------------------------------------
// capabilities specialisations — one per backend
// ---------------------------------------------------------------------------

/// @brief FreeRTOS capability flags.
template<>
struct capabilities<backend_freertos>
{
    static constexpr bool has_recursive_mutex      = true;
    static constexpr bool has_timed_mutex          = true;
    static constexpr bool has_priority_inheritance = true;  // xSemaphoreCreateMutex uses PI
    static constexpr bool has_isr_semaphore        = true;
    static constexpr bool has_timed_semaphore      = true;
    static constexpr bool has_thread_affinity      = false;  // SMP FreeRTOS only
    static constexpr bool has_timed_join           = false;
#if defined(INCLUDE_vTaskSuspend) && (INCLUDE_vTaskSuspend == 1)
    static constexpr bool has_thread_suspend_resume = true;
#else
    static constexpr bool has_thread_suspend_resume = false;
#endif
    static constexpr bool has_thread_local_data       = true;
    static constexpr bool has_dynamic_thread_priority = true;  // vTaskPrioritySet
    static constexpr bool has_task_notification       = true;  // xTaskNotify / xTaskNotifyFromISR
#if defined(configCHECK_FOR_STACK_OVERFLOW) && (configCHECK_FOR_STACK_OVERFLOW > 0)
    static constexpr bool has_stack_overflow_detection = true;
#else
    static constexpr bool has_stack_overflow_detection = false;
#endif
#if defined(configGENERATE_RUN_TIME_STATS) && (configGENERATE_RUN_TIME_STATS == 1)
    static constexpr bool has_cpu_load_stats = true;
#else
    static constexpr bool has_cpu_load_stats = false;
#endif
    static constexpr bool has_isr_queue                  = true;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = true;
    static constexpr bool has_isr_event_flags            = true;
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = false;
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = false;
    static constexpr bool has_native_stream_buffer       = true;
    static constexpr bool has_isr_stream_buffer          = true;
    static constexpr bool has_native_message_buffer      = true;  // xMessageBufferCreate
    static constexpr bool has_isr_message_buffer         = true;  // xMessageBufferSendFromISR
    static constexpr bool has_native_rwlock              = false;
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = false;
    static constexpr bool has_high_resolution            = false;
};

/// @brief Zephyr RTOS capability flags.
template<>
struct capabilities<backend_zephyr>
{
    static constexpr bool has_recursive_mutex            = true;  // k_mutex is recursive by default
    static constexpr bool has_timed_mutex                = true;
    static constexpr bool has_priority_inheritance       = true;  // k_mutex uses PI by design
    static constexpr bool has_isr_semaphore              = true;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = true;
    static constexpr bool has_timed_join                 = true;
    static constexpr bool has_thread_suspend_resume      = true;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // k_thread_priority_set
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = true;  // CONFIG_THREAD_STACK_INFO canary
    static constexpr bool has_cpu_load_stats             = true;  // CONFIG_THREAD_RUNTIME_STATS
    static constexpr bool has_isr_queue                  = true;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = true;
    static constexpr bool has_native_event_flags         = true;  // k_event
    static constexpr bool has_isr_event_flags            = true;  // k_event_post ISR-safe
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = true;  // k_condvar
    static constexpr bool has_native_work_queue          = true;  // k_work_queue
    static constexpr bool has_isr_work_queue_submit      = true;
    static constexpr bool has_native_memory_pool         = false;  // emulated via bitmap + sync primitives
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = false;
    static constexpr bool has_spinlock                   = true;  // k_spinlock
    static constexpr bool has_barrier                    = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = true;
    static constexpr bool has_high_resolution            = true;
};

/// @brief ThreadX / Azure RTOS capability flags.
template<>
struct capabilities<backend_threadx>
{
    static constexpr bool has_recursive_mutex            = true;
    static constexpr bool has_timed_mutex                = true;
    static constexpr bool has_priority_inheritance       = true;  // TX_MUTEX_INHERIT
    static constexpr bool has_isr_semaphore              = true;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = false;
    static constexpr bool has_timed_join                 = false;
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // tx_thread_priority_change
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = false;
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = true;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = true;
    static constexpr bool has_isr_event_flags            = true;
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = false;
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = true;  // tx_block_pool
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = false;
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = false;
    static constexpr bool has_high_resolution            = false;
};

/// @brief PX5 RTOS capability flags.
template<>
struct capabilities<backend_px5>
{
    static constexpr bool has_recursive_mutex            = true;
    static constexpr bool has_timed_mutex                = true;
    static constexpr bool has_priority_inheritance       = true;  // PX5 inherits ThreadX mutex PI
    static constexpr bool has_isr_semaphore              = true;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = false;
    static constexpr bool has_timed_join                 = false;
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // px5_thread_priority_change
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = false;
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = true;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = true;
    static constexpr bool has_isr_event_flags            = true;
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = true;
    static constexpr bool has_native_condvar             = false;
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = true;  // tx_block_pool
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = false;
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = false;
    static constexpr bool has_high_resolution            = false;
};

/// @brief POSIX (pthreads) capability flags.
template<>
struct capabilities<backend_posix>
{
    static constexpr bool has_recursive_mutex            = true;
    static constexpr bool has_timed_mutex                = true;
    static constexpr bool has_priority_inheritance       = true;  // PTHREAD_PRIO_INHERIT
    static constexpr bool has_isr_semaphore              = false;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = false;
    static constexpr bool has_timed_join                 = false;
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // pthread_setschedparam
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = false;
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = false;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = false;
    static constexpr bool has_isr_event_flags            = false;
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = true;  // poll()
    static constexpr bool has_native_condvar             = true;  // pthread_cond_t
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = false;
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = true;  // pthread_rwlock_t
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // pthread_barrier_t
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = true;
    static constexpr bool has_high_resolution            = true;
};

/// @brief Linux native API capability flags.
template<>
struct capabilities<backend_linux>
{
    static constexpr bool has_recursive_mutex            = true;
    static constexpr bool has_timed_mutex                = true;
    static constexpr bool has_priority_inheritance       = true;  // PTHREAD_PRIO_INHERIT
    static constexpr bool has_isr_semaphore              = false;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = true;  // sched_setaffinity
    static constexpr bool has_timed_join                 = false;
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // pthread_setschedparam
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = false;
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = false;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = false;
    static constexpr bool has_isr_event_flags            = false;
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = true;  // epoll
    static constexpr bool has_native_condvar             = true;  // pthread_cond_t
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = false;
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = true;  // pthread_rwlock_t
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // pthread_barrier_t
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = true;
    static constexpr bool has_high_resolution            = true;
};

/// @brief RTEMS capability flags.
template<>
struct capabilities<backend_rtems> : capabilities<backend_posix>
{
};

/// @brief INTEGRITY RTOS capability flags.
template<>
struct capabilities<backend_integrity> : capabilities<backend_posix>
{
};

/// @brief Bare-metal capability flags.
template<>
struct capabilities<backend_baremetal>
{
    static constexpr bool has_recursive_mutex          = false;
    static constexpr bool has_timed_mutex              = true;
    static constexpr bool has_priority_inheritance     = false;
    static constexpr bool has_isr_semaphore            = true;
    static constexpr bool has_timed_semaphore          = true;
    static constexpr bool has_thread_affinity          = false;
    static constexpr bool has_timed_join               = true;
    static constexpr bool has_thread_suspend_resume    = false;
    static constexpr bool has_thread_local_data        = true;
    static constexpr bool has_dynamic_thread_priority  = false;
    static constexpr bool has_task_notification        = false;
    static constexpr bool has_stack_overflow_detection = false;
    static constexpr bool has_cpu_load_stats           = false;
    static constexpr bool has_isr_queue                = true;
    static constexpr bool has_timed_queue              = true;
    static constexpr bool has_timer                    = true;
    static constexpr bool has_periodic_timer           = true;
#if defined(OSAL_BM_TEST_SELF_TICK)
    static constexpr bool timer_callbacks_may_run_in_isr = false;
#else
    static constexpr bool timer_callbacks_may_run_in_isr = true;
#endif
    static constexpr bool has_native_event_flags    = false;
    static constexpr bool has_isr_event_flags       = true;
    static constexpr bool has_timed_event_flags     = true;
    static constexpr bool has_wait_set              = false;
    static constexpr bool has_native_condvar        = false;
    static constexpr bool has_native_work_queue     = false;
    static constexpr bool has_isr_work_queue_submit = false;
    static constexpr bool has_native_memory_pool    = false;
    static constexpr bool has_native_stream_buffer  = false;
    static constexpr bool has_isr_stream_buffer     = false;
    static constexpr bool has_native_message_buffer = false;
    static constexpr bool has_isr_message_buffer    = false;
    static constexpr bool has_native_rwlock         = false;
    static constexpr bool has_spinlock              = false;
    static constexpr bool has_barrier               = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock       = true;
    static constexpr bool has_system_clock          = false;
    static constexpr bool has_high_resolution       = false;
};

/// @brief VxWorks capability flags.
template<>
struct capabilities<backend_vxworks>
{
    static constexpr bool has_recursive_mutex            = true;
    static constexpr bool has_timed_mutex                = true;
    static constexpr bool has_priority_inheritance       = true;  // SEM_INVERSION_SAFE
    static constexpr bool has_isr_semaphore              = true;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = false;
    static constexpr bool has_timed_join                 = false;
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // taskPrioritySet
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = false;
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = true;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = true;
    static constexpr bool has_native_event_flags         = false;  // per-task events, not per-object
    static constexpr bool has_isr_event_flags            = true;   // emulated via semaphore_give_isr
    static constexpr bool has_timed_event_flags          = true;   // emulated via semaphore timed-take
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = true;  // condVarLib
    static constexpr bool has_native_work_queue          = true;  // jobQueueLib
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = true;  // memPartLib
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = false;
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = false;
    static constexpr bool has_high_resolution            = false;
};

/// @brief NuttX capability flags.
template<>
struct capabilities<backend_nuttx>
{
    static constexpr bool has_recursive_mutex            = true;
    static constexpr bool has_timed_mutex                = true;
    static constexpr bool has_priority_inheritance       = true;  // pthread_mutexattr_setprotocol
    static constexpr bool has_isr_semaphore              = false;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = false;
    static constexpr bool has_timed_join                 = false;
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // pthread_setschedparam
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = true;  // CONFIG_STACK_CANARIES
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = false;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = false;  // condvar-based
    static constexpr bool has_isr_event_flags            = false;
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = true;  // pthread_cond_t
    static constexpr bool has_native_work_queue          = true;  // work_queue()
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = false;
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = true;  // pthread_rwlock_t
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // pthread_barrier_t
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = true;
    static constexpr bool has_high_resolution            = true;
};

/// @brief Micrium µC/OS-III capability flags.
template<>
struct capabilities<backend_micrium>
{
    static constexpr bool has_recursive_mutex            = true;
    static constexpr bool has_timed_mutex                = true;
    static constexpr bool has_priority_inheritance       = true;  // OS_MUTEX_Create uses PI
    static constexpr bool has_isr_semaphore              = true;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = false;
    static constexpr bool has_timed_join                 = false;
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // OSTaskChangePrio
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = false;
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = true;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = true;
    static constexpr bool has_isr_event_flags            = true;
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = false;
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = true;  // OSMemCreate
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = false;
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = false;
    static constexpr bool has_high_resolution            = false;
};

/// @brief ChibiOS/RT capability flags.
template<>
struct capabilities<backend_chibios>
{
    static constexpr bool has_recursive_mutex            = false;  // ChibiOS mutexes are non-recursive
    static constexpr bool has_timed_mutex                = false;  // lock or try-lock only
    static constexpr bool has_priority_inheritance       = true;   // chMtxLock uses PI by design
    static constexpr bool has_isr_semaphore              = true;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = false;
    static constexpr bool has_timed_join                 = false;
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // chThdSetPriority
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = false;
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = true;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = true;
    static constexpr bool has_native_event_flags         = true;
    static constexpr bool has_isr_event_flags            = true;
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = true;  // chCondInit
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = true;  // chPoolInit
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = false;
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = false;
    static constexpr bool has_high_resolution            = true;
};

/// @brief SEGGER embOS capability flags.
template<>
struct capabilities<backend_embos>
{
    static constexpr bool has_recursive_mutex            = true;
    static constexpr bool has_timed_mutex                = true;
    static constexpr bool has_priority_inheritance       = true;  // embOS mutexes use PI by default
    static constexpr bool has_isr_semaphore              = true;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = false;
    static constexpr bool has_timed_join                 = false;
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // OS_TASK_SetPriority
    static constexpr bool has_task_notification          = true;  // OS_TASKEVENT direct task signalling
    static constexpr bool has_stack_overflow_detection   = true;  // OS_STACK_CHECK
    static constexpr bool has_cpu_load_stats             = true;  // OS_GetTaskLoad
    static constexpr bool has_isr_queue                  = true;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = true;
    static constexpr bool has_isr_event_flags            = true;
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = false;
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = true;  // OS_MEMPOOL
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = false;
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = false;
    static constexpr bool has_high_resolution            = true;
};

/// @brief QNX Neutrino capability flags.
template<>
struct capabilities<backend_qnx>
{
    static constexpr bool has_recursive_mutex            = true;
    static constexpr bool has_timed_mutex                = true;
    static constexpr bool has_priority_inheritance       = true;  // PTHREAD_PRIO_INHERIT
    static constexpr bool has_isr_semaphore              = false;
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = true;  // ThreadCtl runmask
    static constexpr bool has_timed_join                 = true;  // pthread_timedjoin_monotonic
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // pthread_setschedparam
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = false;
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = false;
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = false;  // condvar-based
    static constexpr bool has_isr_event_flags            = false;
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = true;  // pthread_cond_t
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = false;
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = true;  // pthread_rwlock_t
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // pthread_barrier_t
    static constexpr bool has_monotonic_clock            = true;
    static constexpr bool has_system_clock               = true;
    static constexpr bool has_high_resolution            = true;
};

/// @brief CMSIS-RTOS v1 capability flags.
template<>
struct capabilities<backend_cmsis_rtos>
{
    static constexpr bool has_recursive_mutex            = true;  // osMutexRecursive attr
    static constexpr bool has_timed_mutex                = true;  // osMutexWait with timeout
    static constexpr bool has_priority_inheritance       = true;  // osMutexPrioInherit attr
    static constexpr bool has_isr_semaphore              = true;  // osSemaphoreRelease from ISR
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = false;
    static constexpr bool has_timed_join                 = false;
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // osThreadSetPriority
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = false;
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = true;  // osMessagePut from ISR
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = false;  // osSignal is per-thread
    static constexpr bool has_isr_event_flags            = true;   // emulated via semaphore_give_isr
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = false;
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = true;  // osPoolCreate
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = false;
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock            = true;  // osKernelSysTick
    static constexpr bool has_system_clock               = false;
    static constexpr bool has_high_resolution            = false;
};

/// @brief CMSIS-RTOS2 (v2) capability flags.
template<>
struct capabilities<backend_cmsis_rtos2>
{
    static constexpr bool has_recursive_mutex            = true;  // osMutexRecursive
    static constexpr bool has_timed_mutex                = true;  // osMutexAcquire with timeout
    static constexpr bool has_priority_inheritance       = true;  // osMutexPrioInherit attr
    static constexpr bool has_isr_semaphore              = true;  // osSemaphoreRelease from ISR
    static constexpr bool has_timed_semaphore            = true;
    static constexpr bool has_thread_affinity            = false;
    static constexpr bool has_timed_join                 = false;  // osThreadJoin has no timeout
    static constexpr bool has_thread_suspend_resume      = false;
    static constexpr bool has_thread_local_data          = true;
    static constexpr bool has_dynamic_thread_priority    = true;  // osThreadSetPriority
    static constexpr bool has_task_notification          = false;
    static constexpr bool has_stack_overflow_detection   = false;
    static constexpr bool has_cpu_load_stats             = false;
    static constexpr bool has_isr_queue                  = true;  // osMessageQueuePut from ISR
    static constexpr bool has_timed_queue                = true;
    static constexpr bool has_timer                      = true;
    static constexpr bool has_periodic_timer             = true;
    static constexpr bool timer_callbacks_may_run_in_isr = false;
    static constexpr bool has_native_event_flags         = true;  // osEventFlags*
    static constexpr bool has_isr_event_flags            = true;  // osEventFlagsSet from ISR
    static constexpr bool has_timed_event_flags          = true;
    static constexpr bool has_wait_set                   = false;
    static constexpr bool has_native_condvar             = false;
    static constexpr bool has_native_work_queue          = false;
    static constexpr bool has_isr_work_queue_submit      = false;
    static constexpr bool has_native_memory_pool         = true;  // osMemoryPool*
    static constexpr bool has_native_stream_buffer       = false;
    static constexpr bool has_isr_stream_buffer          = false;
    static constexpr bool has_native_message_buffer      = false;
    static constexpr bool has_isr_message_buffer         = false;
    static constexpr bool has_native_rwlock              = false;
    static constexpr bool has_spinlock                   = false;
    static constexpr bool has_barrier                    = true;  // shared emulated barrier
    static constexpr bool has_monotonic_clock            = true;  // osKernelGetTickCount
    static constexpr bool has_system_clock               = false;
    static constexpr bool has_high_resolution            = false;
};

/// @} // osal_capabilities

}  // namespace osal
