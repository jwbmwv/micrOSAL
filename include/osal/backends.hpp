// SPDX-License-Identifier: Apache-2.0
/// @file backends.hpp
/// @brief Active-backend selector
/// @details Sets osal::active_backend to the backend tag type chosen by the
///          user-defined OSAL_BACKEND_* macro. Exactly one macro must be defined;
///          a missing or conflicting definition is a hard compile error.
///
///          C++20 @c consteval validation keeps backend selection fully
///          compile-time and confirms that the selected backend satisfies the
///          OSAL traits/capabilities concepts before any wrapper code is used.
///
///          Supported macros (define exactly one):
///          - @c OSAL_BACKEND_FREERTOS
///          - @c OSAL_BACKEND_ZEPHYR
///          - @c OSAL_BACKEND_THREADX
///          - @c OSAL_BACKEND_PX5
///          - @c OSAL_BACKEND_POSIX
///          - @c OSAL_BACKEND_LINUX
///          - @c OSAL_BACKEND_BAREMETAL
///          - @c OSAL_BACKEND_VXWORKS
///          - @c OSAL_BACKEND_NUTTX
///          - @c OSAL_BACKEND_MICRIUM
///          - @c OSAL_BACKEND_CHIBIOS
///          - @c OSAL_BACKEND_EMBOS
///          - @c OSAL_BACKEND_QNX
///          - @c OSAL_BACKEND_RTEMS
///          - @c OSAL_BACKEND_INTEGRITY
///          - @c OSAL_BACKEND_CMSIS_RTOS
///          - @c OSAL_BACKEND_CMSIS_RTOS2
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_core
#pragma once

#include "capabilities.hpp"
#include "backend_traits.hpp"
#include "concepts.hpp"

#include <array>
#include <span>

namespace osal
{

namespace detail
{

// NOLINTBEGIN(cert-dcl58-cpp) — false positive: standalone parsing confuses std::size_t usage with std modification
template<std::size_t N>
consteval std::size_t count_selected_backends(std::span<const bool, N> flags) noexcept
// NOLINTEND(cert-dcl58-cpp)
{
    std::size_t count = 0U;
    for (const bool flag : flags)
    {
        count += flag ? 1U : 0U;
    }
    return count;
}

inline constexpr std::array<bool, 17U> backend_selection_flags{
#if defined(OSAL_BACKEND_FREERTOS)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_ZEPHYR)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_THREADX)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_PX5)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_POSIX)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_LINUX)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_BAREMETAL)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_VXWORKS)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_NUTTX)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_MICRIUM)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_CHIBIOS)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_EMBOS)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_QNX)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_RTEMS)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_INTEGRITY)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_CMSIS_RTOS)
    true,
#else
    false,
#endif
#if defined(OSAL_BACKEND_CMSIS_RTOS2)
    true,
#else
    false,
#endif
};

}  // namespace detail

/// @defgroup osal_backend OSAL Backend Selection
/// @brief Compile-time active-backend configuration.
/// @{

// ---------------------------------------------------------------------------
// Count how many OSAL_BACKEND_* macros are defined (must be exactly 1)
// ---------------------------------------------------------------------------

#if defined(OSAL_BACKEND_FREERTOS) || defined(OSAL_BACKEND_ZEPHYR) || defined(OSAL_BACKEND_THREADX) || \
    defined(OSAL_BACKEND_PX5) || defined(OSAL_BACKEND_POSIX) || defined(OSAL_BACKEND_LINUX) ||         \
    defined(OSAL_BACKEND_BAREMETAL) || defined(OSAL_BACKEND_VXWORKS) || defined(OSAL_BACKEND_NUTTX) || \
    defined(OSAL_BACKEND_MICRIUM) || defined(OSAL_BACKEND_CHIBIOS) || defined(OSAL_BACKEND_EMBOS) ||   \
    defined(OSAL_BACKEND_QNX) || defined(OSAL_BACKEND_RTEMS) || defined(OSAL_BACKEND_INTEGRITY) ||     \
    defined(OSAL_BACKEND_CMSIS_RTOS) || defined(OSAL_BACKEND_CMSIS_RTOS2)
// at least one is defined — good
#else
#error "OSAL: no backend selected.  Define exactly one of: "    \
         "OSAL_BACKEND_FREERTOS, OSAL_BACKEND_ZEPHYR, "           \
         "OSAL_BACKEND_THREADX, OSAL_BACKEND_PX5, "               \
         "OSAL_BACKEND_POSIX, OSAL_BACKEND_LINUX, "               \
         "OSAL_BACKEND_BAREMETAL, OSAL_BACKEND_VXWORKS, "         \
         "OSAL_BACKEND_NUTTX, OSAL_BACKEND_MICRIUM, "             \
         "OSAL_BACKEND_CHIBIOS, OSAL_BACKEND_EMBOS, "             \
         "OSAL_BACKEND_QNX, OSAL_BACKEND_RTEMS, "                 \
         "OSAL_BACKEND_INTEGRITY, OSAL_BACKEND_CMSIS_RTOS, "      \
         "OSAL_BACKEND_CMSIS_RTOS2"
#endif

// Detect multiple definitions with the consteval validation below.

// ---------------------------------------------------------------------------
// POSIX-pthread backend group macro
// ---------------------------------------------------------------------------
/// @brief Defined whenever the active backend's thread primitive is POSIX pthreads.
/// Covers POSIX, Linux, NuttX, QNX, RTEMS and INTEGRITY — all of which expose the full
/// <pthread.h> + <semaphore.h> surface.  Use this instead of listing
/// individual backend macros when writing pthread-conditional code.
#if defined(OSAL_BACKEND_POSIX) || defined(OSAL_BACKEND_LINUX) || defined(OSAL_BACKEND_NUTTX) || \
    defined(OSAL_BACKEND_QNX) || defined(OSAL_BACKEND_RTEMS) || defined(OSAL_BACKEND_INTEGRITY)
#define OSAL_BACKEND_HAS_PTHREAD 1
#endif

// ---------------------------------------------------------------------------
// Define active_backend type alias
// ---------------------------------------------------------------------------

#if defined(OSAL_BACKEND_FREERTOS)
using active_backend = backend_freertos;
#elif defined(OSAL_BACKEND_ZEPHYR)
using active_backend = backend_zephyr;
#elif defined(OSAL_BACKEND_THREADX)
using active_backend = backend_threadx;
#elif defined(OSAL_BACKEND_PX5)
using active_backend = backend_px5;
#elif defined(OSAL_BACKEND_POSIX)
using active_backend = backend_posix;
#elif defined(OSAL_BACKEND_LINUX)
using active_backend = backend_linux;
#elif defined(OSAL_BACKEND_BAREMETAL)
using active_backend = backend_baremetal;
#elif defined(OSAL_BACKEND_VXWORKS)
using active_backend = backend_vxworks;
#elif defined(OSAL_BACKEND_NUTTX)
using active_backend = backend_nuttx;
#elif defined(OSAL_BACKEND_MICRIUM)
using active_backend = backend_micrium;
#elif defined(OSAL_BACKEND_CHIBIOS)
using active_backend = backend_chibios;
#elif defined(OSAL_BACKEND_EMBOS)
using active_backend = backend_embos;
#elif defined(OSAL_BACKEND_QNX)
using active_backend = backend_qnx;
#elif defined(OSAL_BACKEND_RTEMS)
using active_backend = backend_rtems;
#elif defined(OSAL_BACKEND_INTEGRITY)
using active_backend = backend_integrity;
#elif defined(OSAL_BACKEND_CMSIS_RTOS)
using active_backend = backend_cmsis_rtos;
#elif defined(OSAL_BACKEND_CMSIS_RTOS2)
using active_backend = backend_cmsis_rtos2;
#endif

static_assert(detail::count_selected_backends(std::span{detail::backend_selection_flags}) == 1U,
              "OSAL: define exactly one OSAL_BACKEND_* macro.");
static_assert(backend_capabilities_spec<active_backend>,
              "OSAL: selected backend capabilities<> specialisation is incomplete.");
static_assert(backend_traits_spec<active_backend>,
              "OSAL: selected backend backend_traits<> specialisation is incomplete.");

// ---------------------------------------------------------------------------
// Convenience aliases using the active backend
// ---------------------------------------------------------------------------

/// @brief Capability descriptor for the currently selected backend.
using active_capabilities = capabilities<active_backend>;

/// @brief Trait descriptor for the currently selected backend.
using active_traits = backend_traits<active_backend>;

/// @brief Human-readable name of the currently selected backend.
constexpr const char* backend_name() noexcept
{
    return active_traits::name;
}

/// @brief Compile-time requirement selector for optional or native-only APIs.
enum class support_requirement
{
    timer,
    delayable_work,
    wait_set,
    spinlock,
    timed_join,
    thread_affinity,
    dynamic_thread_priority,
    task_notification,
    thread_suspend_resume,
    thread_stack_watermark,
    thread_execution_time,
    thread_cpu_load_stats,
    high_resolution_clock,
    current_cpu_query,
    irq_mask_guard,
};

namespace detail
{

template<support_requirement Requirement, typename Backend>
struct support_requirement_value;

template<typename Backend>
struct support_requirement_value<support_requirement::timer, Backend>
{
    static constexpr bool value = timer_backend<Backend>;
};

template<typename Backend>
struct support_requirement_value<support_requirement::delayable_work, Backend>
{
    static constexpr bool value = delayable_work_backend<Backend>;
};

template<typename Backend>
struct support_requirement_value<support_requirement::wait_set, Backend>
{
    static constexpr bool value = wait_set_backend<Backend>;
};

template<typename Backend>
struct support_requirement_value<support_requirement::spinlock, Backend>
{
    static constexpr bool value = capabilities<Backend>::has_spinlock;
};

template<typename Backend>
struct support_requirement_value<support_requirement::timed_join, Backend>
{
    static constexpr bool value = timed_join_backend<Backend>;
};

template<typename Backend>
struct support_requirement_value<support_requirement::thread_affinity, Backend>
{
    static constexpr bool value = thread_affinity_backend<Backend>;
};

template<typename Backend>
struct support_requirement_value<support_requirement::dynamic_thread_priority, Backend>
{
    static constexpr bool value = dynamic_thread_priority_backend<Backend>;
};

template<typename Backend>
struct support_requirement_value<support_requirement::task_notification, Backend>
{
    static constexpr bool value = task_notification_backend<Backend>;
};

template<typename Backend>
struct support_requirement_value<support_requirement::thread_suspend_resume, Backend>
{
    static constexpr bool value = capabilities<Backend>::has_thread_suspend_resume;
};

template<typename Backend>
struct support_requirement_value<support_requirement::thread_stack_watermark, Backend>
{
    static constexpr bool value = thread_stack_watermark_capability<Backend>::value;
};

template<typename Backend>
struct support_requirement_value<support_requirement::thread_execution_time, Backend>
{
    static constexpr bool value = thread_execution_time_capability<Backend>::value;
};

template<typename Backend>
struct support_requirement_value<support_requirement::thread_cpu_load_stats, Backend>
{
    static constexpr bool value = thread_cpu_load_stats_capability<Backend>::value;
};

template<typename Backend>
struct support_requirement_value<support_requirement::high_resolution_clock, Backend>
{
    static constexpr bool value = high_resolution_clock_capability<Backend>::value;
};

template<typename Backend>
struct support_requirement_value<support_requirement::current_cpu_query, Backend>
{
    static constexpr bool value = current_cpu_query_capability<Backend>::value;
};

template<typename Backend>
struct support_requirement_value<support_requirement::irq_mask_guard, Backend>
{
    static constexpr bool value = irq_mask_guard_capability<Backend>::value;
};

}  // namespace detail

/// @brief True when the selected backend satisfies the requested requirement.
template<support_requirement Requirement, typename Backend = active_backend>
// NOLINTNEXTLINE(google-readability-casting) -- false positive on non-type template parameter usage.
inline constexpr bool supports_requirement = detail::support_requirement_value<Requirement, Backend>::value;

/// @brief Force a clear compile-time diagnostic when a backend requirement is mandatory.
template<support_requirement Requirement, typename Backend = active_backend>
consteval void require_backend_support() noexcept
{
    if constexpr (Requirement == support_requirement::timer)
    {
        static_assert(timer_backend<Backend>,
                      "osal::timer requires backend timer support. This feature has no universal portable fallback.");
    }
    else if constexpr (Requirement == support_requirement::delayable_work)
    {
        static_assert(delayable_work_backend<Backend>,
                      "osal::delayable_work requires timers plus thread-context work-queue dispatch. If timer "
                      "callbacks may run in ISR, the backend must support work_queue submit_from_isr().");
    }
    else if constexpr (Requirement == support_requirement::wait_set)
    {
        static_assert(wait_set_backend<Backend>, "osal::wait_set requires a native descriptor/object wait primitive. "
                                                 "Use osal::object_wait_set for portable OSAL-object multiplexing.");
    }
    else if constexpr (Requirement == support_requirement::spinlock)
    {
        static_assert(capabilities<Backend>::has_spinlock,
                      "osal::spinlock requires native spinlock support. There is intentionally no portable emulation; "
                      "use osal::mutex for longer critical sections.");
    }
    else if constexpr (Requirement == support_requirement::timed_join)
    {
        static_assert(timed_join_backend<Backend>,
                      "osal::thread::join_for requires timed-join support from the active backend.");
    }
    else if constexpr (Requirement == support_requirement::thread_affinity)
    {
        static_assert(thread_affinity_backend<Backend>,
                      "osal::thread::set_affinity requires backend thread-affinity support.");
    }
    else if constexpr (Requirement == support_requirement::dynamic_thread_priority)
    {
        static_assert(dynamic_thread_priority_backend<Backend>,
                      "osal::thread::set_priority requires backend dynamic-priority support.");
    }
    else if constexpr (Requirement == support_requirement::task_notification)
    {
        static_assert(task_notification_backend<Backend>,
                      "osal::thread task notifications require backend-native task-notification support. Use "
                      "osal::notification<Slots> for a portable alternative.");
    }
    else if constexpr (Requirement == support_requirement::thread_stack_watermark)
    {
        static_assert(thread_stack_watermark_capability<Backend>::value,
                      "osal::thread stack watermark queries require backend support for per-thread stack usage "
                      "reporting.");
    }
    else if constexpr (Requirement == support_requirement::thread_execution_time)
    {
        static_assert(thread_execution_time_capability<Backend>::value,
                      "osal::thread execution-time queries require backend support for per-thread runtime "
                      "accounting.");
    }
    else if constexpr (Requirement == support_requirement::thread_cpu_load_stats)
    {
        static_assert(thread_cpu_load_stats_capability<Backend>::value,
                      "osal::thread CPU-load queries require backend support for per-thread load statistics.");
    }
    else if constexpr (Requirement == support_requirement::high_resolution_clock)
    {
        static_assert(high_resolution_clock_capability<Backend>::value,
                      "osal::high_resolution_clock requires a backend-provided high-resolution timing source.");
    }
    else if constexpr (Requirement == support_requirement::current_cpu_query)
    {
        static_assert(current_cpu_query_capability<Backend>::value,
                      "osal::thread current-CPU queries require backend support for reporting the running core.");
    }
    else if constexpr (Requirement == support_requirement::irq_mask_guard)
    {
        static_assert(irq_mask_guard_capability<Backend>::value,
                      "osal::irq_mask_guard requires backend support for short-duration interrupt masking.");
    }
    else
    {
        static_assert(capabilities<Backend>::has_thread_suspend_resume,
                      "osal::thread::suspend/resume requires backend suspend/resume support.");
    }
}

/// @} // osal_backend

}  // namespace osal
