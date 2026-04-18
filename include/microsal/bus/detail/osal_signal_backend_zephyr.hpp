// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_backend_zephyr.hpp
/// @brief Zephyr Zbus-backed channel + topic backend (skeleton)
/// @details Maps osal_bus and osal_signal onto Zephyr's Zbus:
///          - osal_bus<T, N, bus_backend_zephyr> wraps a Zbus channel.
///          - osal_signal<T, N, M, bus_backend_zephyr> uses Zbus pub/sub
///            with native observer callbacks.
///
///          This file is a **skeleton** — Zbus integration requires access to
///          the Zephyr build system and its generated channel definitions.
///          The TODO markers below indicate where Zbus API calls belong.
///
///          Premium capabilities for this backend:
///          - native_pubsub = true
///          - native_observers = true
///          - native_routing = true
///          - zero_copy = true
///
/// @note This header is conditionally compiled; it does nothing unless
///       OSAL_BACKEND_ZEPHYR is defined.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <microsal/bus/detail/osal_bus_fwd.hpp>
#include <osal/types.hpp>
#include <cstddef>
#include <functional>

#if defined(OSAL_BACKEND_ZEPHYR)
// Only compiled when building against the Zephyr RTOS.
// TODO: Include Zephyr Zbus headers when integrating.
// #include <zephyr/zbus/zbus.h>
#endif

namespace osal
{

// ===========================================================================
// osal_bus<T, Capacity, bus_backend_zephyr>
// ===========================================================================

/// @brief Zephyr Zbus-backed channel (skeleton).
/// @details In a full Zephyr integration each osal_bus<T, N> instance
///          corresponds to one ZBUS_CHAN_DEFINE channel.  Publish maps to
///          zbus_chan_pub(); receive maps to a Zbus subscriber listener queue.
///
///          @par TODO (Zephyr integration)
///          - Replace the stub storage with a real Zbus channel reference.
///          - Implement try_send() via zbus_chan_pub(chan, &item, K_NO_WAIT).
///          - Implement receive() via zbus_sub_wait() on a per-subscriber k_msgq.
template<queue_element T, std::size_t Capacity>
    requires(Capacity > 0U)
class osal_bus<T, Capacity, bus_backend_zephyr>
{
public:
    using value_type                      = T;
    static constexpr std::size_t capacity = Capacity;

    osal_bus() noexcept = default;

    [[nodiscard]] bool valid() const noexcept
    {
        // TODO: return (zbus_chan != nullptr);
        return true;
    }

    /// @brief Non-blocking publish to the Zbus channel.
    /// @todo zbus_chan_pub(chan_, &item, K_NO_WAIT)
    bool try_send([[maybe_unused]] const T& item) noexcept
    {
        // TODO: return zbus_chan_pub(&chan_, &item, K_NO_WAIT) == 0;
        return false;
    }

    /// @brief Publish with a timeout.
    /// @todo zbus_chan_pub(chan_, &item, K_MSEC(timeout_ms))
    bool send([[maybe_unused]] const T& item, [[maybe_unused]] tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        // TODO: convert ticks → k_timeout_t; call zbus_chan_pub.
        return false;
    }

    /// @brief Non-blocking receive from subscriber listener queue.
    /// @todo zbus_sub_wait(sub_, &out, K_NO_WAIT)
    bool try_receive([[maybe_unused]] T& out) noexcept
    {
        // TODO: return zbus_sub_wait(&sub_, &out, K_NO_WAIT) == 0;
        return false;
    }

    /// @brief Blocking receive with timeout.
    bool receive([[maybe_unused]] T& out, [[maybe_unused]] tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        // TODO: convert ticks → k_timeout_t; call zbus_sub_wait.
        return false;
    }

private:
    // TODO: Zbus channel handle / reference (e.g. ZBUS_CHAN_DECLARE(chan_name);)
};

// ===========================================================================
// osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_zephyr>
// ===========================================================================

/// @brief Zephyr Zbus-backed pub/sub topic (skeleton).
/// @details Maps each osal_signal<T> to a Zbus channel.
///
///          LCD semantics preserved:
///          - subscribe() → register a Zbus subscriber (ZBUS_SUBSCRIBER_DEFINE)
///          - publish()   → zbus_chan_pub()
///          - receive()   → zbus_sub_wait() on the subscriber's message queue
///
///          Premium additions (native_observers = true):
///          - Observer callbacks invoked synchronously by Zbus listener thread.
///
///          @par TODO (Zephyr integration)
///          - Use ZBUS_CHAN_DEFINE() macro at file scope for the channel.
///          - Use ZBUS_SUBSCRIBER_DEFINE() for each subscriber slot.
///          - Map publish() → zbus_chan_pub().
///          - Map subscribe() → zbus_obs_attach_to_chan().
template<queue_element T, std::size_t MaxSubscribers, std::size_t PerSubCapacity>
    requires(MaxSubscribers > 0U && PerSubCapacity > 0U)
class osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_zephyr>
{
public:
    using value_type = T;

    [[nodiscard]] bool subscribe([[maybe_unused]] subscriber_id& out_id) noexcept
    {
        // TODO: allocate Zbus subscriber slot; zbus_obs_attach_to_chan()
        return false;
    }

    [[nodiscard]] bool unsubscribe([[maybe_unused]] subscriber_id id) noexcept
    {
        // TODO: zbus_obs_detach_from_chan()
        return false;
    }

    [[nodiscard]] bool publish([[maybe_unused]] const T& msg) noexcept
    {
        // TODO: return zbus_chan_pub(&chan_, &msg, K_NO_WAIT) == 0;
        return false;
    }

    [[nodiscard]] bool try_receive([[maybe_unused]] subscriber_id id, [[maybe_unused]] T& out) noexcept
    {
        // TODO: zbus_sub_wait(slot, &out, K_NO_WAIT)
        return false;
    }

    [[nodiscard]] bool receive([[maybe_unused]] subscriber_id id, [[maybe_unused]] T& out,
                               [[maybe_unused]] tick_t timeout = WAIT_FOREVER) noexcept
    {
        // TODO: zbus_sub_wait(slot, &out, k_timeout)
        return false;
    }

    [[nodiscard]] std::size_t subscriber_count() const noexcept
    {
        // TODO: query Zbus observer count
        return 0U;
    }

    static constexpr std::size_t max_subscribers() noexcept { return MaxSubscribers; }
    static constexpr std::size_t per_subscriber_capacity() noexcept { return PerSubCapacity; }

private:
    // TODO: Zbus channel handle / ZBUS_CHAN_DEFINE reference
};

}  // namespace osal
