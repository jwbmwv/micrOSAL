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

    [[nodiscard]] bool valid() const noexcept { return impl_.valid(); }
    bool               try_send(const T& item) noexcept { return impl_.try_send(item); }
    bool               send(const T& item, tick_t t = WAIT_FOREVER) noexcept { return impl_.send(item, t); }
    bool               try_receive(T& out) noexcept { return impl_.try_receive(out); }
    bool               receive(T& out, tick_t t = WAIT_FOREVER) noexcept { return impl_.receive(out, t); }

private:
    osal_bus<T, Capacity, bus_backend_generic> impl_{};
};

}  // namespace osal
