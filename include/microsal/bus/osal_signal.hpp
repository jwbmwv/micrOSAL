// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal.hpp
/// @brief Typed, fixed-capacity pub/sub topic (LCD and premium)
/// @details Provides osal::osal_signal<T, MaxSubscribers, PerSubCapacity, BackendTag>
///          — the primary user-facing pub/sub abstraction.
///
///          LCD semantics are available on every backend:
///          @code
///          osal::osal_signal<std::uint32_t, 4, 8> topic;
///
///          osal::subscriber_id sub{osal::invalid_subscriber_id};
///          topic.subscribe(sub);
///
///          topic.publish(42U);
///
///          std::uint32_t val{};
///          topic.try_receive(sub, val);  // val == 42
///
///          topic.unsubscribe(sub);
///          @endcode
///
///          For premium features (observers, zero-copy, routing) see
///          @c osal_signal_premium.hpp.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <microsal/bus/osal_bus.hpp>

namespace osal
{

/// @defgroup osal_signal OSAL Topic (Pub/Sub)
/// @brief Typed, zero-allocation pub/sub topics with a per-subscriber queue model.
/// @{

// ---------------------------------------------------------------------------
// Primary template — intentionally incomplete
// ---------------------------------------------------------------------------

/// @brief Typed, fixed-capacity pub/sub topic.
/// @tparam T               Message type (trivially copyable + standard layout).
/// @tparam MaxSubscribers  Compile-time maximum number of concurrent subscribers.
/// @tparam PerSubCapacity  Depth of each subscriber's receive queue.
/// @tparam BackendTag      Backend implementation (defaults to MICROSAL_DEFAULT_BACKEND_TAG).
///
/// @details Specialisations are provided via the backend headers included by
///          osal_bus.hpp.  An unsupported BackendTag produces an "incomplete
///          type" compile error — choose a supported backend tag.
///
///          LCD API:
///          | Method                    | Semantics                                     |
///          |---------------------------|-----------------------------------------------|
///          | subscribe(id)             | Allocates a subscriber slot; returns its id.  |
///          | unsubscribe(id)           | Releases the slot.                            |
///          | publish(msg)              | Fans out to all active subscriber queues.     |
///          | try_receive(id, out)      | Non-blocking dequeue from subscriber queue.   |
///          | receive(id, out, timeout) | Blocking dequeue with optional timeout.       |
///          | subscriber_count()        | Current number of active subscribers.         |
template<typename T, std::size_t MaxSubscribers, std::size_t PerSubCapacity,
         typename BackendTag>
class osal_signal;  // intentionally incomplete — specialised per backend

/// @} // osal_signal

}  // namespace osal

// ---------------------------------------------------------------------------
// Backend specialisations are already included transitively via
// osal_bus.hpp → osal_signal_backend_generic.hpp / _zephyr.hpp / _mock.hpp
// No additional includes needed here.
// ---------------------------------------------------------------------------
