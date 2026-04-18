// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_backend_mock.hpp
/// @brief Mock premium channel + topic backend for unit tests
/// @details Provides osal::osal_bus and osal::osal_signal specialisations
///          for osal::bus_backend_mock.
///
///          The mock backend is identical to bus_backend_generic in its
///          LCD behaviour (uses the same osal::queue + osal::mutex primitives)
///          but its osal_signal_capabilities<bus_backend_mock> flags all
///          premium features as @c true.  This lets test builds exercise
///          osal_signal_premium premium code paths without requiring Zephyr or
///          another premium-capable RTOS.
///
///          Additionally, the mock topic exposes a lightweight in-class
///          observer table so that observer-subscription tests compile and
///          pass on any micrOSAL backend.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <microsal/bus/detail/osal_bus_fwd.hpp>
#include <microsal/bus/detail/osal_signal_backend_generic.hpp>

#include <array>
#include <cstddef>

namespace osal
{

// ===========================================================================
// osal_bus<T, Capacity, bus_backend_mock>
// ===========================================================================

/// @brief Mock channel — delegates entirely to the generic backend.
template<queue_element T, std::size_t Capacity>
    requires(Capacity > 0U)
class osal_bus<T, Capacity, bus_backend_mock>
{
public:
    using value_type                      = T;
    static constexpr std::size_t capacity = Capacity;

    [[nodiscard]] bool valid() const noexcept { return impl_.valid(); }
    bool               try_send(const T& item) noexcept { return impl_.try_send(item); }
    bool               send(const T& item, tick_t t = WAIT_FOREVER) noexcept { return impl_.send(item, t); }
    bool               try_receive(T& out) noexcept { return impl_.try_receive(out); }
    bool               receive(T& out, tick_t t = WAIT_FOREVER) noexcept { return impl_.receive(out, t); }

private:
    osal_bus<T, Capacity, bus_backend_generic> impl_{};
};

// ===========================================================================
// osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_mock>
// ===========================================================================

/// @brief Mock LCD pub/sub topic — generic behaviour, premium capability flags.
/// @details Inherits from the generic specialisation and keeps the same LCD
///          interface.  The premium observer table is added by osal_signal_premium
///          when it detects native_observer_backend<bus_backend_mock>.
template<queue_element T, std::size_t MaxSubscribers, std::size_t PerSubCapacity>
    requires(MaxSubscribers > 0U && PerSubCapacity > 0U)
class osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_mock>
    : public osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>
{
public:
    using base_type  = osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>;
    using value_type = T;

    // All LCD methods inherited from base_type.
};

}  // namespace osal
