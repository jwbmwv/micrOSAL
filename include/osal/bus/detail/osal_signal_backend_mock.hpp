// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_backend_mock.hpp
/// @brief Mock premium topic backend for unit tests
/// @details Provides the osal::osal_signal specialisation for
///          osal::bus_backend_mock.
///
///          The mock backend is identical to bus_backend_generic in its
///          LCD behaviour (uses the same osal::queue + osal::mutex primitives)
///          but its osal_signal_capabilities<bus_backend_mock> flags all
///          premium features as @c true.  This lets test builds exercise
///          osal_signal_premium premium code paths without requiring Zephyr or
///          another premium-capable RTOS.
///
///          Additionally, the mock topic exposes lightweight backend-owned
///          observer hooks so premium tests can exercise backend-managed
///          observer registration and dispatch without requiring Zephyr.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <osal/bus/detail/osal_signal_backend_generic.hpp>

#include <array>
#include <cstddef>

namespace osal
{

// ===========================================================================
// osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_mock>
// ===========================================================================

/// @brief Mock LCD pub/sub topic — generic behaviour, premium capability flags.
/// @details Inherits from the generic specialisation and keeps the same LCD
///          interface while exposing backend-owned observer hooks used by
///          @c osal_signal_premium when it detects
///          @c native_observer_backend<bus_backend_mock>.
template<queue_element T, std::size_t MaxSubscribers, std::size_t PerSubCapacity>
    requires(MaxSubscribers > 0U && PerSubCapacity > 0U)
class osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_mock>
    : public osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>
{
public:
    using base_type   = osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>;
    using value_type  = T;
    using observer_fn = void (*)(const T&) noexcept;

    // All LCD methods inherited from base_type.

    [[nodiscard]] bool native_subscribe_observer(observer_fn fn) noexcept
    {
        if (fn == nullptr || observer_count_ >= max_observers_)
        {
            return false;
        }
        observers_[observer_count_++] = fn;
        return true;
    }

    [[nodiscard]] bool native_unsubscribe_observer(observer_fn fn) noexcept
    {
        if (fn == nullptr)
        {
            return false;
        }
        for (std::size_t i = 0U; i < observer_count_; ++i)
        {
            if (observers_[i] == fn)
            {
                observers_[i]               = observers_[--observer_count_];
                observers_[observer_count_] = nullptr;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t native_observer_count() const noexcept { return observer_count_; }

    [[nodiscard]] bool native_publish_observers(const T& msg) noexcept
    {
        const bool any = observer_count_ > 0U;
        for (std::size_t i = 0U; i < observer_count_; ++i)
        {
            if (observers_[i] != nullptr)
            {
                observers_[i](msg);
            }
        }
        return any;
    }

private:
    static constexpr std::size_t max_observers_ = MaxSubscribers;

    std::array<observer_fn, max_observers_> observers_{};
    std::size_t                             observer_count_{0U};
};

}  // namespace osal
