// SPDX-License-Identifier: Apache-2.0
/// @file osal_bus_backend_zephyr.hpp
/// @brief Zephyr channel backend tag
/// @details Provides the osal::osal_bus specialisation for
///          osal::bus_backend_zephyr.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <osal/bus/detail/osal_bus_backend_generic.hpp>

#if defined(OSAL_BACKEND_ZEPHYR)
#include <osal/types.hpp>

#include <zephyr/kernel.h>

#include <array>
#include <cstdint>
#endif

#include <cstddef>

namespace osal
{

#if defined(OSAL_BACKEND_ZEPHYR)

namespace zephyr_bus_detail
{

[[nodiscard]] inline k_timeout_t to_zephyr_timeout(tick_t timeout_ticks) noexcept
{
    if (timeout_ticks == WAIT_FOREVER)
    {
        return K_FOREVER;
    }
    if (timeout_ticks == NO_WAIT)
    {
        return K_NO_WAIT;
    }
    return K_MSEC(static_cast<int64_t>(timeout_ticks));
}

}  // namespace zephyr_bus_detail

/// @brief Zephyr-native channel backed by @c k_msgq.
template<queue_element T, std::size_t Capacity>
    requires(Capacity > 0U)
class osal_bus<T, Capacity, bus_backend_zephyr>
{
public:
    using value_type                      = T;
    static constexpr std::size_t capacity = Capacity;

    osal_bus() noexcept
    {
        k_msgq_init(&queue_, reinterpret_cast<char*>(storage_), sizeof(T), Capacity);
        valid_ = true;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    bool try_send(const T& item) noexcept { return k_msgq_put(&queue_, &item, K_NO_WAIT) == 0; }

    bool send(const T& item, tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        return k_msgq_put(&queue_, &item, zephyr_bus_detail::to_zephyr_timeout(timeout_ticks)) == 0;
    }

    bool try_receive(T& out) noexcept { return k_msgq_get(&queue_, &out, K_NO_WAIT) == 0; }

    bool receive(T& out, tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        return k_msgq_get(&queue_, &out, zephyr_bus_detail::to_zephyr_timeout(timeout_ticks)) == 0;
    }

private:
    template<typename U, std::size_t MaxSignalSubscribers, std::size_t SignalPerSubCapacity, typename BackendTag>
    friend class osal_signal;

    void purge() noexcept
    {
        std::array<std::byte, sizeof(T)> scratch{};
        while (k_msgq_get(&queue_, scratch.data(), K_NO_WAIT) == 0)
        {
        }
    }

    bool valid_{false};
    struct k_msgq queue_
    {
    };
    alignas(T) std::uint8_t storage_[Capacity * sizeof(T)]{};
};

#else

/// @brief Zephyr-tagged channel that delegates to the generic runtime on non-Zephyr builds.
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

#endif

}  // namespace osal
