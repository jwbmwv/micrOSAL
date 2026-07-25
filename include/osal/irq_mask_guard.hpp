// SPDX-License-Identifier: Apache-2.0
/// @file irq_mask_guard.hpp
/// @brief Draft OSAL IRQ-mask guard API
/// @details Provides osal::irq_mask_guard, a native-only draft abstraction for
///          very short sections that need local interrupt masking. This is not
///          a mutex, not a scheduler lock, and not an SMP-wide exclusion
///          primitive. Bare-metal uses save/restore IRQ-mask hooks, Zephyr
///          maps to irq_lock()/irq_unlock(), and FreeRTOS uses a task-context
///          critical section. On FreeRTOS, construct this guard only from task
///          context.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_irq_mask_guard
#pragma once

#include "backends.hpp"

#include <cstdint>

#if defined(OSAL_BACKEND_ZEPHYR)
#include <zephyr/kernel.h>
#elif defined(OSAL_BACKEND_FREERTOS)
#include "FreeRTOS.h"
#include "task.h"
#endif

#if defined(OSAL_BACKEND_BAREMETAL)
#if defined(OSAL_BM_IRQ_LOCK) && !defined(OSAL_BM_IRQ_UNLOCK)
#error "OSAL_BM_IRQ_LOCK() requires a matching OSAL_BM_IRQ_UNLOCK(state)."
#endif

#if !defined(OSAL_BM_IRQ_LOCK) && defined(OSAL_BM_IRQ_UNLOCK)
#error "OSAL_BM_IRQ_UNLOCK(state) requires a matching OSAL_BM_IRQ_LOCK()."
#endif

#if defined(OSAL_BM_IRQ_STATE_T) && !defined(OSAL_BM_IRQ_LOCK)
#error "OSAL_BM_IRQ_STATE_T requires OSAL_BM_IRQ_LOCK() and OSAL_BM_IRQ_UNLOCK(state)."
#endif
#endif

namespace osal
{

namespace detail
{

#if defined(OSAL_BACKEND_ZEPHYR)
using irq_mask_guard_state_t = unsigned int;

[[nodiscard]] inline irq_mask_guard_state_t irq_mask_guard_lock() noexcept
{
    return irq_lock();
}

inline void irq_mask_guard_unlock(irq_mask_guard_state_t state) noexcept
{
    irq_unlock(state);
}
#elif defined(OSAL_BACKEND_FREERTOS)
using irq_mask_guard_state_t = std::uintptr_t;

[[nodiscard]] inline irq_mask_guard_state_t irq_mask_guard_lock() noexcept
{
    // taskENTER_CRITICAL() is valid only from task context; ISR context must
    // use taskENTER_CRITICAL_FROM_ISR(). Integrators may provide a portable
    // port-specific probe when their FreeRTOS port exposes one.
#if defined(OSAL_FREERTOS_IS_IN_ISR)
    configASSERT(!OSAL_FREERTOS_IS_IN_ISR());
#endif
    taskENTER_CRITICAL();
    return 0U;
}

inline void irq_mask_guard_unlock(irq_mask_guard_state_t state) noexcept
{
    (void)state;
    taskEXIT_CRITICAL();
}
#elif defined(OSAL_BACKEND_BAREMETAL)
#if defined(OSAL_BM_IRQ_STATE_T)
using irq_mask_guard_state_t = OSAL_BM_IRQ_STATE_T;
#else
using irq_mask_guard_state_t = std::uintptr_t;
#endif

#if defined(OSAL_BM_IRQ_LOCK)
[[nodiscard]] inline irq_mask_guard_state_t irq_mask_guard_lock() noexcept
{
    return OSAL_BM_IRQ_LOCK();
}

inline void irq_mask_guard_unlock(irq_mask_guard_state_t state) noexcept
{
    OSAL_BM_IRQ_UNLOCK(state);
}
#elif defined(OSAL_BM_TEST_SELF_TICK)
[[nodiscard]] inline irq_mask_guard_state_t irq_mask_guard_lock() noexcept
{
    return irq_mask_guard_state_t{};
}

inline void irq_mask_guard_unlock(irq_mask_guard_state_t state) noexcept
{
    (void)state;
}
#elif defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')
[[nodiscard]] inline irq_mask_guard_state_t irq_mask_guard_lock() noexcept
{
    irq_mask_guard_state_t state{};
    // Save PRIMASK and disable all interrupts in a single compiler-indivisible unit.
    __asm volatile("mrs %0, primask\n\t"
                   "cpsid i"
                   : "=r"(state)
                   :
                   : "memory");
    return state;
}

inline void irq_mask_guard_unlock(irq_mask_guard_state_t state) noexcept
{
    __asm volatile("msr primask, %0" : : "r"(state) : "memory");
}
#else
using irq_mask_guard_state_t = std::uintptr_t;

[[nodiscard]] inline irq_mask_guard_state_t irq_mask_guard_lock() noexcept
{
    return 0U;
}

inline void irq_mask_guard_unlock(irq_mask_guard_state_t state) noexcept
{
    (void)state;
}
#endif
#else
using irq_mask_guard_state_t = std::uintptr_t;

[[nodiscard]] inline irq_mask_guard_state_t irq_mask_guard_lock() noexcept
{
    return 0U;
}

inline void irq_mask_guard_unlock(irq_mask_guard_state_t state) noexcept
{
    (void)state;
}
#endif

}  // namespace detail

/// @defgroup osal_irq_mask_guard OSAL IRQ Mask Guard
/// @brief Draft native-only guard for short interrupt-masked sections.
/// @{

class irq_mask_guard
{
public:
    /// @brief True when the active backend supports the draft IRQ-mask guard.
    static constexpr bool is_supported = supports_requirement<support_requirement::irq_mask_guard>;

    /// @brief Enforce IRQ-mask-guard support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_support()
    {
        require_backend_support<support_requirement::irq_mask_guard, Backend>();
    }

    /// @brief Constructs the guard.
    /// @details Acquires the backend's IRQ-mask token on supported backends.
    ///          On FreeRTOS this maps to a task critical section.
    irq_mask_guard() noexcept
    {
        if constexpr (is_supported)
        {
            state_ = detail::irq_mask_guard_lock();
        }
    }

    /// @brief Destructs the guard.
    ~irq_mask_guard() noexcept
    {
        if constexpr (is_supported)
        {
            detail::irq_mask_guard_unlock(state_);
        }
    }

    irq_mask_guard(const irq_mask_guard&)            = delete;
    irq_mask_guard& operator=(const irq_mask_guard&) = delete;
    irq_mask_guard(irq_mask_guard&&)                 = delete;
    irq_mask_guard& operator=(irq_mask_guard&&)      = delete;

    /// @brief Returns true when the active backend supports the IRQ-mask guard.
    /// @details This is a compile-time constant; the guard always holds its
    ///          mask for its full lifetime on supported backends.
    [[nodiscard]] static constexpr bool active() noexcept { return is_supported; }

private:
    detail::irq_mask_guard_state_t state_{};
};

/// @} // osal_irq_mask_guard

}  // namespace osal
