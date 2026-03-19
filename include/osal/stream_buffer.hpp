// SPDX-License-Identifier: Apache-2.0
/// @file stream_buffer.hpp
/// @brief OSAL stream buffer — byte-oriented, blocking SPSC ring
/// @details Provides osal::stream_buffer<N, TriggerLevel> with FreeRTOS
///          xStreamBuffer semantics:
///
///          • Byte-oriented (not message-oriented): any number of bytes can be
///            written and read per call, up to the ring capacity.
///          • Blocking: send blocks until space is available; receive blocks
///            until TriggerLevel or more bytes are readable.
///          • ISR-safe: send_isr / receive_isr are non-blocking; on backends
///            with has_isr_stream_buffer == true they are genuinely interrupt-
///            safe (FreeRTOS xStreamBufferSendFromISR etc.); on others the
///            underlying semaphore give is used and ISR-safety depends on the
///            backend's has_isr_semaphore capability.
///
///          Template parameters
///          ─────────────────────────────────────────────────────────────────
///          @tparam N             Byte capacity (usable bytes).
///          @tparam TriggerLevel  Minimum bytes that must be available before
///                                receive() unblocks.  Default is 1 (wake on
///                                any data, FreeRTOS default).  Must be in
///                                [1, N].
///
///          Storage model
///          ─────────────────────────────────────────────────────────────────
///          stream_buffer contains a @c std::uint8_t storage_[N+1] member
///          (one sentinel byte for the SPSC ring's full/empty distinction).
///          No heap allocation is ever performed.  On native-backend paths
///          (FreeRTOS), the storage array is passed directly to the RTOS
///          static allocation API.
///
///          MISRA C++ 2023 notes
///          ─────────────────────────────────────────────────────────────────
///          • Template parameters N and TriggerLevel are constrained with
///            C++20 requires-clauses.
///          • All operations noexcept.
///          • No dynamic allocation; storage is inline in the object.
///
/// @par Example
/// @code
/// osal::stream_buffer<128> sb;   // 128-byte ring, trigger=1
///
/// // Task A (producer):
/// const char msg[] = "hello";
/// sb.send(msg, sizeof(msg));     // blocks if ring is full
///
/// // Task B (consumer):
/// char rx[64];
/// std::size_t n = sb.receive(rx, sizeof(rx));  // blocks until ≥1 byte
/// @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_stream_buffer
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

// ---------------------------------------------------------------------------
// C-linkage backend functions (implemented per backend)
// ---------------------------------------------------------------------------
extern "C"
{
    /// @brief Initialise a stream buffer.
    /// @param handle        Output handle.
    /// @param buffer        Caller-supplied backing storage (must be capacity+1 bytes).
    /// @param capacity      Usable byte capacity (N).
    /// @param trigger_level Receive unblock threshold (1 = wake on any byte).
    osal::result osal_stream_buffer_create(osal::active_traits::stream_buffer_handle_t* handle, void* buffer,
                                           std::size_t capacity, std::size_t trigger_level) noexcept;

    osal::result osal_stream_buffer_destroy(osal::active_traits::stream_buffer_handle_t* handle) noexcept;

    /// @brief Write bytes to the stream buffer from task context.
    /// @param handle        Stream-buffer handle.
    /// @param data          Source data.
    /// @param len           Number of bytes to write.  Must be ≤ capacity.
    /// @param timeout_ticks WAIT_FOREVER / NO_WAIT / tick count.
    osal::result osal_stream_buffer_send(osal::active_traits::stream_buffer_handle_t* handle, const void* data,
                                         std::size_t len, osal::tick_t timeout_ticks) noexcept;

    /// @brief Write bytes from ISR context (non-blocking).
    osal::result osal_stream_buffer_send_isr(osal::active_traits::stream_buffer_handle_t* handle, const void* data,
                                             std::size_t len) noexcept;

    /// @brief Read up to max_len bytes from the stream buffer.
    /// @return Number of bytes actually read (0 = timeout or empty below trigger).
    std::size_t osal_stream_buffer_receive(osal::active_traits::stream_buffer_handle_t* handle, void* buf,
                                           std::size_t max_len, osal::tick_t timeout_ticks) noexcept;

    /// @brief Read from ISR context (non-blocking).
    std::size_t osal_stream_buffer_receive_isr(osal::active_traits::stream_buffer_handle_t* handle, void* buf,
                                               std::size_t max_len) noexcept;

    /// @brief Bytes available to read.
    std::size_t osal_stream_buffer_available(const osal::active_traits::stream_buffer_handle_t* handle) noexcept;

    /// @brief Free bytes (can be written before blocking).
    std::size_t osal_stream_buffer_free_space(const osal::active_traits::stream_buffer_handle_t* handle) noexcept;

    /// @brief Flush all pending data.  Call only when no concurrent access.
    osal::result osal_stream_buffer_reset(osal::active_traits::stream_buffer_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_stream_buffer OSAL Stream Buffer
/// @brief Byte-oriented, blocking SPSC ring with FreeRTOS semantics.
/// @{

/// @brief Byte-oriented blocking SPSC stream buffer.
///
/// @tparam N             Byte capacity.  Actual internal storage is N+1 bytes.
/// @tparam TriggerLevel  Minimum bytes available before receive() unblocks.
template<std::size_t N, std::size_t TriggerLevel = 1U>
    requires valid_trigger_level<N, TriggerLevel>
class stream_buffer
{
public:
    // ---- types -------------------------------------------------------------
    static constexpr std::size_t capacity      = N;
    static constexpr std::size_t trigger_level = TriggerLevel;

    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the stream buffer.
    /// @complexity O(1)
    /// @blocking   Never.
    stream_buffer() noexcept { valid_ = osal_stream_buffer_create(&handle_, storage_, N, TriggerLevel).ok(); }

    /// @brief Destructs the stream buffer and releases OS resources.
    ~stream_buffer() noexcept
    {
        if (valid_)
        {
            (void)osal_stream_buffer_destroy(&handle_);
            valid_ = false;
        }
    }

    stream_buffer(const stream_buffer&)            = delete;
    stream_buffer& operator=(const stream_buffer&) = delete;
    stream_buffer(stream_buffer&&)                 = delete;
    stream_buffer& operator=(stream_buffer&&)      = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the stream buffer was successfully initialised.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    /// @brief Bytes currently available to read.
    [[nodiscard]] std::size_t available() const noexcept { return osal_stream_buffer_available(&handle_); }

    /// @brief Free bytes (can be written without blocking).
    [[nodiscard]] std::size_t free_space() const noexcept { return osal_stream_buffer_free_space(&handle_); }

    /// @brief Returns true if the ring is empty.
    [[nodiscard]] bool empty() const noexcept { return available() == 0U; }

    /// @brief Returns true if the ring is full.
    [[nodiscard]] bool full() const noexcept { return free_space() == 0U; }

    // ---- send (producer API) -----------------------------------------------

    /// @brief Blocks until @p len bytes of space are available, then writes.
    /// @param data  Source buffer.
    /// @param len   Bytes to write.  Must be ≤ N.
    /// @return result::ok() on success; error_code::timeout is never returned
    ///         from this overload (blocks indefinitely).
    /// @complexity O(len)
    /// @blocking   Until @p len bytes of space are available.
    [[nodiscard]] result send(const void* data, std::size_t len) noexcept
    {
        return osal_stream_buffer_send(&handle_, data, len, WAIT_FOREVER);
    }

    /// @brief Span overload for byte-oriented writes.
    result send(std::span<const std::byte> data) noexcept { return send(data.data(), data.size()); }

    /// @brief Attempts to write without blocking.
    /// @return true if all @p len bytes were written.
    /// @complexity O(len)
    /// @blocking   Never.
    bool try_send(const void* data, std::size_t len) noexcept
    {
        return osal_stream_buffer_send(&handle_, data, len, NO_WAIT).ok();
    }

    /// @brief Span overload for non-blocking byte-oriented writes.
    bool try_send(std::span<const std::byte> data) noexcept { return try_send(data.data(), data.size()); }

    /// @brief Writes with a timeout.
    /// @param data Source byte sequence.
    /// @param len Number of bytes to write.
    /// @param timeout Maximum time to wait for space.
    /// @return true if all @p len bytes were written within the timeout.
    /// @complexity O(len)
    /// @blocking   Up to @p timeout.
    bool send_for(const void* data, std::size_t len, milliseconds timeout) noexcept
    {
        if constexpr (active_capabilities::has_timed_semaphore)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            return osal_stream_buffer_send(&handle_, data, len, ticks).ok();
        }
        else
        {
            return try_send(data, len);
        }
    }

    /// @brief Span overload for timed byte-oriented writes.
    bool send_for(std::span<const std::byte> data, milliseconds timeout) noexcept
    {
        return send_for(data.data(), data.size(), timeout);
    }

    /// @brief Writes from ISR context (non-blocking).
    /// @warning ISR-safety depends on has_isr_stream_buffer / has_isr_semaphore.
    /// @complexity O(len)
    /// @blocking   Never.
    [[nodiscard]] result send_isr(const void* data, std::size_t len) noexcept
    {
        return osal_stream_buffer_send_isr(&handle_, data, len);
    }

    /// @brief Span overload for ISR-context byte-oriented writes.
    result send_isr(std::span<const std::byte> data) noexcept { return send_isr(data.data(), data.size()); }

    // ---- receive (consumer API) --------------------------------------------

    /// @brief Blocks until TriggerLevel bytes are available, then reads up to
    ///        @p max_len bytes.
    /// @param[out] buf      Destination buffer.
    /// @param      max_len  Maximum bytes to read.
    /// @return  Number of bytes placed in @p buf (always ≥ TriggerLevel unless
    ///          the buffer is reset concurrently).
    /// @complexity O(n read)
    /// @blocking   Until TriggerLevel bytes are available.
    std::size_t receive(void* buf, std::size_t max_len) noexcept
    {
        return osal_stream_buffer_receive(&handle_, buf, max_len, WAIT_FOREVER);
    }

    /// @brief Span overload for byte-oriented reads.
    std::size_t receive(std::span<std::byte> buf) noexcept { return receive(buf.data(), buf.size()); }

    /// @brief Attempts to read without blocking.
    /// @return  Bytes read, or 0 if fewer than TriggerLevel bytes are available.
    /// @complexity O(n read)
    /// @blocking   Never.
    std::size_t try_receive(void* buf, std::size_t max_len) noexcept
    {
        return osal_stream_buffer_receive(&handle_, buf, max_len, NO_WAIT);
    }

    /// @brief Span overload for non-blocking byte-oriented reads.
    std::size_t try_receive(std::span<std::byte> buf) noexcept { return try_receive(buf.data(), buf.size()); }

    /// @brief Reads with a timeout.
    /// @return  Bytes read within @p timeout, or 0 on timeout.
    /// @complexity O(n read)
    /// @blocking   Up to @p timeout.
    std::size_t receive_for(void* buf, std::size_t max_len, milliseconds timeout) noexcept
    {
        if constexpr (active_capabilities::has_timed_semaphore)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            return osal_stream_buffer_receive(&handle_, buf, max_len, ticks);
        }
        else
        {
            return try_receive(buf, max_len);
        }
    }

    /// @brief Span overload for timed byte-oriented reads.
    std::size_t receive_for(std::span<std::byte> buf, milliseconds timeout) noexcept
    {
        return receive_for(buf.data(), buf.size(), timeout);
    }

    /// @brief Reads from ISR context (non-blocking).
    /// @warning ISR-safety depends on has_isr_stream_buffer / has_isr_semaphore.
    /// @return  Bytes read, or 0 if fewer than TriggerLevel bytes are available.
    /// @complexity O(n read)
    /// @blocking   Never.
    std::size_t receive_isr(void* buf, std::size_t max_len) noexcept
    {
        return osal_stream_buffer_receive_isr(&handle_, buf, max_len);
    }

    /// @brief Span overload for ISR-context byte-oriented reads.
    std::size_t receive_isr(std::span<std::byte> buf) noexcept { return receive_isr(buf.data(), buf.size()); }

    // ---- control -----------------------------------------------------------

    /// @brief Discards all buffered data.
    /// @warning Must be called only when no concurrent send/receive is in progress.
    result reset() noexcept { return osal_stream_buffer_reset(&handle_); }

private:
    bool                                          valid_{false};
    mutable active_traits::stream_buffer_handle_t handle_{};
    /// @brief Ring storage: N usable bytes + 1 sentinel byte (SPSC ring marker).
    std::uint8_t storage_[N + 1U]{};
};

/// @} // osal_stream_buffer

}  // namespace osal
