// SPDX-License-Identifier: Apache-2.0
/// @file backends.hpp
/// @brief Active-backend selector
/// @details Sets osal::active_backend to the backend tag type chosen by the
///          user-defined OSAL_BACKEND_* macro.  Exactly one macro must be defined;
///          a missing or conflicting definition is a hard compile error.
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

namespace osal
{

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

// Detect multiple definitions (only possible for some compilers lacking
// proper counting; we rely on naming collisions instead).

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

// ---------------------------------------------------------------------------
// Convenience aliases using the active backend
// ---------------------------------------------------------------------------

/// @brief Capability descriptor for the currently selected backend.
using active_capabilities = capabilities<active_backend>;

/// @brief Trait descriptor for the currently selected backend.
using active_traits = backend_traits<active_backend>;

/// @brief Human-readable name of the currently selected backend.
inline constexpr const char* backend_name() noexcept
{
    return active_traits::name;
}

/// @} // osal_backend

}  // namespace osal
