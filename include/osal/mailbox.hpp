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

/// @brief Typed single-slot message exchange built on a queue.
/// @details Uses the same blocking, timeout, and ISR queue semantics as
///          @c osal::queue<T, 1>. ISR methods are valid only when the active
///          backend advertises ISR queue support. The wrapper owns its queue;
///          it performs no dynamic allocation.
/// @tparam T  Message type satisfying @c osal::queue_element.
template<queue_element T>
class mailbox
{
public:
    /// @brief Type of values sent through the mailbox.
    using value_type = T;
    /// @brief Fixed capacity of this single-slot mailbox.
    static constexpr queue_depth_t capacity = 1U;

    /// @brief Constructs the underlying one-slot queue.
    /// @details Check valid() before using the mailbox.
    mailbox() noexcept = default;
    /// @brief Destroys the underlying queue.
    /// @details No concurrent operations or waiters may remain.
    ~mailbox() noexcept = default;

    /// @brief Copy construction is disabled; the queue storage has one owner.
    mailbox(const mailbox&) = delete;
    /// @brief Copy assignment is disabled; the queue storage has one owner.
    mailbox& operator=(const mailbox&) = delete;
    /// @brief Move construction is disabled; the queue handle is not transferable.
    mailbox(mailbox&&) = delete;
    /// @brief Move assignment is disabled; the queue handle is not transferable.
    mailbox& operator=(mailbox&&) = delete;

    /// @brief Reports whether the underlying queue initialized successfully.
    /// @return true if the underlying queue is initialized.
    [[nodiscard]] bool valid() const noexcept { return slot_.valid(); }
    /// @brief Reports whether the mailbox currently contains no item.
    /// @return true if the slot is empty.
    [[nodiscard]] bool empty() const noexcept { return slot_.empty(); }
    /// @brief Reports whether its single slot is occupied.
    /// @return true if the slot contains an item.
    [[nodiscard]] bool full() const noexcept { return slot_.full(); }
    /// @brief Returns the number of queued items (zero or one).
    /// @return Current item count, either zero or one.
    [[nodiscard]] std::size_t count() const noexcept { return slot_.count(); }

    /// @brief Sends one item, waiting indefinitely if the slot is occupied.
    /// @param[in] item Item to enqueue.
    /// @return The underlying queue result; backend errors are returned unchanged.
    /// @details Thread context only; the mailbox must outlive the operation.
    result send(const T& item) noexcept { return slot_.send(item); }
    /// @brief Attempts to send one item without waiting.
    /// @param[in] item Item to enqueue.
    /// @return true if enqueued, false if the slot is unavailable or the send fails.
    /// @details Thread context only.
    bool try_send(const T& item) noexcept { return slot_.try_send(item); }
    /// @brief Sends one item, waiting up to the supplied duration.
    /// @param[in] item    Item to enqueue.
    /// @param[in] timeout Maximum wait; a non-positive duration does not wait.
    ///                    Backend timed-queue support determines whether waiting is available.
    /// @return true if enqueued before the call returned; false otherwise.
    bool send_for(const T& item, milliseconds timeout) noexcept { return slot_.send_for(item, timeout); }
    /// @brief Attempts to send one item from ISR context without waiting.
    /// @param[in] item Item to enqueue.
    /// @return true if the underlying ISR queue operation succeeded.
    /// @warning Use from ISR only when the active backend advertises ISR queue support.
    bool send_isr(const T& item) noexcept { return slot_.send_isr(item); }

    /// @brief Receives one item, waiting indefinitely while the slot is empty.
    /// @param[out] item Receives the dequeued item.
    /// @return The underlying queue result; backend errors are returned unchanged.
    /// @details Thread context only; the mailbox must outlive the operation.
    result receive(T& item) noexcept { return slot_.receive(item); }
    /// @brief Attempts to receive one item without waiting.
    /// @param[out] item Receives the item when one is available.
    /// @return true if an item was received.
    bool try_receive(T& item) noexcept { return slot_.try_receive(item); }
    /// @brief Receives one item, waiting up to the supplied duration.
    /// @param[out] item    Receives the item when one is available.
    /// @param[in]  timeout Maximum wait; a non-positive duration does not wait.
    ///                     Backend timed-queue support determines whether waiting is available.
    /// @return true if an item was received before the call returned.
    bool receive_for(T& item, milliseconds timeout) noexcept { return slot_.receive_for(item, timeout); }
    /// @brief Attempts to receive one item from ISR context without waiting.
    /// @param[out] item Receives the item when one is available.
    /// @return true if the underlying ISR queue operation succeeded.
    /// @warning Use from ISR only when the active backend advertises ISR queue support.
    bool receive_isr(T& item) noexcept { return slot_.receive_isr(item); }
    /// @brief Copies the queued item without removing it.
    /// @param[out] item Receives the item when one is available.
    /// @return true if the item was copied; false if empty or the operation failed.
    bool peek(T& item) noexcept { return slot_.peek(item); }

    /// @brief Alias for send().
    /// @param[in] item Item to enqueue.
    /// @return The underlying queue result.
    result post(const T& item) noexcept { return send(item); }
    /// @brief Non-blocking alias for try_send().
    /// @param[in] item Item to enqueue.
    /// @return true if enqueued.
    bool try_post(const T& item) noexcept { return try_send(item); }
    /// @brief Timed alias for send_for().
    /// @param[in] item    Item to enqueue.
    /// @param[in] timeout Maximum wait.
    /// @return true if enqueued before the call returned.
    bool post_for(const T& item, milliseconds timeout) noexcept { return send_for(item, timeout); }

private:
    queue<T, 1U> slot_;
};

/// @} // osal_mailbox

}  // namespace osal
