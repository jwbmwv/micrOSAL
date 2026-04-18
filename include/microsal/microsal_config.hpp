// SPDX-License-Identifier: Apache-2.0
/// @file microsal_config.hpp
/// @brief micrOSAL bus layer — compile-time configuration
/// @details Defines the bus backend tag types for the bus + pub/sub
///          abstraction layer and selects the default backend tag based on the
///          active micrOSAL OSAL backend macro.
///
///          To override the default, define @c MICROSAL_DEFAULT_BACKEND_TAG
///          before including any bus header:
///          @code
///          #define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_generic
///          #include <microsal/bus/osal_signal.hpp>
///          @endcode
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <osal/backends.hpp>

namespace osal
{

/// @defgroup osal_bus_backends Channel Backend Tags
/// @brief Compile-time tag types that select the bus/signal implementation.
/// @{

/// @brief Generic micrOSAL fallback backend.
/// @details Implemented using osal::queue (per-subscriber) and osal::mutex
///          for subscriber-slot management. Works on every micrOSAL backend.
struct bus_backend_generic
{
};

/// @brief Zephyr Zbus-backed bus backend.
/// @details Maps each osal_signal<T> to a Zbus bus. Provides native
///          pub/sub, observers, routing, and zero-copy publish.
struct bus_backend_zephyr
{
};

/// @brief FreeRTOS-specific bus backend.
/// @details Uses FreeRTOS queues and task notifications directly.
///          Currently falls back to bus_backend_generic behaviour.
struct bus_backend_freertos
{
};

/// @brief POSIX-thread bus backend.
/// @details Uses POSIX pthread primitives directly.
///          Currently falls back to bus_backend_generic behaviour.
struct bus_backend_posix
{
};

/// @brief Bare-metal bus backend (no RTOS, ISR-safe ring buffers).
struct bus_backend_bare_metal
{
};

/// @brief ThreadX / Azure RTOS bus backend.
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_threadx
{
};

/// @brief PX5 RTOS bus backend (ThreadX-compatible).
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_px5
{
};

/// @brief Linux-specific bus backend.
/// @details Delegates to bus_backend_generic via the common C ABI.
///          Inherits POSIX pthread primitives plus Linux extensions.
struct bus_backend_linux
{
};

/// @brief VxWorks bus backend.
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_vxworks
{
};

/// @brief NuttX bus backend (POSIX-compliant RTOS).
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_nuttx
{
};

/// @brief Micrium µC/OS-III bus backend.
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_micrium
{
};

/// @brief ChibiOS/RT bus backend.
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_chibios
{
};

/// @brief SEGGER embOS bus backend.
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_embos
{
};

/// @brief QNX Neutrino bus backend.
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_qnx
{
};

/// @brief RTEMS bus backend (POSIX-compliant RTOS).
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_rtems
{
};

/// @brief INTEGRITY RTOS bus backend.
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_integrity
{
};

/// @brief CMSIS-RTOS v1 bus backend.
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_cmsis_rtos
{
};

/// @brief CMSIS-RTOS2 v2 bus backend.
/// @details Delegates to bus_backend_generic via the common C ABI.
struct bus_backend_cmsis_rtos2
{
};

/// @brief Mock premium backend for unit tests.
/// @details Uses the generic LCD implementation internally but advertises
///          all premium capabilities as @c true so that osal_signal_premium
///          premium paths are exercised in test builds.
struct bus_backend_mock
{
};

/// @} // osal_bus_backends

}  // namespace osal

// ---------------------------------------------------------------------------
// MICROSAL_DEFAULT_BACKEND_TAG — auto-select unless user overrides
// ---------------------------------------------------------------------------

#if !defined(MICROSAL_DEFAULT_BACKEND_TAG)
#if defined(OSAL_BACKEND_ZEPHYR)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_zephyr
#elif defined(OSAL_BACKEND_FREERTOS)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_freertos
#elif defined(OSAL_BACKEND_THREADX)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_threadx
#elif defined(OSAL_BACKEND_PX5)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_px5
#elif defined(OSAL_BACKEND_POSIX)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_posix
#elif defined(OSAL_BACKEND_LINUX)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_linux
#elif defined(OSAL_BACKEND_BAREMETAL)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_bare_metal
#elif defined(OSAL_BACKEND_VXWORKS)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_vxworks
#elif defined(OSAL_BACKEND_NUTTX)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_nuttx
#elif defined(OSAL_BACKEND_MICRIUM)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_micrium
#elif defined(OSAL_BACKEND_CHIBIOS)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_chibios
#elif defined(OSAL_BACKEND_EMBOS)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_embos
#elif defined(OSAL_BACKEND_QNX)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_qnx
#elif defined(OSAL_BACKEND_RTEMS)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_rtems
#elif defined(OSAL_BACKEND_INTEGRITY)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_integrity
#elif defined(OSAL_BACKEND_CMSIS_RTOS)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_cmsis_rtos
#elif defined(OSAL_BACKEND_CMSIS_RTOS2)
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_cmsis_rtos2
#else
/// @brief Default bus backend tag for the current build configuration.
/// @details Defined automatically from the active OSAL_BACKEND_* macro.
///          Override by defining this macro before including bus headers.
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_generic
#endif
#endif
