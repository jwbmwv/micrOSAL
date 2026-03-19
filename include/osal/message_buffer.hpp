// SPDX-License-Identifier: Apache-2.0
/// @file message_buffer.hpp
/// @brief OSAL message buffer — length-prefixed, blocking SPSC ring
/// @details Provides osal::message_buffer<N> with FreeRTOS xMessageBuffer
///          semantics:
///
///          • Message-oriented (not byte-oriented): each send writes one
///            complete, indivisible message; each receive reads exactly one
///            message.
///          • Each message is framed internally as:
///            [ osal_mb_length_t length ][ payload bytes ]
///            where sizeof(osal_mb_length_t) == 2 by default.  Override by
///            defining OSAL_MESSAGE_BUFFER_LENGTH_TYPE before including this
///            header.
///          • Blocking: send blocks until the full frame fits; receive blocks
///            until a complete message is available.
///          • ISR-safe: send_isr / receive_isr never block.
///
///          Template parameters
///          ─────────────────────────────────────────────────────────────────
///          @tparam N  Total byte capacity of the ring (including framing
///                     overhead).  The maximum single-message payload is
///                     N - sizeof(osal_mb_length_t) bytes.
///
///          Storage model
///          ─────────────────────────────────────────────────────────────────
///          message_buffer contains a @c std::uint8_t storage_[N+1] member —
///          identical sizing convention as stream_buffer.  No heap allocation.
///
///          MISRA C++ 2023 notes
///          ─────────────────────────────────────────────────────────────────
///          • All operations noexcept.
///          • Template capacity is constrained with a C++20 requires-clause.
///          • No dynamic allocation; storage is inline.
///
/// @par Example
/// @code
/// struct Sensor { float value; uint32_t ts; };
///
/// // 128-byte ring, up to 126-byte payload per message
/// osal::message_buffer<128> mb;
///
/// // Producer:
/// Sensor s{42.0f, 1234};
/// mb.send(&s, sizeof(s));           // blocks if no space
///
/// // Consumer:
/// Sensor rx{};
/// std::size_t n = mb.receive(&rx, sizeof(rx));  // blocks until msg ready
/// @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_message_buffer
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

// ---------------------------------------------------------------------------
// Length-prefix type (matches FreeRTOS configMESSAGE_BUFFER_LENGTH_TYPE)
// ---------------------------------------------------------------------------

#ifndef OSAL_MESSAGE_BUFFER_LENGTH_TYPE
/// @brief Type used to encode the per-message length header.
/// Override to std::uint32_t if any single message can exceed 65535 bytes.
using osal_mb_length_t = std::uint16_t;
#else
using osal_mb_length_t = OSAL_MESSAGE_BUFFER_LENGTH_TYPE;
#endif

/// @brief Byte overhead per message (size of the length prefix).
static constexpr std::size_t kMsgHeaderBytes = sizeof(osal_mb_length_t);

// ---------------------------------------------------------------------------
// C-linkage backend functions (implemented per backend)
// ---------------------------------------------------------------------------
extern "C"
{
    /// @brief Initialise a message buffer.
    /// @param handle    Output handle.
    /// @param buffer    Caller-supplied backing storage (capacity+1 bytes).
    /// @param capacity  Total byte capacity of the ring (N).
    osal::result osal_message_buffer_create(osal::active_traits::message_buffer_handle_t* handle, void* buffer,
                                            std::size_t capacity) noexcept;

    osal::result osal_message_buffer_destroy(osal::active_traits::message_buffer_handle_t* handle) noexcept;

    /// @brief Write one complete message from task context.
    /// @param handle        Message-buffer handle.
    /// @param msg           Source message payload.
    /// @param len           Payload length in bytes.
    /// @param timeout_ticks WAIT_FOREVER / NO_WAIT / tick count.
    osal::result osal_message_buffer_send(osal::active_traits::message_buffer_handle_t* handle, const void* msg,
                                          std::size_t len, osal::tick_t timeout_ticks) noexcept;

    /// @brief Write one message from ISR context (non-blocking).
    osal::result osal_message_buffer_send_isr(osal::active_traits::message_buffer_handle_t* handle, const void* msg,
                                              std::size_t len) noexcept;

    /// @brief Read the next complete message (blocks until one is available).
    /// @param handle  Message-buffer handle.
    /// @param buf     Destination buffer.
    /// @param max_len Destination buffer size.  If the message exceeds max_len
    ///                the excess bytes are silently discarded (consistent with
    ///                FreeRTOS behaviour).
    /// @param timeout_ticks WAIT_FOREVER / NO_WAIT / tick count.
    /// @return Bytes written to @p buf (0 = timeout / empty).
    std::size_t osal_message_buffer_receive(osal::active_traits::message_buffer_handle_t* handle, void* buf,
                                            std::size_t max_len, osal::tick_t timeout_ticks) noexcept;

    /// @brief Read next message from ISR context (non-blocking).
    std::size_t osal_message_buffer_receive_isr(osal::active_traits::message_buffer_handle_t* handle, void* buf,
                                                std::size_t max_len) noexcept;

    /// @brief Payload size of the next complete message (0 if none ready).
    std::size_t osal_message_buffer_available(const osal::active_traits::message_buffer_handle_t* handle) noexcept;

    /// @brief Maximum additional payload that can be enqueued without blocking.
    std::size_t osal_message_buffer_free_space(const osal::active_traits::message_buffer_handle_t* handle) noexcept;

    /// @brief Discard all queued messages.  Call only when no concurrent access.
    osal::result osal_message_buffer_reset(osal::active_traits::message_buffer_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_message_buffer OSAL Message Buffer
/// @brief Length-prefixed, blocking SPSC ring with FreeRTOS semantics.
/// @{

/// @brief Message-oriented blocking SPSC buffer.
///
/// @tparam N  Total ring byte capacity (including the 2-byte per-message
///             header overhead).  Maximum single-message payload is
///             N - sizeof(osal_mb_length_t).
template<std::size_t N>
    requires(N > kMsgHeaderBytes)
class message_buffer
{
public:
    // ---- types -------------------------------------------------------------
    static constexpr std::size_t capacity = N;
    /// @brief Maximum payload size of a single message.
    static constexpr std::size_t max_message_size = N - kMsgHeaderBytes;

    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the message buffer.
    /// @complexity O(1)
    /// @blocking   Never.
    message_buffer() noexcept { valid_ = osal_message_buffer_create(&handle_, storage_, N).ok(); }

    /// @brief Destructs the message buffer and releases OS resources.
    ~message_buffer() noexcept
    {
        if (valid_)
        {
            (void)osal_message_buffer_destroy(&handle_);
            valid_ = false;
        }
    }

    message_buffer(const message_buffer&)            = delete;
    message_buffer& operator=(const message_buffer&) = delete;
    message_buffer(message_buffer&&)                 = delete;
    message_buffer& operator=(message_buffer&&)      = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the buffer was successfully initialised.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    /// @brief Payload size of the next complete message (0 if no message ready).
    [[nodiscard]] std::size_t next_message_size() const noexcept { return osal_message_buffer_available(&handle_); }

    /// @brief Maximum payload that can be sent without blocking.
    [[nodiscard]] std::size_t free_space() const noexcept { return osal_message_buffer_free_space(&handle_); }

    /// @brief Returns true if no complete message is waiting.
    [[nodiscard]] bool empty() const noexcept { return next_message_size() == 0U; }

    // ---- send (producer API) -----------------------------------------------

    /// @brief Blocks until space for the message is available, then writes it.
    /// @param msg  Pointer to the message payload.
    /// @param len  Payload length.  Must be ≤ max_message_size.
    /// @return result::ok() on success.
    /// @complexity O(len)
    /// @blocking   Until space is available.
    [[nodiscard]] result send(const void* msg, std::size_t len) noexcept
    {
        return osal_message_buffer_send(&handle_, msg, len, WAIT_FOREVER);
    }

    /// @brief Span overload for byte-oriented message payloads.
    result send(std::span<const std::byte> msg) noexcept { return send(msg.data(), msg.size()); }

    /// @brief Attempts to write without blocking.
    /// @return true if the message was enqueued.
    /// @complexity O(len)
    /// @blocking   Never.
    bool try_send(const void* msg, std::size_t len) noexcept
    {
        return osal_message_buffer_send(&handle_, msg, len, NO_WAIT).ok();
    }

    /// @brief Span overload for non-blocking payload writes.
    bool try_send(std::span<const std::byte> msg) noexcept { return try_send(msg.data(), msg.size()); }

    /// @brief Writes with a timeout.
    /// @return true if the message was enqueued within @p timeout.
    /// @complexity O(len)
    /// @blocking   Up to @p timeout.
    bool send_for(const void* msg, std::size_t len, milliseconds timeout) noexcept
    {
        if constexpr (active_capabilities::has_timed_semaphore)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            return osal_message_buffer_send(&handle_, msg, len, ticks).ok();
        }
        else
        {
            return try_send(msg, len);
        }
    }

    /// @brief Span overload for timed payload writes.
    bool send_for(std::span<const std::byte> msg, milliseconds timeout) noexcept
    {
        return send_for(msg.data(), msg.size(), timeout);
    }

    /// @brief Writes from ISR context (non-blocking).
    /// @warning ISR-safety depends on has_isr_stream_buffer / has_isr_semaphore.
    /// @return result::ok() on success; error_code::timeout if no space.
    /// @complexity O(len)
    /// @blocking   Never.
    [[nodiscard]] result send_isr(const void* msg, std::size_t len) noexcept
    {
        return osal_message_buffer_send_isr(&handle_, msg, len);
    }

    /// @brief Span overload for ISR-context payload writes.
    result send_isr(std::span<const std::byte> msg) noexcept { return send_isr(msg.data(), msg.size()); }

    // ---- receive (consumer API) -------------------------------------------

    /// @brief Blocks until a complete message is available, then reads it.
    /// @param[out] buf     Destination buffer.
    /// @param      max_len Destination buffer size.
    /// @return  Bytes copied into @p buf.  If the message payload exceeds
    ///          @p max_len the excess bytes are discarded (FreeRTOS behaviour).
    /// @complexity O(len read)
    /// @blocking   Until a complete message is available.
    std::size_t receive(void* buf, std::size_t max_len) noexcept
    {
        return osal_message_buffer_receive(&handle_, buf, max_len, WAIT_FOREVER);
    }

    /// @brief Span overload for message reads.
    std::size_t receive(std::span<std::byte> buf) noexcept { return receive(buf.data(), buf.size()); }

    /// @brief Attempts to read without blocking.
    /// @return  Bytes read, or 0 if no complete message is available.
    /// @complexity O(len read)
    /// @blocking   Never.
    std::size_t try_receive(void* buf, std::size_t max_len) noexcept
    {
        return osal_message_buffer_receive(&handle_, buf, max_len, NO_WAIT);
    }

    /// @brief Span overload for non-blocking message reads.
    std::size_t try_receive(std::span<std::byte> buf) noexcept { return try_receive(buf.data(), buf.size()); }

    /// @brief Reads with a timeout.
    /// @return  Bytes read within @p timeout, or 0 on timeout.
    /// @complexity O(len read)
    /// @blocking   Up to @p timeout.
    std::size_t receive_for(void* buf, std::size_t max_len, milliseconds timeout) noexcept
    {
        if constexpr (active_capabilities::has_timed_semaphore)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            return osal_message_buffer_receive(&handle_, buf, max_len, ticks);
        }
        else
        {
            return try_receive(buf, max_len);
        }
    }

    /// @brief Span overload for timed message reads.
    std::size_t receive_for(std::span<std::byte> buf, milliseconds timeout) noexcept
    {
        return receive_for(buf.data(), buf.size(), timeout);
    }

    /// @brief Reads from ISR context (non-blocking).
    /// @warning ISR-safety depends on has_isr_stream_buffer / has_isr_semaphore.
    /// @return  Bytes read, or 0 if no complete message is available.
    /// @complexity O(len read)
    /// @blocking   Never.
    std::size_t receive_isr(void* buf, std::size_t max_len) noexcept
    {
        return osal_message_buffer_receive_isr(&handle_, buf, max_len);
    }

    /// @brief Span overload for ISR-context message reads.
    std::size_t receive_isr(std::span<std::byte> buf) noexcept { return receive_isr(buf.data(), buf.size()); }

    // ---- control -----------------------------------------------------------

    /// @brief Discards all queued messages.
    /// @warning Must be called only when no concurrent send/receive is in progress.
    [[nodiscard]] result reset() noexcept { return osal_message_buffer_reset(&handle_); }

private:
    bool                                           valid_{false};
    mutable active_traits::message_buffer_handle_t handle_{};
    /// @brief Ring storage: N usable bytes + 1 sentinel byte (SPSC ring marker).
    std::uint8_t storage_[N + 1U]{};
};

/// @} // osal_message_buffer

}  // namespace osal
