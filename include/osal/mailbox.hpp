// SPDX-License-Identifier: Apache-2.0
/// @file mailbox.hpp
/// @brief OSAL mailbox — single-slot typed message exchange
/// @details Provides osal::mailbox<T>, a thin wrapper over osal::queue<T, 1>.
///          It models the common RTOS mailbox primitive as a single-slot queue
///          with static storage, blocking send/receive operations, and optional
///          ISR-safe access when the active backend supports ISR queues.
///
///          Design rules:
///          - No dynamic allocation.
///          - Same type constraints and backend semantics as osal::queue.
///          - C users can model the same primitive with osal_c_queue_create(..., capacity=1).
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_mailbox
#pragma once

#include "queue.hpp"

namespace osal
{

/// @defgroup osal_mailbox OSAL Mailbox
/// @brief Single-slot typed mailbox built on top of osal::queue.
/// @{

template<queue_element T>
class mailbox
{
public:
    using value_type                        = T;
    static constexpr queue_depth_t capacity = 1U;

    mailbox() noexcept  = default;
    ~mailbox() noexcept = default;

    mailbox(const mailbox&)            = delete;
    mailbox& operator=(const mailbox&) = delete;
    mailbox(mailbox&&)                 = delete;
    mailbox& operator=(mailbox&&)      = delete;

    [[nodiscard]] bool        valid() const noexcept { return slot_.valid(); }
    [[nodiscard]] bool        empty() const noexcept { return slot_.empty(); }
    [[nodiscard]] bool        full() const noexcept { return slot_.full(); }
    [[nodiscard]] std::size_t count() const noexcept { return slot_.count(); }

    result send(const T& item) noexcept { return slot_.send(item); }
    bool   try_send(const T& item) noexcept { return slot_.try_send(item); }
    bool   send_for(const T& item, milliseconds timeout) noexcept { return slot_.send_for(item, timeout); }
    bool   send_isr(const T& item) noexcept { return slot_.send_isr(item); }

    result receive(T& item) noexcept { return slot_.receive(item); }
    bool   try_receive(T& item) noexcept { return slot_.try_receive(item); }
    bool   receive_for(T& item, milliseconds timeout) noexcept { return slot_.receive_for(item, timeout); }
    bool   receive_isr(T& item) noexcept { return slot_.receive_isr(item); }
    bool   peek(T& item) noexcept { return slot_.peek(item); }

    result post(const T& item) noexcept { return send(item); }
    bool   try_post(const T& item) noexcept { return try_send(item); }
    bool   post_for(const T& item, milliseconds timeout) noexcept { return send_for(item, timeout); }

private:
    queue<T, 1U> slot_;
};

/// @} // osal_mailbox

}  // namespace osal
