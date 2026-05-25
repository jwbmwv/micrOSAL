// SPDX-License-Identifier: Apache-2.0
/// @file osal_bus.hpp
/// @brief Typed, fixed-capacity point-to-point channel
/// @details Provides osal::osal_bus<T, Capacity, BackendTag> — a typed,
///          bounded, zero-allocation message channel.  The template is
///          specialised per backend tag; include this header to get all
///          available specialisations.
///
///          Also defines (via osal_bus_fwd.hpp):
///          - osal::subscriber_id  — opaque subscriber token
///          - osal::invalid_subscriber_id — sentinel value
///
///          Typical usage (generic backend, queue-backed):
///          @code
///          osal::osal_bus<std::uint32_t, 8> ch;
///          ch.try_send(42U);
///          std::uint32_t v{};
///          ch.try_receive(v);  // v == 42
///          @endcode
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

/// @defgroup osal_bus OSAL Channel
/// @brief Fixed-capacity, typed point-to-point channels.
/// @{

// Forward declarations (subscriber_id, primary templates, traits).
#include <osal/bus/detail/osal_bus_fwd.hpp>

/// @} // osal_bus

// ---------------------------------------------------------------------------
// Channel backend specialisations.
// ---------------------------------------------------------------------------
#include <osal/bus/detail/osal_bus_backend_generic.hpp>
#include <osal/bus/detail/osal_bus_backend_zephyr.hpp>
#include <osal/bus/detail/osal_bus_backend_delegated.hpp>
#include <osal/bus/detail/osal_bus_backend_mock.hpp>
