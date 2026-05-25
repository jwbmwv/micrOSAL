// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_backend_delegated.hpp
/// @brief Delegated topic backends for non-Zephyr RTOS/OS targets
/// @details This header stamps out thin @c osal_signal specialisations that
///          delegate entirely to the @c bus_backend_generic implementation.
///          Delegated channel specialisations live in
///          @c osal_bus_backend_delegated.hpp.
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

#include <osal/bus/detail/osal_signal_backend_generic.hpp>

#include <cstddef>

namespace osal
{

// ---------------------------------------------------------------------------
// Macro to stamp out a delegated osal_signal specialisation
// ---------------------------------------------------------------------------

/// @cond INTERNAL_MACROS

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — template specialisation generator
#define OSAL_SIGNAL_DELEGATE_BACKEND_(Tag_)                                                     \
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

OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_freertos);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_posix);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_bare_metal);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_threadx);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_px5);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_linux);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_vxworks);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_nuttx);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_micrium);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_chibios);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_embos);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_qnx);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_rtems);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_integrity);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_cmsis_rtos);
OSAL_SIGNAL_DELEGATE_BACKEND_(bus_backend_cmsis_rtos2);

#undef OSAL_SIGNAL_DELEGATE_BACKEND_

/// @endcond

}  // namespace osal
