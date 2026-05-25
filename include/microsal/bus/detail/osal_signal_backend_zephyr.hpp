// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_backend_zephyr.hpp
/// @brief Zephyr bus + signal backend tag
/// @details Until a native Zbus runtime is implemented, the Zephyr backend tag
///          delegates to the generic MicrOSAL bus/signal runtime so the
///          default Zephyr bus tag remains usable.
///
///          Native premium capability traits stay disabled for
///          @c bus_backend_zephyr until the dedicated Zbus path exists.
///
///          This header intentionally keeps the Zephyr-specific specialisation
///          point so a future native implementation can replace the delegation
///          without changing public backend selection.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <microsal/bus/detail/osal_signal_backend_generic.hpp>

#include <cstddef>

namespace osal
{

// ===========================================================================
// osal_bus<T, Capacity, bus_backend_zephyr>
// ===========================================================================

/// @brief Zephyr-tagged channel that currently delegates to the generic runtime.
template<queue_element T, std::size_t Capacity>
    requires(Capacity > 0U)
class osal_bus<T, Capacity, bus_backend_zephyr>
{
public:
    using value_type                      = T;
    static constexpr std::size_t capacity = Capacity;

    [[nodiscard]] bool valid() const noexcept { return impl_.valid(); }

    bool try_send(const T& item) noexcept { return impl_.try_send(item); }

    bool send(const T& item, tick_t timeout_ticks = WAIT_FOREVER) noexcept { return impl_.send(item, timeout_ticks); }

    bool try_receive(T& out) noexcept { return impl_.try_receive(out); }

    bool receive(T& out, tick_t timeout_ticks = WAIT_FOREVER) noexcept { return impl_.receive(out, timeout_ticks); }

private:
    osal_bus<T, Capacity, bus_backend_generic> impl_{};
};

// ===========================================================================
// osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_zephyr>
// ===========================================================================

/// @brief Zephyr-tagged pub/sub topic that currently delegates to the generic runtime.
template<queue_element T, std::size_t MaxSubscribers, std::size_t PerSubCapacity>
    requires(MaxSubscribers > 0U && PerSubCapacity > 0U)
class osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_zephyr>
    : public osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>
{
public:
    using base_type  = osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>;
    using value_type = T;
};

}  // namespace osal
