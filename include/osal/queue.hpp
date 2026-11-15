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
#include "diagnostics.hpp"
#include "types.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>

extern "C"
{
    /// @brief Creates a backend queue backed by caller-owned storage where required.
    /// @param[out] handle   Receives the native queue handle.
    /// @param[in]  buf      Backing storage; must remain alive until successful destruction.
    /// @param[in]  item_sz  Size of each copied item in bytes.
    /// @param[in]  cap      Maximum number of items.
    /// @pre Storage size and alignment satisfy the backend requirements for item_sz * cap bytes.
    /// @return Success or the backend creation error.
    /// @note Thread context only.
    // NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buf, std::size_t item_sz,
                                   std::size_t cap) noexcept;

    /// @brief Destroys a backend queue after concurrent users have quiesced.
    /// @param[in,out] handle  Queue handle to release.
    /// @return Success or the backend destruction error.
    /// @note Thread context only; backing storage must remain alive if destruction fails.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept;

    /// @brief Copies an item into a backend queue from thread context.
    /// @param[in,out] handle         Initialised queue handle.
    /// @param[in]     item           Source containing one complete item.
    /// @param[in]     timeout_ticks  Wait limit in ticks; NO_WAIT polls and WAIT_FOREVER waits indefinitely.
    /// @return Success or the backend wait/capacity/error result.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept;

    /// @brief Attempts an ISR-context send without waiting.
    /// @param[in,out] handle  Initialised queue handle.
    /// @param[in]     item    Source containing one complete item.
    /// @pre The active backend advertises ISR queue support.
    /// @return Success or the backend capacity/error result.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept;

    /// @brief Copies and removes a queued item from thread context.
    /// @param[in,out] handle         Initialised queue handle.
    /// @param[out]    item           Storage for one complete received item.
    /// @param[in]     timeout_ticks  Wait limit in ticks; NO_WAIT polls and WAIT_FOREVER waits indefinitely.
    /// @return Success or the backend wait/error result.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept;

    /// @brief Attempts an ISR-context receive without waiting.
    /// @param[in,out] handle  Initialised queue handle.
    /// @param[out]    item    Storage for one complete received item.
    /// @pre The active backend advertises ISR queue support.
    /// @return Success or the backend empty/error result.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept;

    /// @brief Copies the front item without removing it.
    /// @param[in,out] handle         Initialised queue handle.
    /// @param[out]    item           Storage for one complete copied item.
    /// @param[in]     timeout_ticks  Backend wait limit in ticks; NO_WAIT polls.
    /// @return Success or the backend wait/capability/error result.
    /// @note Thread context only.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item,
                                 osal::tick_t timeout_ticks) noexcept;

    /// @brief Samples the number of items in a backend queue.
    /// @param[in] handle  Initialised queue handle.
    /// @return Observed occupancy; concurrent operations may immediately change it.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept;

    /// @brief Samples the unused capacity of a backend queue.
    /// @param[in] handle  Initialised queue handle.
    /// @return Observed free slots; concurrent operations may immediately change them.
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
class queue
{
public:
    // ---- types -------------------------------------------------------------
    using value_type                        = T;
    static constexpr queue_depth_t capacity = N;

#if defined(OSAL_BACKEND_CMSIS_RTOS)
    static_assert(sizeof(T) <= sizeof(std::uint32_t),
                  "CMSIS-RTOS v1 queues transport values of at most 32 bits; use a handle or a smaller payload.");
#endif

    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the queue.
    /// @details Check valid() or initialization_status() for creation failure.
    /// @complexity O(1)
    /// @blocking   Never.
    queue() noexcept
        : initialization_status_(osal_queue_create(&handle_, storage_, sizeof(T), N)),
          valid_(initialization_status_.ok())
    {
        auto& counters = diagnostics(diagnostic_resource::queue);
        counters.attempt(initialization_status_);
        operations_.set_capacity(N);
        if (valid_)
        {
            counters.acquire();
        }
    }

    /// @brief Attempts queue cleanup, discarding close() errors.
    /// @pre Concurrent users have quiesced; explicitly close and handle failure before destroying storage.
    ~queue() noexcept { (void)close(); }

    /// @brief Copy construction is disabled.
    queue(const queue&) = delete;
    /// @brief Copy assignment is disabled.
    queue& operator=(const queue&) = delete;
    /// @brief Move construction is disabled because backing storage is inline.
    queue(queue&&) = delete;
    /// @brief Move assignment is disabled.
    queue& operator=(queue&&) = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the queue was successfully initialised.
    /// @return Whether this object currently owns a valid queue.
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    /// @brief Reports the original queue creation result.
    /// @return Success or the creation error, unchanged by close().
    [[nodiscard]] result initialization_status() const noexcept { return initialization_status_; }

    /// @brief Idempotent checked destruction; failure retains ownership for retry.
    /// @details Call after quiescing concurrent users. Destruction is best-effort.
    ///          On failure keep this object (and its inline storage) alive.
    /// @return Success if already closed or destroyed, otherwise the backend destruction error.
    /// @note Thread context only.
    [[nodiscard]] result close() noexcept
    {
        if (!valid_)
        {
            return ok();
        }
        const result status = osal_queue_destroy(&handle_);
        diagnostics(diagnostic_resource::queue).attempt(status);
        if (status.ok())
        {
            valid_ = false;
            operations_.set_backlog(0);
            diagnostics(diagnostic_resource::queue).release();
        }
        return status;
    }

    /// @brief Independently sampled gauges and modulo-2^32 operation counters.
    /// @details ISR operations are intentionally excluded. Backlog is the last
    ///          thread-context observation, not a synchronization predicate.
    ///          Drops count sends rejected by capacity/backpressure, not invalid
    ///          handles. Global in_use/high water measure owned queue handles.
    /// @return Approximate per-object operation counters and backlog gauges.
    [[nodiscard]] diagnostic_snapshot statistics() const noexcept { return operations_.snapshot(); }
    /// @brief Returns the number of items currently in the queue.
    /// @return Sampled occupancy, or zero when invalid; not a synchronization predicate.
    [[nodiscard]] std::size_t count() const noexcept
    {
        const auto count = valid_ ? osal_queue_count(&handle_) : 0U;
        operations_.set_backlog(static_cast<std::uint32_t>(count));
        return count;
    }
    /// @brief Returns remaining free slots.
    /// @return Sampled free capacity, or zero when invalid.
    [[nodiscard]] std::size_t free_slots() const noexcept { return valid_ ? osal_queue_free(&handle_) : 0U; }
    /// @brief Returns true if the queue is empty.
    /// @return Whether count() observes zero items, including an invalid queue.
    [[nodiscard]] bool empty() const noexcept { return count() == 0U; }
    /// @brief Returns true if the queue is full.
    /// @return Whether free_slots() observes zero slots, including an invalid queue.
    [[nodiscard]] bool full() const noexcept { return free_slots() == 0U; }

    // ---- send --------------------------------------------------------------

    /// @brief Sends an item, blocking indefinitely if the queue is full.
    /// @param[in] item  Item to enqueue (copied by value).
    /// @retval osal::ok()  The item was enqueued successfully.
    /// @return Otherwise not_initialized or the backend send error.
    /// @note Thread context only; inline payload storage remains owned by this queue.
    /// @complexity O(1)
    /// @blocking   Potentially blocking.
    [[nodiscard]] result send(const T& item) noexcept { return send_checked(item, WAIT_FOREVER); }

    /// @brief Attempts to send without blocking.
    /// @param[in] item  Item to enqueue.
    /// @return True if enqueued; false on full/invalid queues or another send error.
    /// @note Thread context only.
    /// @complexity O(1)
    /// @blocking   Never.
    [[nodiscard]] bool try_send(const T& item) noexcept { return send_checked(item, NO_WAIT).ok(); }

    /// @brief Sends with a timeout.
    /// @param[in] item     Item to enqueue.
    /// @param[in] timeout  Maximum wait; non-positive values poll.
    /// @return True if enqueued; false on timeout, invalid state, or another send error.
    /// @note Thread context only; unsupported timed queues fall back to a non-blocking attempt.
    /// @complexity O(1)
    /// @blocking   Up to timeout.
    bool send_for(const T& item, milliseconds timeout) noexcept
    {
        if constexpr (timed_queue_backend<active_backend>)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            return send_checked(item, ticks).ok();
        }
        else
        {
            return try_send(item);
        }
    }

    /// @brief Sends from ISR context.
    /// @param[in] item  Item to enqueue.
    /// @return True on success, false on any backend send error.
    /// @warning Only safe when capabilities<active_backend>::has_isr_queue.
    /// @complexity O(1)
    /// @blocking   Never.
    [[nodiscard]] bool send_isr(const T& item) noexcept { return osal_queue_send_isr(&handle_, &item).ok(); }

    // ---- receive -----------------------------------------------------------

    /// @brief Receives an item, blocking indefinitely if the queue is empty.
    /// @param[out] item  Destination for received item.
    /// @retval osal::ok()  An item was received successfully.
    /// @return Otherwise not_initialized or the backend receive error.
    /// @note Thread context only.
    /// @complexity O(1)
    /// @blocking   Potentially blocking.
    [[nodiscard]] result receive(T& item) noexcept { return receive_checked(item, WAIT_FOREVER); }

    /// @brief Attempts to receive without blocking.
    /// @param[out] item  Destination.
    /// @return True if an item was received; false on empty/invalid queues or another receive error.
    /// @note Thread context only.
    [[nodiscard]] bool try_receive(T& item) noexcept { return receive_checked(item, NO_WAIT).ok(); }

    /// @brief Receives with a timeout.
    /// @param[out] item     Storage for the received item.
    /// @param[in]  timeout  Maximum wait; non-positive values poll.
    /// @return True if received; false on timeout, invalid state, or another receive error.
    /// @note Thread context only; unsupported timed queues fall back to a non-blocking attempt.
    bool receive_for(T& item, milliseconds timeout) noexcept
    {
        if constexpr (timed_queue_backend<active_backend>)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            return receive_checked(item, ticks).ok();
        }
        else
        {
            return try_receive(item);
        }
    }

    /// @brief Receives from ISR context.
    /// @param[out] item  Destination.
    /// @return True if an item was received, false on any backend receive error.
    /// @pre The active backend advertises ISR queue support.
    [[nodiscard]] bool receive_isr(T& item) noexcept { return osal_queue_receive_isr(&handle_, &item).ok(); }

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
    /// @brief Receives an item returning std::expected<T, error_code> (C++23).
    /// @details Requires non-throwing default and move construction of T; preserves backend errors.
    /// @return Received value on success, otherwise unexpected containing the receive error.
    /// @note Thread context only; waits indefinitely for an item.
    [[nodiscard]] std::expected<T, error_code> receive_expected() noexcept
        requires(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_move_constructible_v<T>)
    {
        if (!valid())
        {
            (void)observe(error_code::not_initialized, false);
            return std::unexpected(error_code::not_initialized);
        }
        T            item{};
        const result status = receive(item);
        if (status.ok())
        {
            return std::move(item);
        }
        return std::unexpected(status.code());
    }

    /// @brief Non-blocking receive returning std::expected<T, error_code> (C++23).
    /// @details Requires non-throwing default and move construction of T; preserves backend errors.
    /// @return Received value on success, otherwise unexpected containing the receive error.
    /// @note Thread context only; does not wait for an item.
    [[nodiscard]] std::expected<T, error_code> try_receive_expected() noexcept
        requires(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_move_constructible_v<T>)
    {
        if (!valid())
        {
            (void)observe(error_code::not_initialized, false);
            return std::unexpected(error_code::not_initialized);
        }
        T            item{};
        const result status = receive_checked(item, NO_WAIT);
        if (status.ok())
        {
            return std::move(item);
        }
        return std::unexpected(status.code());
    }
#endif

    /// @brief Peeks at the front item without removing it.
    /// @param[out] item  Destination.
    /// @return True if copied; false for an invalid/empty queue or another backend error.
    /// @note Thread context only; backend peek support is required.
    [[nodiscard]] bool peek(T& item) noexcept
    {
        return observe(valid_ ? osal_queue_peek(&handle_, &item, NO_WAIT) : result{error_code::not_initialized}, false)
            .ok();
    }

private:
    result observe(result status, bool sending) noexcept
    {
        operations_.attempt(status);
        auto& counters = diagnostics(diagnostic_resource::queue);
        counters.attempt(status);
        if (status == error_code::timeout || status == error_code::would_block || status == error_code::overflow)
        {
            operations_.contended();
            counters.contended();
            if (sending)
            {
                operations_.dropped();
                counters.dropped();
            }
        }
        (void)count();
        return status;
    }

    result send_checked(const T& item, tick_t ticks) noexcept
    {
        return observe(valid_ ? osal_queue_send(&handle_, &item, ticks) : result{error_code::not_initialized}, true);
    }

    result receive_checked(T& item, tick_t ticks) noexcept
    {
        return observe(valid_ ? osal_queue_receive(&handle_, &item, ticks) : result{error_code::not_initialized},
                       false);
    }

    mutable diagnostic_counters   operations_{};
    active_traits::queue_handle_t handle_{};
    /// @brief Static backing storage — N items of size sizeof(T).
    alignas(T) std::uint8_t storage_[N * sizeof(T)]{};
    result initialization_status_{error_code::not_initialized};
    bool   valid_{};
};

/// @} // osal_queue

}  // namespace osal
