// SPDX-License-Identifier: Apache-2.0
/// @file backend_traits.hpp
/// @brief Per-backend native handle and function pointer types
/// @details Every backend specialises osal::backend_traits<Backend> to expose:
///          - Native OS handle typedefs for each primitive.
///          - A static init/deinit pair.
///          - Backend name string for diagnostics.
///
///          The C++20 concept @c osal::backend_traits_spec validates that each
///          backend provides the required handle wrappers and metadata while
///          leaving the underlying handle ABI unchanged.
///
///          Implementation (.cpp) files fill in the function bodies; this header
///          only declares the interface contract.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_core
#pragma once

#include "capabilities.hpp"
#include "types.hpp"
#include "error.hpp"
#include <cstddef>
#include <cstdint>

namespace osal
{

/// @defgroup osal_traits OSAL Backend Traits
/// @brief Per-backend opaque handle and metadata types.
/// @{

// ---------------------------------------------------------------------------
// Primary template — intentionally incomplete (no default implementation)
// ---------------------------------------------------------------------------

/// @brief Backend trait descriptor.
/// @tparam Backend  One of the osal::backend_* tag types.
/// @details Specialisations must provide all typedefs listed below.
///          Incomplete instantiation is a hard compile error: if you see
///          "incomplete type" here, you have not included the correct backend
///          header, or OSAL_BACKEND_* is not defined.
template<typename Backend>
struct backend_traits;  // intentionally incomplete

// ---------------------------------------------------------------------------
// FreeRTOS specialisation
// ---------------------------------------------------------------------------

/// @brief FreeRTOS backend traits.
template<>
struct backend_traits<backend_freertos>
{
    // ---- name ----
    static constexpr const char* name = "FreeRTOS";

    // ---- thread ----
    /// @brief Opaque FreeRTOS task handle (TaskHandle_t).
    struct thread_handle_t
    {
        void* native;
    };
    static constexpr stack_size_t default_stack_bytes = 2048U;

    // ---- mutex ----
    /// @brief Opaque FreeRTOS mutex / recursive mutex handle (SemaphoreHandle_t).
    struct mutex_handle_t
    {
        void* native;
    };

    // ---- semaphore ----
    /// @brief Opaque FreeRTOS semaphore handle (SemaphoreHandle_t).
    struct semaphore_handle_t
    {
        void* native;
    };

    // ---- queue ----
    /// @brief Opaque FreeRTOS queue handle (QueueHandle_t).
    struct queue_handle_t
    {
        void* native;
    };

    // ---- timer ----
    /// @brief Opaque FreeRTOS timer handle (TimerHandle_t).
    struct timer_handle_t
    {
        void* native;
    };

    // ---- event flags ----
    /// @brief Opaque FreeRTOS event group handle (EventGroupHandle_t).
    struct event_flags_handle_t
    {
        void* native;
    };

    // ---- work queue ----
    /// @brief Opaque work queue handle (emulated: thread + queue).
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (emulated: mutex + semaphore).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (emulated: block array + bitmap).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (emulated: mutex + condvar).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- wait-set ----
    /// @brief Opaque wait-set handle (FreeRTOS: not_supported; handle is unused).
    struct wait_set_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    // ---- static storage sizes (optional — used by static-creation helpers) ----
    static constexpr std::size_t mutex_storage_bytes       = 80U;
    static constexpr std::size_t semaphore_storage_bytes   = 80U;
    static constexpr std::size_t event_flags_storage_bytes = 36U;
};

// ---------------------------------------------------------------------------
// Zephyr specialisation
// ---------------------------------------------------------------------------

/// @brief Zephyr RTOS backend traits.
template<>
struct backend_traits<backend_zephyr>
{
    static constexpr const char* name = "Zephyr";

    struct thread_handle_t
    {
        void* native;
    };
    struct mutex_handle_t
    {
        void* native;
    };
    struct semaphore_handle_t
    {
        void* native;
    };
    struct queue_handle_t
    {
        void* native;
    };
    struct timer_handle_t
    {
        void* native;
    };
    struct event_flags_handle_t
    {
        void* native;
    };
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };  // k_work_q*

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (k_condvar*).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (k_mem_slab*).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (emulated: mutex + condvar).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 2048U;
    static constexpr std::size_t  mutex_storage_bytes       = 28U;
    static constexpr std::size_t  semaphore_storage_bytes   = 20U;
    static constexpr std::size_t  event_flags_storage_bytes = 32U;
};

// ---------------------------------------------------------------------------
// ThreadX specialisation
// ---------------------------------------------------------------------------

/// @brief ThreadX / Azure RTOS backend traits.
template<>
struct backend_traits<backend_threadx>
{
    static constexpr const char* name = "ThreadX";

    struct thread_handle_t
    {
        void* native;
    };
    struct mutex_handle_t
    {
        void* native;
    };
    struct semaphore_handle_t
    {
        void* native;
    };
    struct queue_handle_t
    {
        void* native;
    };
    struct timer_handle_t
    {
        void* native;
    };
    struct event_flags_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (emulated: mutex + semaphore).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (TX_BLOCK_POOL*).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (emulated: mutex + condvar).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 2048U;
    static constexpr std::size_t  mutex_storage_bytes       = 128U;
    static constexpr std::size_t  semaphore_storage_bytes   = 72U;
    static constexpr std::size_t  event_flags_storage_bytes = 72U;
};

// ---------------------------------------------------------------------------
// PX5 specialisation
// ---------------------------------------------------------------------------

/// @brief PX5 RTOS backend traits.
template<>
struct backend_traits<backend_px5>
{
    static constexpr const char* name = "PX5";

    struct thread_handle_t
    {
        void* native;
    };
    struct mutex_handle_t
    {
        void* native;
    };
    struct semaphore_handle_t
    {
        void* native;
    };
    struct queue_handle_t
    {
        void* native;
    };
    struct timer_handle_t
    {
        void* native;
    };
    struct event_flags_handle_t
    {
        void* native;
    };
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (emulated: mutex + semaphore).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (TX_BLOCK_POOL*).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (emulated: mutex + condvar).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 2048U;
    static constexpr std::size_t  mutex_storage_bytes       = 128U;
    static constexpr std::size_t  semaphore_storage_bytes   = 72U;
    static constexpr std::size_t  event_flags_storage_bytes = 72U;
};

// ---------------------------------------------------------------------------
// POSIX specialisation
// ---------------------------------------------------------------------------

/// @brief POSIX (pthreads) backend traits.
template<>
struct backend_traits<backend_posix>
{
    static constexpr const char* name = "POSIX";

    struct thread_handle_t
    {
        void* native;
    };  // pthread_t
    struct mutex_handle_t
    {
        void* native;
    };  // pthread_mutex_t*
    struct semaphore_handle_t
    {
        void* native;
    };  // sem_t*
    struct queue_handle_t
    {
        void* native;
    };  // custom ring buffer + condvar
    struct timer_handle_t
    {
        void* native;
    };  // timer_t
    struct event_flags_handle_t
    {
        void* native;
    };  // emulated with condvar
    struct wait_set_handle_t
    {
        void* native;
    };  // pollfd array
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (pthread_cond_t*).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (emulated: block array + bitmap).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (pthread_rwlock_t*).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 65536U;
    static constexpr std::size_t  mutex_storage_bytes       = 48U;   // sizeof(pthread_mutex_t)
    static constexpr std::size_t  semaphore_storage_bytes   = 32U;   // sizeof(sem_t)
    static constexpr std::size_t  event_flags_storage_bytes = 128U;  // mutex+condvar+bits
};

/// @brief RTEMS backend traits.
template<>
struct backend_traits<backend_rtems> : backend_traits<backend_posix>
{
    static constexpr const char* name = "RTEMS";
};

/// @brief INTEGRITY RTOS backend traits.
template<>
struct backend_traits<backend_integrity> : backend_traits<backend_posix>
{
    static constexpr const char* name = "INTEGRITY";
};

// ---------------------------------------------------------------------------
// Linux specialisation
// ---------------------------------------------------------------------------

/// @brief Linux native API backend traits.
template<>
struct backend_traits<backend_linux>
{
    static constexpr const char* name = "Linux";

    struct thread_handle_t
    {
        void* native;
    };  // pthread_t + cpu_set_t
    struct mutex_handle_t
    {
        void* native;
    };  // pthread_mutex_t*
    struct semaphore_handle_t
    {
        void* native;
    };  // sem_t*
    struct queue_handle_t
    {
        void* native;
    };  // pipe fd pair or custom
    struct timer_handle_t
    {
        void* native;
    };  // timerfd
    struct event_flags_handle_t
    {
        void* native;
    };  // eventfd based
    struct wait_set_handle_t
    {
        void* native;
    };  // epoll fd
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (pthread_cond_t*).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (emulated: block array + bitmap).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (pthread_rwlock_t*).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 65536U;
    static constexpr std::size_t  mutex_storage_bytes       = 48U;
    static constexpr std::size_t  semaphore_storage_bytes   = 32U;
    static constexpr std::size_t  event_flags_storage_bytes = 128U;
};

// ---------------------------------------------------------------------------
// Bare-metal specialisation
// ---------------------------------------------------------------------------

/// @brief Bare-metal (interrupt-driven, no RTOS) backend traits.
template<>
struct backend_traits<backend_baremetal>
{
    static constexpr const char* name = "BareMetal";

    // All handles use the uniform void* native pattern — the backend
    // implementation maintains separate static pools and stores a pointer
    // to the pool entry in native.

    struct thread_handle_t
    {
        void* native;
    };
    struct mutex_handle_t
    {
        void* native;
    };
    struct semaphore_handle_t
    {
        void* native;
    };
    struct queue_handle_t
    {
        void* native;
    };
    struct timer_handle_t
    {
        void* native;
    };
    struct event_flags_handle_t
    {
        void* native;
    };
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };
    struct condvar_handle_t
    {
        void* native;
    };
    struct memory_pool_handle_t
    {
        void* native;
    };
    struct stream_buffer_handle_t
    {
        void* native;
    };
    struct message_buffer_handle_t
    {
        void* native;
    };
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 512U;
    static constexpr std::size_t  mutex_storage_bytes       = sizeof(mutex_handle_t);
    static constexpr std::size_t  semaphore_storage_bytes   = sizeof(semaphore_handle_t);
    static constexpr std::size_t  event_flags_storage_bytes = sizeof(event_flags_handle_t);
};

// ---------------------------------------------------------------------------
// VxWorks specialisation
// ---------------------------------------------------------------------------

/// @brief VxWorks backend traits.
template<>
struct backend_traits<backend_vxworks>
{
    static constexpr const char* name = "VxWorks";

    struct thread_handle_t
    {
        void* native;
    };  // TASK_ID
    struct mutex_handle_t
    {
        void* native;
    };  // SEM_ID (mutual-exclusion semaphore)
    struct semaphore_handle_t
    {
        void* native;
    };  // SEM_ID (counting semaphore)
    struct queue_handle_t
    {
        void* native;
    };  // MSG_Q_ID
    struct timer_handle_t
    {
        void* native;
    };  // WDOG_ID
    struct event_flags_handle_t
    {
        void* native;
    };
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };  // jobQueueLib

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (condVarLib).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (PART_ID (memPartLib)).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (emulated: mutex + condvar).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 8192U;
    static constexpr std::size_t  mutex_storage_bytes       = 64U;
    static constexpr std::size_t  semaphore_storage_bytes   = 64U;
    static constexpr std::size_t  event_flags_storage_bytes = 64U;
};

// ---------------------------------------------------------------------------
// NuttX specialisation
// ---------------------------------------------------------------------------

/// @brief NuttX RTOS backend traits.
template<>
struct backend_traits<backend_nuttx>
{
    static constexpr const char* name = "NuttX";

    struct thread_handle_t
    {
        void* native;
    };  // pthread_t*
    struct mutex_handle_t
    {
        void* native;
    };  // nxmutex_t*
    struct semaphore_handle_t
    {
        void* native;
    };  // sem_t*
    struct queue_handle_t
    {
        void* native;
    };  // mqd_t wrapper
    struct timer_handle_t
    {
        void* native;
    };  // work_s* (work queue)
    struct event_flags_handle_t
    {
        void* native;
    };  // condvar-based
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };  // work_queue()

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (pthread_cond_t*).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (emulated: block array + bitmap).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (pthread_rwlock_t*).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 4096U;
    static constexpr std::size_t  mutex_storage_bytes       = 32U;
    static constexpr std::size_t  semaphore_storage_bytes   = 32U;
    static constexpr std::size_t  event_flags_storage_bytes = 128U;
};

// ---------------------------------------------------------------------------
// Micrium µC/OS-III specialisation
// ---------------------------------------------------------------------------

/// @brief Micrium µC/OS-III backend traits.
template<>
struct backend_traits<backend_micrium>
{
    static constexpr const char* name = "Micrium";

    struct thread_handle_t
    {
        void* native;
    };  // OS_TCB*
    struct mutex_handle_t
    {
        void* native;
    };  // OS_MUTEX*
    struct semaphore_handle_t
    {
        void* native;
    };  // OS_SEM*
    struct queue_handle_t
    {
        void* native;
    };  // OS_Q*
    struct timer_handle_t
    {
        void* native;
    };  // OS_TMR*
    struct event_flags_handle_t
    {
        void* native;
    };  // OS_FLAG_GRP*
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (emulated: mutex + semaphore).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (OS_MEM*).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (emulated: mutex + condvar).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 2048U;
    static constexpr std::size_t  mutex_storage_bytes       = 128U;
    static constexpr std::size_t  semaphore_storage_bytes   = 72U;
    static constexpr std::size_t  event_flags_storage_bytes = 72U;
};

// ---------------------------------------------------------------------------
// ChibiOS/RT specialisation
// ---------------------------------------------------------------------------

/// @brief ChibiOS/RT backend traits.
template<>
struct backend_traits<backend_chibios>
{
    static constexpr const char* name = "ChibiOS";

    struct thread_handle_t
    {
        void* native;
    };  // ch_thread_slot*
    struct mutex_handle_t
    {
        void* native;
    };  // mutex_t*
    struct semaphore_handle_t
    {
        void* native;
    };  // semaphore_t*
    struct queue_handle_t
    {
        void* native;
    };  // ch_mailbox_slot*
    struct timer_handle_t
    {
        void* native;
    };  // ch_vt_slot*
    struct event_flags_handle_t
    {
        void* native;
    };  // ch_event_slot*
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (condition_variable_t*).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (memory_pool_t*).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (emulated: mutex + condvar).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 2048U;
    static constexpr std::size_t  mutex_storage_bytes       = 32U;
    static constexpr std::size_t  semaphore_storage_bytes   = 24U;
    static constexpr std::size_t  event_flags_storage_bytes = 32U;
};

// ---------------------------------------------------------------------------
// SEGGER embOS specialisation
// ---------------------------------------------------------------------------

/// @brief SEGGER embOS backend traits.
template<>
struct backend_traits<backend_embos>
{
    static constexpr const char* name = "embOS";

    struct thread_handle_t
    {
        void* native;
    };  // OS_TASK*
    struct mutex_handle_t
    {
        void* native;
    };  // OS_MUTEX*
    struct semaphore_handle_t
    {
        void* native;
    };  // OS_SEMAPHORE*
    struct queue_handle_t
    {
        void* native;
    };  // eos_queue_slot*
    struct timer_handle_t
    {
        void* native;
    };  // eos_timer_slot*
    struct event_flags_handle_t
    {
        void* native;
    };  // OS_EVENT*
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (emulated: mutex + semaphore).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (OS_MEMPOOL*).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (emulated: mutex + condvar).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 2048U;
    static constexpr std::size_t  mutex_storage_bytes       = 64U;
    static constexpr std::size_t  semaphore_storage_bytes   = 32U;
    static constexpr std::size_t  event_flags_storage_bytes = 32U;
};

// ---------------------------------------------------------------------------
// QNX Neutrino specialisation
// ---------------------------------------------------------------------------

/// @brief QNX Neutrino backend traits.
template<>
struct backend_traits<backend_qnx>
{
    static constexpr const char* name = "QNX";

    struct thread_handle_t
    {
        void* native;
    };  // pthread_t*
    struct mutex_handle_t
    {
        void* native;
    };  // pthread_mutex_t*
    struct semaphore_handle_t
    {
        void* native;
    };  // sem_t*
    struct queue_handle_t
    {
        void* native;
    };  // mqd_t wrapper
    struct timer_handle_t
    {
        void* native;
    };  // qnx_timer_ctx*
    struct event_flags_handle_t
    {
        void* native;
    };  // qnx_event_flags*
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (pthread_cond_t*).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (emulated: block array + bitmap).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (pthread_rwlock_t*).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 65536U;
    static constexpr std::size_t  mutex_storage_bytes       = 48U;
    static constexpr std::size_t  semaphore_storage_bytes   = 32U;
    static constexpr std::size_t  event_flags_storage_bytes = 128U;
};

// ---------------------------------------------------------------------------
// CMSIS-RTOS v1 specialisation
// ---------------------------------------------------------------------------

/// @brief CMSIS-RTOS v1 backend traits.
template<>
struct backend_traits<backend_cmsis_rtos>
{
    static constexpr const char* name = "CMSIS-RTOS";

    struct thread_handle_t
    {
        void* native;
    };  // osThreadId
    struct mutex_handle_t
    {
        void* native;
    };  // osMutexId
    struct semaphore_handle_t
    {
        void* native;
    };  // osSemaphoreId
    struct queue_handle_t
    {
        void* native;
    };  // osMessageQId
    struct timer_handle_t
    {
        void* native;
    };  // osTimerId
    struct event_flags_handle_t
    {
        void* native;
    };  // emulated (condvar-based)
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (emulated: mutex + semaphore).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (osPoolId).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (emulated: mutex + condvar).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 2048U;
    static constexpr std::size_t  mutex_storage_bytes       = 64U;
    static constexpr std::size_t  semaphore_storage_bytes   = 32U;
    static constexpr std::size_t  event_flags_storage_bytes = 64U;
};

// ---------------------------------------------------------------------------
// CMSIS-RTOS2 (v2) specialisation
// ---------------------------------------------------------------------------

/// @brief CMSIS-RTOS2 (v2) backend traits.
template<>
struct backend_traits<backend_cmsis_rtos2>
{
    static constexpr const char* name = "CMSIS-RTOS2";

    struct thread_handle_t
    {
        void* native;
    };  // osThreadId_t
    struct mutex_handle_t
    {
        void* native;
    };  // osMutexId_t
    struct semaphore_handle_t
    {
        void* native;
    };  // osSemaphoreId_t
    struct queue_handle_t
    {
        void* native;
    };  // osMessageQueueId_t
    struct timer_handle_t
    {
        void* native;
    };  // osTimerId_t
    struct event_flags_handle_t
    {
        void* native;
    };  // osEventFlagsId_t
    struct wait_set_handle_t
    {
        void* native;
    };
    struct work_queue_handle_t
    {
        void* native;
    };

    // ---- condition variable ----
    /// @brief Opaque condition variable handle (emulated: mutex + semaphore).
    struct condvar_handle_t
    {
        void* native;
    };

    // ---- memory pool ----
    /// @brief Opaque memory pool handle (osMemoryPoolId_t).
    struct memory_pool_handle_t
    {
        void* native;
    };
    // ---- stream buffer ----
    /// @brief Opaque stream buffer handle.
    struct stream_buffer_handle_t
    {
        void* native;
    };

    // ---- message buffer ----
    /// @brief Opaque message buffer handle.
    struct message_buffer_handle_t
    {
        void* native;
    };

    // ---- read-write lock ----
    /// @brief Opaque read-write lock handle (emulated: mutex + condvar).
    struct rwlock_handle_t
    {
        void* native;
    };

    // ---- spinlock ----
    /// @brief Opaque spinlock handle.
    struct spinlock_handle_t
    {
        void* native;
    };

    // ---- barrier ----
    /// @brief Opaque barrier handle.
    struct barrier_handle_t
    {
        void* native;
    };

    static constexpr stack_size_t default_stack_bytes       = 2048U;
    static constexpr std::size_t  mutex_storage_bytes       = 64U;
    static constexpr std::size_t  semaphore_storage_bytes   = 32U;
    static constexpr std::size_t  event_flags_storage_bytes = 32U;
};

/// @} // osal_traits

}  // namespace osal
