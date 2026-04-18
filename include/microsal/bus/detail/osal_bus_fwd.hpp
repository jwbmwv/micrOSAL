// SPDX-License-Identifier: Apache-2.0
/// @file osal_bus_fwd.hpp
/// @brief Forward declarations for osal_bus and osal_signal primary templates
/// @details Provides the shared declarations that all backend specialisation
///          headers need without pulling in the full osal_bus.hpp.
///          This breaks the header-include cycle between osal_bus.hpp and
///          its backend detail headers.
///
///          Contents:
///          - osal::subscriber_id           — opaque subscriber slot index
///          - osal::invalid_subscriber_id   — sentinel value
///          - osal::osal_bus<T, N, Tag> — primary template (incomplete)
///          - osal::osal_signal<T, M, P, Tag> — primary template (incomplete)
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <microsal/bus/detail/osal_signal_traits.hpp>
#include <cstddef>
#include <limits>

namespace osal
{

// ---------------------------------------------------------------------------
// Subscriber identity
// ---------------------------------------------------------------------------

/// @brief Opaque index identifying one subscriber slot in an osal_signal.
using subscriber_id = std::size_t;

/// @brief Sentinel value returned when subscribe() fails or as an initialiser.
inline constexpr subscriber_id invalid_subscriber_id = std::numeric_limits<subscriber_id>::max();

// ---------------------------------------------------------------------------
// Primary templates — intentionally incomplete
// ---------------------------------------------------------------------------

/// @brief Typed, fixed-capacity channel between a producer and a consumer.
/// @tparam T           Message type.  Must be trivially copyable + standard layout.
/// @tparam Capacity    Maximum number of messages buffered (compile-time).
/// @tparam BackendTag  Channel implementation to use (defaults to MICROSAL_DEFAULT_BACKEND_TAG).
template<typename T, std::size_t Capacity, typename BackendTag = MICROSAL_DEFAULT_BACKEND_TAG>
class osal_bus;  // specialised per backend

/// @brief Typed, fixed-capacity pub/sub topic.
/// @tparam T               Message type (trivially copyable + standard layout).
/// @tparam MaxSubscribers  Compile-time maximum number of concurrent subscribers.
/// @tparam PerSubCapacity  Depth of each subscriber's receive queue.
/// @tparam BackendTag      Backend implementation (defaults to MICROSAL_DEFAULT_BACKEND_TAG).
template<typename T, std::size_t MaxSubscribers, std::size_t PerSubCapacity,
         typename BackendTag = MICROSAL_DEFAULT_BACKEND_TAG>
class osal_signal;  // specialised per backend

}  // namespace osal
