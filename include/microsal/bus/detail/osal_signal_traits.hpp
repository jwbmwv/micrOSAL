// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_traits.hpp
/// @brief Compile-time capability traits for channel backend tags
/// @details Provides osal::osal_signal_capabilities<BackendTag> with per-backend
///          feature flags and C++20 concepts for conditional API enablement.
///
///          Usage:
///          @code
///          if constexpr (osal::osal_signal_capabilities<MyBackend>::native_observers) {
///              // use observer-based subscription
///          }
///          @endcode
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <microsal/microsal_config.hpp>
#include <concepts>
#include <type_traits>

namespace osal
{

/// @defgroup osal_signal_traits Channel Topic Traits
/// @brief Compile-time feature flags per channel backend.
/// @{

// ---------------------------------------------------------------------------
// Primary template — conservative defaults (no premium features)
// ---------------------------------------------------------------------------

/// @brief Capability descriptor for a channel/topic backend.
/// @tparam BackendTag  One of the osal::bus_backend_* tag types.
/// @details All flags default to @c false. Specialisations enable flags that
///          the backend natively supports or emulates efficiently.
template<typename BackendTag>
struct osal_signal_capabilities
{
    /// @brief Backend provides native pub/sub (not queue-per-subscriber emulation).
    static constexpr bool native_pubsub = false;
    /// @brief Backend supports callback observer registration (push model).
    static constexpr bool native_observers = false;
    /// @brief Backend supports topic-to-topic message routing.
    static constexpr bool native_routing = false;
    /// @brief Backend supports zero-copy or near-zero-copy publish.
    static constexpr bool zero_copy = false;
};

// ---------------------------------------------------------------------------
// bus_backend_generic — LCD emulation via osal::queue + osal::mutex
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_generic>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_zephyr — full Zbus-backed premium capabilities
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_zephyr>
{
    static constexpr bool native_pubsub    = true;
    static constexpr bool native_observers = true;
    static constexpr bool native_routing   = true;
    static constexpr bool zero_copy        = true;
};

// ---------------------------------------------------------------------------
// bus_backend_freertos — native queues, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_freertos>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_posix — pthreads, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_posix>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_bare_metal — ISR-safe ring buffers, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_bare_metal>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_threadx — ThreadX / Azure RTOS queues, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_threadx>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_px5 — PX5 RTOS (ThreadX-compatible), no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_px5>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_linux — Linux (POSIX + extensions), no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_linux>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_vxworks — VxWorks queues, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_vxworks>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_nuttx — NuttX (POSIX-compliant), no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_nuttx>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_micrium — Micrium µC/OS-III, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_micrium>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_chibios — ChibiOS/RT, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_chibios>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_embos — SEGGER embOS, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_embos>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_qnx — QNX Neutrino, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_qnx>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_rtems — RTEMS (POSIX-compliant), no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_rtems>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_integrity — INTEGRITY RTOS, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_integrity>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_cmsis_rtos — CMSIS-RTOS v1, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_cmsis_rtos>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_cmsis_rtos2 — CMSIS-RTOS2 v2, no premium
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_cmsis_rtos2>
{
    static constexpr bool native_pubsub    = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing   = false;
    static constexpr bool zero_copy        = false;
};

// ---------------------------------------------------------------------------
// bus_backend_mock — all premium flags enabled (for test coverage)
// ---------------------------------------------------------------------------

template<>
struct osal_signal_capabilities<bus_backend_mock>
{
    static constexpr bool native_pubsub    = true;
    static constexpr bool native_observers = true;
    static constexpr bool native_routing   = true;
    static constexpr bool zero_copy        = true;
};

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

/// @brief Satisfied by any recognised channel backend tag type.
template<typename BackendTag>
concept bus_backend_tag =
    std::same_as<BackendTag, bus_backend_generic> || std::same_as<BackendTag, bus_backend_zephyr> ||
    std::same_as<BackendTag, bus_backend_freertos> || std::same_as<BackendTag, bus_backend_posix> ||
    std::same_as<BackendTag, bus_backend_bare_metal> || std::same_as<BackendTag, bus_backend_threadx> ||
    std::same_as<BackendTag, bus_backend_px5> || std::same_as<BackendTag, bus_backend_linux> ||
    std::same_as<BackendTag, bus_backend_vxworks> || std::same_as<BackendTag, bus_backend_nuttx> ||
    std::same_as<BackendTag, bus_backend_micrium> || std::same_as<BackendTag, bus_backend_chibios> ||
    std::same_as<BackendTag, bus_backend_embos> || std::same_as<BackendTag, bus_backend_qnx> ||
    std::same_as<BackendTag, bus_backend_rtems> || std::same_as<BackendTag, bus_backend_integrity> ||
    std::same_as<BackendTag, bus_backend_cmsis_rtos> || std::same_as<BackendTag, bus_backend_cmsis_rtos2> ||
    std::same_as<BackendTag, bus_backend_mock>;

/// @brief Backend with native pub/sub (e.g. Zbus).
template<typename BackendTag>
concept native_pubsub_backend = bus_backend_tag<BackendTag> && osal_signal_capabilities<BackendTag>::native_pubsub;

/// @brief Backend with native observer/callback support.
template<typename BackendTag>
concept native_observer_backend = bus_backend_tag<BackendTag> && osal_signal_capabilities<BackendTag>::native_observers;

/// @brief Backend with native zero-copy publish.
template<typename BackendTag>
concept zero_copy_backend = bus_backend_tag<BackendTag> && osal_signal_capabilities<BackendTag>::zero_copy;

/// @brief Backend with native topic routing.
template<typename BackendTag>
concept native_routing_backend = bus_backend_tag<BackendTag> && osal_signal_capabilities<BackendTag>::native_routing;

/// @} // osal_signal_traits

}  // namespace osal
