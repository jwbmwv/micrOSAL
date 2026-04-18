// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_backend_delegated.hpp
/// @brief Delegated channel + topic backends for all non-Zephyr RTOS/OS targets
/// @details All micrOSAL backends (except Zephyr, which has native Zbus support)
///          share the same C ABI for queue and mutex operations.  Rather than
///          duplicate the generic implementation, this header stamps out thin
///          osal_bus and osal_signal specialisations that delegate entirely
///          to the @c bus_backend_generic implementation.
///
///          Each per-backend tag is a distinct type so that:
///          - Users can explicitly request a specific backend.
///          - Future optimisation can replace any delegation with a native impl.
///          - The @c bus_backend_tag concept remains exhaustive.
///
///          Backends delegated here:
///          - bus_backend_freertos
///          - bus_backend_posix
///          - bus_backend_bare_metal
///          - bus_backend_threadx
///          - bus_backend_px5
///          - bus_backend_linux
///          - bus_backend_vxworks
///          - bus_backend_nuttx
///          - bus_backend_micrium
///          - bus_backend_chibios
///          - bus_backend_embos
///          - bus_backend_qnx
///          - bus_backend_rtems
///          - bus_backend_integrity
///          - bus_backend_cmsis_rtos
///          - bus_backend_cmsis_rtos2
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <microsal/bus/detail/osal_bus_fwd.hpp>
#include <microsal/bus/detail/osal_signal_backend_generic.hpp>

#include <cstddef>

namespace osal
{

// ---------------------------------------------------------------------------
// Macro to stamp out a delegated osal_bus + osal_signal pair
// ---------------------------------------------------------------------------

/// @cond INTERNAL_MACROS

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — template specialisation generator
#define OSAL_BUS_DELEGATE_BACKEND_(Tag_)                                                        \
                                                                                                \
    template<queue_element T, std::size_t Capacity>                                             \
        requires(Capacity > 0U)                                                                 \
    class osal_bus<T, Capacity, Tag_>                                                           \
    {                                                                                           \
    public:                                                                                     \
        using value_type                      = T;                                              \
        static constexpr std::size_t capacity = Capacity;                                       \
                                                                                                \
        [[nodiscard]] bool valid() const noexcept                                               \
        {                                                                                       \
            return impl_.valid();                                                               \
        }                                                                                       \
        bool try_send(const T& item) noexcept                                                   \
        {                                                                                       \
            return impl_.try_send(item);                                                        \
        }                                                                                       \
        bool send(const T& item, tick_t t = WAIT_FOREVER) noexcept                              \
        {                                                                                       \
            return impl_.send(item, t);                                                         \
        }                                                                                       \
        bool try_receive(T& out) noexcept                                                       \
        {                                                                                       \
            return impl_.try_receive(out);                                                      \
        }                                                                                       \
        bool receive(T& out, tick_t t = WAIT_FOREVER) noexcept                                  \
        {                                                                                       \
            return impl_.receive(out, t);                                                       \
        }                                                                                       \
                                                                                                \
    private:                                                                                    \
        osal_bus<T, Capacity, bus_backend_generic> impl_{};                                     \
    };                                                                                          \
                                                                                                \
    template<queue_element T, std::size_t MaxSubscribers, std::size_t PerSubCapacity>           \
        requires(MaxSubscribers > 0U && PerSubCapacity > 0U)                                    \
    class osal_signal<T, MaxSubscribers, PerSubCapacity, Tag_>                                  \
        : public osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>            \
    {                                                                                           \
    public:                                                                                     \
        using base_type  = osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>; \
        using value_type = T;                                                                   \
    }

// ---------------------------------------------------------------------------
// Stamp out delegated backends
// ---------------------------------------------------------------------------

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
