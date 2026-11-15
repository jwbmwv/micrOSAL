// SPDX-License-Identifier: Apache-2.0
/// @file osal_bus_backend_mock.hpp
/// @brief Mock channel backend for unit tests
/// @details Provides the osal::osal_bus specialisation for
///          osal::bus_backend_mock by delegating to the generic channel
///          runtime.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <osal/bus/detail/osal_bus_backend_generic.hpp>

#include <cstddef>

namespace osal
{

/// @brief Mock channel — delegates entirely to the generic backend.
template<queue_element T, std::size_t Capacity>
    requires(Capacity > 0U)
class osal_bus<T, Capacity, bus_backend_mock>
{
public:
    using value_type                      = T;
    static constexpr std::size_t capacity = Capacity;

    /// @brief Reports whether the delegated queue was initialized.
    /// @return @c true when channel operations can use the queue.
    [[nodiscard]] bool valid() const noexcept { return impl_.valid(); }
    /// @brief Attempts to enqueue an item without waiting.
    /// @param[in] item  Item copied into the channel if accepted.
    /// @return @c true if enqueued; @c false if invalid or full.
    bool try_send(const T& item) noexcept { return impl_.try_send(item); }
    /// @brief Enqueues an item, waiting up to the specified number of ticks.
    /// @param[in] item  Item copied into the channel if accepted.
    /// @param[in] t     Maximum wait; @c NO_WAIT polls and @c WAIT_FOREVER waits indefinitely.
    /// @return @c true if enqueued; @c false on timeout or another queue failure.
    bool send(const T& item, tick_t t = WAIT_FOREVER) noexcept { return impl_.send(item, t); }
    /// @brief Attempts to dequeue an item without waiting.
    /// @param[out] out  Receives the item when one is available.
    /// @return @c true if an item was received; @c false if invalid or empty.
    bool try_receive(T& out) noexcept { return impl_.try_receive(out); }
    /// @brief Dequeues an item, waiting up to the specified number of ticks.
    /// @param[out] out  Receives the item when successful.
    /// @param[in]  t    Maximum wait; @c NO_WAIT polls and @c WAIT_FOREVER waits indefinitely.
    /// @return @c true if an item was received; @c false on timeout or another queue failure.
    bool receive(T& out, tick_t t = WAIT_FOREVER) noexcept { return impl_.receive(out, t); }

private:
    osal_bus<T, Capacity, bus_backend_generic> impl_{};
};

}  // namespace osal
