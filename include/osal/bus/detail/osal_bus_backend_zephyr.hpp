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
    return K_TICKS(static_cast<int64_t>(timeout_ticks));
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

    /// @brief Initializes the native message queue in this object's storage.
    osal_bus() noexcept
    {
        k_msgq_init(&queue_, reinterpret_cast<char*>(storage_), sizeof(T), Capacity);
        valid_ = true;
    }

    /// @brief Reports whether the native queue is initialized.
    /// @return @c true after successful construction.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    /// @brief Attempts to enqueue an item without waiting.
    /// @param[in] item  Item copied into the channel if accepted.
    /// @return @c true if enqueued; @c false if the queue is full.
    bool try_send(const T& item) noexcept { return k_msgq_put(&queue_, &item, K_NO_WAIT) == 0; }

    /// @brief Enqueues an item, waiting up to the specified number of ticks.
    /// @param[in] item           Item copied into the channel if accepted.
    /// @param[in] timeout_ticks  Maximum wait; @c NO_WAIT polls and @c WAIT_FOREVER waits indefinitely.
    /// @return @c true if enqueued; @c false on timeout or another queue failure.
    bool send(const T& item, tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        return k_msgq_put(&queue_, &item, zephyr_bus_detail::to_zephyr_timeout(timeout_ticks)) == 0;
    }

    /// @brief Attempts to dequeue an item without waiting.
    /// @param[out] out  Receives the item when one is available.
    /// @return @c true if an item was received; @c false if the queue is empty.
    bool try_receive(T& out) noexcept { return k_msgq_get(&queue_, &out, K_NO_WAIT) == 0; }

    /// @brief Dequeues an item, waiting up to the specified number of ticks.
    /// @param[out] out           Receives the item when successful.
    /// @param[in]  timeout_ticks Maximum wait; @c NO_WAIT polls and @c WAIT_FOREVER waits indefinitely.
    /// @return @c true if an item was received; @c false on timeout or another queue failure.
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

    /// @brief Reports whether the delegated queue was initialized.
    /// @return @c true when channel operations can use the queue.
    [[nodiscard]] bool valid() const noexcept { return impl_.valid(); }

    /// @brief Attempts to enqueue an item without waiting.
    /// @param[in] item  Item copied into the channel if accepted.
    /// @return @c true if enqueued; @c false if invalid or full.
    bool try_send(const T& item) noexcept { return impl_.try_send(item); }

    /// @brief Enqueues an item, waiting up to the specified number of ticks.
    /// @param[in] item           Item copied into the channel if accepted.
    /// @param[in] timeout_ticks  Maximum wait; @c NO_WAIT polls and @c WAIT_FOREVER waits indefinitely.
    /// @return @c true if enqueued; @c false on timeout or another queue failure.
    bool send(const T& item, tick_t timeout_ticks = WAIT_FOREVER) noexcept { return impl_.send(item, timeout_ticks); }

    /// @brief Attempts to dequeue an item without waiting.
    /// @param[out] out  Receives the item when one is available.
    /// @return @c true if an item was received; @c false if invalid or empty.
    bool try_receive(T& out) noexcept { return impl_.try_receive(out); }

    /// @brief Dequeues an item, waiting up to the specified number of ticks.
    /// @param[out] out           Receives the item when successful.
    /// @param[in]  timeout_ticks Maximum wait; @c NO_WAIT polls and @c WAIT_FOREVER waits indefinitely.
    /// @return @c true if an item was received; @c false on timeout or another queue failure.
    bool receive(T& out, tick_t timeout_ticks = WAIT_FOREVER) noexcept { return impl_.receive(out, timeout_ticks); }

private:
    osal_bus<T, Capacity, bus_backend_generic> impl_{};
};

#endif

}  // namespace osal
