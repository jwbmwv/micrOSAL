// SPDX-License-Identifier: Apache-2.0
/// @file irq_mask_guard.hpp
/// @brief Draft OSAL IRQ-mask guard API
/// @details Provides osal::irq_mask_guard, a native-only draft abstraction for
///          very short sections that need local interrupt masking. This is not
///          a mutex, not a scheduler lock, and not an SMP-wide exclusion
///          primitive.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_irq_mask_guard
#pragma once

#include "backends.hpp"

namespace osal
{

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
    /// @details The draft implementation is a no-op until backends opt in.
    irq_mask_guard() noexcept = default;

    /// @brief Destructs the guard.
    ~irq_mask_guard() noexcept = default;

    irq_mask_guard(const irq_mask_guard&)            = delete;
    irq_mask_guard& operator=(const irq_mask_guard&) = delete;
    irq_mask_guard(irq_mask_guard&&)                 = delete;
    irq_mask_guard& operator=(irq_mask_guard&&)      = delete;

    /// @brief Returns true when the guard actively owns an interrupt mask.
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    bool active_{false};
};

/// @} // osal_irq_mask_guard

}  // namespace osal
