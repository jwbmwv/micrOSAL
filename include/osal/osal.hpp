// SPDX-License-Identifier: Apache-2.0
/// @file osal.hpp
/// @brief OSAL convenience header — includes all components
/// @details Including this single header pulls in all OSAL primitives.
///          Prefer individual headers (mutex.hpp, thread.hpp, …) in
///          header-only / embedded contexts to minimise compile-time.
///
/// @par Selecting a backend
/// Define exactly one of the following before including this header
/// (or pass via CMake `target_compile_definitions`):
/// @code
///   OSAL_BACKEND_FREERTOS
///   OSAL_BACKEND_ZEPHYR
///   OSAL_BACKEND_THREADX
///   OSAL_BACKEND_PX5
///   OSAL_BACKEND_POSIX
///   OSAL_BACKEND_LINUX
///   OSAL_BACKEND_BAREMETAL
///   OSAL_BACKEND_VXWORKS
///   OSAL_BACKEND_NUTTX
///   OSAL_BACKEND_MICRIUM
///   OSAL_BACKEND_CHIBIOS
///   OSAL_BACKEND_EMBOS
///   OSAL_BACKEND_QNX
///   OSAL_BACKEND_RTEMS
///   OSAL_BACKEND_INTEGRITY
///   OSAL_BACKEND_CMSIS_RTOS
///   OSAL_BACKEND_CMSIS_RTOS2
/// @endcode
///
/// @par Example
/// @code
/// #define OSAL_BACKEND_POSIX
/// #include <osal/osal.hpp>
///
/// int main() {
///     osal::mutex m;
///     osal::semaphore s{osal::semaphore_type::binary, 0U};
///     osal::queue<std::uint32_t, 8> q;
///
///     {
///         osal::mutex::lock_guard lg{m};
///         // critical section
///     }
///
///     s.give();
///     s.take_for(std::chrono::milliseconds{100});
/// }
/// @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_core
#pragma once

#if !defined(__cplusplus) || (__cplusplus < 202002L)
#error "micrOSAL requires at least C++20. Compile with -std=c++20 (or newer)."
#endif

// Version.
#include "version.hpp"

// Core types and error codes (no backend dependency).
#include "error.hpp"
#include "types.hpp"
#include "concepts.hpp"
#include "capabilities.hpp"
#include "backend_traits.hpp"
#include "backends.hpp"

// Primitives (depend on backends.hpp).
#include "clock.hpp"
#include "mutex.hpp"
#include "semaphore.hpp"
#include "thread.hpp"
#include "queue.hpp"
#include "mailbox.hpp"
#include "timer.hpp"
#include "event_flags.hpp"
#include "wait_set.hpp"
#include "condvar.hpp"
#include "work_queue.hpp"
#include "memory_pool.hpp"
#include "rwlock.hpp"
#include "spinlock.hpp"
#include "barrier.hpp"
#include "ring_buffer.hpp"
#include "stream_buffer.hpp"
#include "message_buffer.hpp"
#include "thread_local_data.hpp"
#include "notification.hpp"
#include "delayable_work.hpp"
#include "object_wait_set.hpp"

/// @defgroup osal OSAL
/// @brief Operating System Abstraction Layer for portable embedded C++20 code.
/// @details
/// The OSAL provides a uniform API over multiple RTOS and OS backends with
/// no virtual functions, no RTTI, and noexcept operations throughout.
/// C++20 concepts and @c consteval checks keep backend selection, fixed-storage
/// constraints, and capability validation fully compile-time.
///
/// Feature availability at compile-time:
/// @code
///   if constexpr (osal::active_capabilities::has_recursive_mutex) { ... }
/// @endcode
///
/// @{
/// @}
