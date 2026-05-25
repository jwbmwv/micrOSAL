// SPDX-License-Identifier: Apache-2.0
/// @file queue.hpp
/// @brief OSAL static-storage message queue
/// @details Provides osal::queue<T, N> — a fixed-capacity, type-safe message queue
///          with static backing storage.  No dynamic allocation is ever performed.
///
///          Storage model:
///          - Each queue<T,N> contains an aligned_storage buffer of exactly
///            N * sizeof(T) bytes — no heap involvement.
///          - The backend uses that buffer directly (FreeRTOS static queue,
///            Zephyr k_msgq with static buffer, etc.).
///
///          ISR safety:
///          - send_isr() / receive_isr() are available when
///            capabilities<active_backend>::has_isr_queue == true.
///
///          MISRA C++ 2023 notes:
///          - Template queue<T,N>: T must satisfy the C++20
///            @c osal::queue_element concept.
///          - All operations noexcept.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_queue
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>

extern "C"
{
    // NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buf, std::size_t item_sz,
                                   std::size_t cap) noexcept;

    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept;

    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept;

    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept;

    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept;

    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept;

    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item,
                                 osal::tick_t timeout_ticks) noexcept;

    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept;

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_queue OSAL Queue
/// @brief Static-storage, fixed-capacity message queues.
/// @{

/// @brief Fixed-capacity, type-safe OSAL message queue with static storage.
/// @tparam T  Message type. Must satisfy @c osal::queue_element.
/// @tparam N  Maximum number of items (queue depth).
template<queue_element T, queue_depth_t N>
    requires valid_queue_depth<N>
class queue  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
{
public:
    // ---- types -------------------------------------------------------------
    using value_type                        = T;
    static constexpr queue_depth_t capacity = N;

    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the queue.
    /// @complexity O(1)
    /// @blocking   Never.
    queue() noexcept = default;  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

    /// @brief Destructs the queue.
    ~queue() noexcept
    {
        if (valid_)
        {
            (void)osal_queue_destroy(&handle_);
            valid_ = false;
        }
    }

    queue(const queue&)            = delete;
    queue& operator=(const queue&) = delete;
    queue(queue&&)                 = delete;
    queue& operator=(queue&&)      = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the queue was successfully initialised.
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    /// @brief Returns the number of items currently in the queue.
    [[nodiscard]] std::size_t count() const noexcept { return osal_queue_count(&handle_); }
    /// @brief Returns remaining free slots.
    [[nodiscard]] std::size_t free_slots() const noexcept { return osal_queue_free(&handle_); }
    /// @brief Returns true if the queue is empty.
    [[nodiscard]] bool empty() const noexcept { return count() == 0U; }
    /// @brief Returns true if the queue is full.
    [[nodiscard]] bool full() const noexcept { return free_slots() == 0U; }

    // ---- send --------------------------------------------------------------

    /// @brief Sends an item, blocking indefinitely if the queue is full.
    /// @param item  Item to enqueue (copied by value).
    /// @return result::ok() on success.
    /// @complexity O(1)
    /// @blocking   Potentially blocking.
    [[nodiscard]] result send(const T& item) noexcept { return osal_queue_send(&handle_, &item, WAIT_FOREVER); }

    /// @brief Attempts to send without blocking.
    /// @param item  Item to enqueue.
    /// @return true if enqueued; false if the queue was full.
    /// @complexity O(1)
    /// @blocking   Never.
    bool try_send(const T& item) noexcept { return osal_queue_send(&handle_, &item, NO_WAIT).ok(); }

    /// @brief Sends with a timeout.
    /// @param item    Item to enqueue.
    /// @param timeout Maximum wait time.
    /// @return true if enqueued within the timeout.
    /// @complexity O(1)
    /// @blocking   Up to timeout.
    bool send_for(const T& item, milliseconds timeout) noexcept
    {
        if constexpr (timed_queue_backend<active_backend>)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            return osal_queue_send(&handle_, &item, ticks).ok();
        }
        else
        {
            return try_send(item);
        }
    }

    /// @brief Sends from ISR context.
    /// @param item  Item to enqueue.
    /// @return true on success.
    /// @warning Only safe when capabilities<active_backend>::has_isr_queue.
    /// @complexity O(1)
    /// @blocking   Never.
    bool send_isr(const T& item) noexcept { return osal_queue_send_isr(&handle_, &item).ok(); }

    // ---- receive -----------------------------------------------------------

    /// @brief Receives an item, blocking indefinitely if the queue is empty.
    /// @param[out] item  Destination for received item.
    /// @return result::ok() on success.
    /// @complexity O(1)
    /// @blocking   Potentially blocking.
    [[nodiscard]] result receive(T& item) noexcept { return osal_queue_receive(&handle_, &item, WAIT_FOREVER); }

    /// @brief Attempts to receive without blocking.
    /// @param[out] item  Destination.
    /// @return true if an item was available.
    bool try_receive(T& item) noexcept { return osal_queue_receive(&handle_, &item, NO_WAIT).ok(); }

    /// @brief Receives with a timeout.
    /// @param[out] item    Destination.
    /// @param      timeout Maximum wait time.
    bool receive_for(T& item, milliseconds timeout) noexcept
    {
        if constexpr (timed_queue_backend<active_backend>)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            return osal_queue_receive(&handle_, &item, ticks).ok();
        }
        else
        {
            return try_receive(item);
        }
    }

    /// @brief Receives from ISR context.
    /// @param[out] item  Destination.
    /// @return true on success.
    bool receive_isr(T& item) noexcept { return osal_queue_receive_isr(&handle_, &item).ok(); }

    /// @brief Peeks at the front item without removing it.
    /// @param[out] item  Destination.
    /// @return true if an item was available.
    bool peek(T& item) noexcept { return osal_queue_peek(&handle_, &item, NO_WAIT).ok(); }

private:
    active_traits::queue_handle_t handle_{};
    /// @brief Static backing storage — N items of size sizeof(T).
    alignas(T) std::uint8_t storage_[N * sizeof(T)]{};
    bool valid_{osal_queue_create(&handle_, storage_, sizeof(T), N).ok()};
};

/// @} // osal_queue

}  // namespace osal
