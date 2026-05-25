// SPDX-License-Identifier: Apache-2.0
/// @file osal_bus_backend_delegated.hpp
/// @brief Delegated channel backends for non-Zephyr RTOS/OS targets
/// @details Stamps out thin osal::osal_bus specialisations that delegate to
///          the generic channel runtime.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <osal/bus/detail/osal_bus_backend_generic.hpp>

#include <cstddef>

namespace osal
{

/// @cond INTERNAL_MACROS

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — template specialisation generator
#define OSAL_BUS_DELEGATE_BACKEND_(Tag_)                           \
                                                                   \
    template<queue_element T, std::size_t Capacity>                \
        requires(Capacity > 0U)                                    \
    class osal_bus<T, Capacity, Tag_>                              \
    {                                                              \
    public:                                                        \
        using value_type                      = T;                 \
        static constexpr std::size_t capacity = Capacity;          \
                                                                   \
        [[nodiscard]] bool valid() const noexcept                  \
        {                                                          \
            return impl_.valid();                                  \
        }                                                          \
        bool try_send(const T& item) noexcept                      \
        {                                                          \
            return impl_.try_send(item);                           \
        }                                                          \
        bool send(const T& item, tick_t t = WAIT_FOREVER) noexcept \
        {                                                          \
            return impl_.send(item, t);                            \
        }                                                          \
        bool try_receive(T& out) noexcept                          \
        {                                                          \
            return impl_.try_receive(out);                         \
        }                                                          \
        bool receive(T& out, tick_t t = WAIT_FOREVER) noexcept     \
        {                                                          \
            return impl_.receive(out, t);                          \
        }                                                          \
                                                                   \
    private:                                                       \
        osal_bus<T, Capacity, bus_backend_generic> impl_{};        \
    }

OSAL_BUS_DELEGATE_BACKEND_(bus_backend_freertos);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_posix);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_bare_metal);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_threadx);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_px5);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_linux);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_vxworks);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_nuttx);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_micrium);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_chibios);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_embos);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_qnx);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_rtems);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_integrity);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_cmsis_rtos);
OSAL_BUS_DELEGATE_BACKEND_(bus_backend_cmsis_rtos2);

#undef OSAL_BUS_DELEGATE_BACKEND_

/// @endcond

}  // namespace osal