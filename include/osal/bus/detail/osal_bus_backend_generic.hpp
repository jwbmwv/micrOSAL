// SPDX-License-Identifier: Apache-2.0
/// @file osal_bus_backend_generic.hpp
/// @brief Generic micrOSAL channel backend (LCD implementation)
/// @details Provides the osal::osal_bus specialisation for
///          osal::bus_backend_generic.
///
///          Implementation uses only micrOSAL primitives:
///          - queue storage via the C queue ABI
///          - no dynamic allocation
///          - exact tick_t timeout forwarding
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <osal/bus/detail/osal_bus_fwd.hpp>
#include <osal/error.hpp>
#include <osal/queue.hpp>
#include <osal/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace osal
{

/// @brief Generic channel specialisation — queue backed by the micrOSAL C ABI.
/// @tparam T         Message type (trivially copyable + standard layout).
/// @tparam Capacity  Maximum number of buffered messages.
///
/// @details This specialisation owns its backing storage and calls the
///          osal_queue_* C functions directly so that @c tick_t timeouts
///          are forwarded verbatim without a ticks↔ms round-trip.
template<queue_element T, std::size_t Capacity>
    requires(Capacity > 0U)
class osal_bus<T, Capacity, bus_backend_generic>
{
public:
    using value_type                        = T;
    static constexpr std::size_t capacity   = Capacity;
    static constexpr std::size_t value_size = sizeof(T);

    osal_bus() noexcept { valid_ = osal_queue_create(&handle_, storage_, value_size, Capacity).ok(); }

    ~osal_bus() noexcept
    {
        if (valid_)
        {
            (void)osal_queue_destroy(&handle_);
            valid_ = false;
        }
    }

    osal_bus(const osal_bus&)            = delete;
    osal_bus& operator=(const osal_bus&) = delete;
    osal_bus(osal_bus&&)                 = delete;
    osal_bus& operator=(osal_bus&&)      = delete;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    bool try_send(const T& item) noexcept { return osal_queue_send(&handle_, &item, NO_WAIT).ok(); }

    bool send(const T& item, tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        return osal_queue_send(&handle_, &item, timeout_ticks).ok();
    }

    bool try_receive(T& out) noexcept { return osal_queue_receive(&handle_, &out, NO_WAIT).ok(); }

    bool receive(T& out, tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        return osal_queue_receive(&handle_, &out, timeout_ticks).ok();
    }

private:
    template<typename U, std::size_t MaxSignalSubscribers, std::size_t SignalPerSubCapacity, typename BackendTag>
    friend class osal_signal;

    void purge() noexcept
    {
        if (!valid_)
        {
            return;
        }

        std::array<std::byte, value_size> scratch{};
        while (osal_queue_receive(&handle_, scratch.data(), NO_WAIT).ok())
        {
        }
    }

    bool                          valid_{false};
    active_traits::queue_handle_t handle_{};
    alignas(T) std::uint8_t storage_[Capacity * sizeof(T)]{};
};

}  // namespace osal