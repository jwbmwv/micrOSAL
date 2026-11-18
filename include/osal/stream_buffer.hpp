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
    /// @param[out] handle        Output handle.
    /// @param[in]  buffer        Caller-supplied backing storage (capacity+1 bytes).
    /// @param[in]  capacity      Usable byte capacity.
    /// @param[in]  trigger_level Receive-unblock threshold.
    /// @return The backend result; consult its error code if initialization fails.
    /// @details The storage must remain alive until the stream buffer is destroyed.
    osal::result osal_stream_buffer_create(osal::active_traits::stream_buffer_handle_t* handle, void* buffer,
                                           std::size_t capacity, std::size_t trigger_level) noexcept;

    /// @brief Destroy a stream buffer.
    /// @param[in,out] handle Handle to destroy.
    /// @return The backend result; consult its error code if destruction fails.
    osal::result osal_stream_buffer_destroy(osal::active_traits::stream_buffer_handle_t* handle) noexcept;

    /// @brief Write bytes to the stream buffer from task context.
    /// @param[in,out] handle        Initialised stream-buffer handle.
    /// @param[in]     data          Source bytes.
    /// @param[in]     len           Number of bytes to write.
    /// @param[in]     timeout_ticks Maximum wait in ticks; WAIT_FOREVER waits indefinitely.
    /// @return The backend result; consult its error code if the requested bytes are not sent.
    /// @details The source must remain readable for the duration of the call.
    osal::result osal_stream_buffer_send(osal::active_traits::stream_buffer_handle_t* handle, const void* data,
                                         std::size_t len, osal::tick_t timeout_ticks) noexcept;

    /// @brief Write bytes from ISR context (non-blocking).
    /// @param[in,out] handle Initialised stream-buffer handle.
    /// @param[in]     data   Source bytes.
    /// @param[in]     len    Number of bytes to write.
    /// @return The backend result; consult its error code if the requested bytes are not sent.
    /// @warning ISR use requires backend-advertised ISR-safe stream-buffer/semaphore support.
    osal::result osal_stream_buffer_send_isr(osal::active_traits::stream_buffer_handle_t* handle, const void* data,
                                             std::size_t len) noexcept;

    /// @brief Read up to max_len bytes from the stream buffer.
    /// @param[in,out] handle        Initialised stream-buffer handle.
    /// @param[out]    buf           Destination storage.
    /// @param[in]     max_len       Maximum bytes to read.
    /// @param[in]     timeout_ticks Maximum wait in ticks; WAIT_FOREVER waits indefinitely.
    /// @return Number of bytes actually read (0 = timeout or empty below trigger).
    /// @details The destination must be writable for at least @p max_len bytes.
    std::size_t osal_stream_buffer_receive(osal::active_traits::stream_buffer_handle_t* handle, void* buf,
                                           std::size_t max_len, osal::tick_t timeout_ticks) noexcept;

    /// @brief Read from ISR context (non-blocking).
    /// @param[in,out] handle  Initialised stream-buffer handle.
    /// @param[out]    buf     Destination storage.
    /// @param[in]     max_len Maximum bytes to read.
    /// @return Number of bytes actually read (zero if below trigger).
    /// @warning ISR use requires backend-advertised ISR-safe stream-buffer/semaphore support.
    std::size_t osal_stream_buffer_receive_isr(osal::active_traits::stream_buffer_handle_t* handle, void* buf,
                                               std::size_t max_len) noexcept;

    /// @brief Bytes available to read.
    /// @param[in] handle Initialised stream-buffer handle.
    /// @return Snapshot of readable bytes.
    std::size_t osal_stream_buffer_available(const osal::active_traits::stream_buffer_handle_t* handle) noexcept;

    /// @brief Free bytes (can be written before blocking).
    /// @param[in] handle Initialised stream-buffer handle.
    /// @return Snapshot of writable bytes.
    std::size_t osal_stream_buffer_free_space(const osal::active_traits::stream_buffer_handle_t* handle) noexcept;

    /// @brief Flush all pending data. Call only when no concurrent access.
    /// @param[in,out] handle Initialised stream-buffer handle.
    /// @return The backend result; consult its error code if reset fails.
    osal::result osal_stream_buffer_reset(osal::active_traits::stream_buffer_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_stream_buffer OSAL Stream Buffer
/// @brief Byte-oriented, blocking SPSC ring with FreeRTOS semantics.
/// @{

/// @brief Byte-oriented blocking SPSC stream buffer.
///
/// @tparam N             Byte capacity. Actual internal storage is N+1 bytes.
/// @tparam TriggerLevel  Minimum readable bytes before a blocking receive unblocks.
template<std::size_t N, std::size_t TriggerLevel = 1U>
    requires valid_trigger_level<N, TriggerLevel>
class stream_buffer  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
{
public:
    // ---- types -------------------------------------------------------------
    /// @brief Usable byte capacity of the ring.
    static constexpr std::size_t capacity = N;
    /// @brief Minimum readable byte count that releases a blocking receive.
    static constexpr std::size_t trigger_level = TriggerLevel;

    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the stream buffer.
    /// @complexity O(1)
    /// @blocking   Never.
    /// @details Storage is inline; check valid() before use.
    stream_buffer() noexcept = default;  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

    /// @brief Destroys the stream buffer and releases backend resources.
    /// @details No concurrent access may remain.
    ~stream_buffer() noexcept
    {
        if (valid_)
        {
            (void)osal_stream_buffer_destroy(&handle_);
            valid_ = false;
        }
    }

    /// @brief Copy construction is disabled; ring storage and backend handle are unique.
    stream_buffer(const stream_buffer&) = delete;
    /// @brief Copy assignment is disabled; ring storage and backend handle are unique.
    stream_buffer& operator=(const stream_buffer&) = delete;
    /// @brief Move construction is disabled; backend state is not transferable.
    stream_buffer(stream_buffer&&) = delete;
    /// @brief Move assignment is disabled; backend state is not transferable.
    stream_buffer& operator=(stream_buffer&&) = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the stream buffer was successfully initialised.
    /// @return true if backend initialization succeeded.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    /// @brief Bytes currently available to read.
    /// @return Snapshot of currently readable bytes.
    [[nodiscard]] std::size_t available() const noexcept { return osal_stream_buffer_available(&handle_); }

    /// @brief Free bytes (can be written without blocking).
    /// @return Snapshot of currently writable bytes.
    [[nodiscard]] std::size_t free_space() const noexcept { return osal_stream_buffer_free_space(&handle_); }

    /// @brief Returns true if the ring is empty.
    /// @return true if available() currently reports zero bytes.
    [[nodiscard]] bool empty() const noexcept { return available() == 0U; }

    /// @brief Returns true if the ring is full.
    /// @return true if free_space() currently reports zero bytes.
    [[nodiscard]] bool full() const noexcept { return free_space() == 0U; }

    // ---- send (producer API) -----------------------------------------------

    /// @brief Blocks until @p len bytes of space are available, then writes.
    /// @param[in] data Source buffer.
    /// @param[in] len  Bytes to write.  Must be ≤ N.
    /// @retval osal::ok() The bytes were written successfully.
    /// @note This overload blocks indefinitely and does not return error_code::timeout.
    /// @par Other errors
    /// Backend errors are returned unchanged.
    /// @details Thread context only; @p data must remain readable for the call.
    /// @complexity O(len)
    /// @blocking   Until @p len bytes of space are available.
    [[nodiscard]] result send(const void* data, std::size_t len) noexcept
    {
        return osal_stream_buffer_send(&handle_, data, len, WAIT_FOREVER);
    }

    /// @brief Send all bytes from a span, blocking for space.
    /// @param[in] data Source bytes.
    /// @return Same backend result as send(const void*, std::size_t).
    result send(std::span<const std::byte> data) noexcept { return send(data.data(), data.size()); }

    /// @brief Attempts to write without blocking.
    /// @param[in] data Source bytes.
    /// @param[in] len  Number of bytes to write.
    /// @return true if all @p len bytes were written.
    /// @details Thread context only.
    /// @complexity O(len)
    /// @blocking   Never.
    bool try_send(const void* data, std::size_t len) noexcept
    {
        return osal_stream_buffer_send(&handle_, data, len, NO_WAIT).ok();
    }

    /// @brief Attempt a non-blocking send from a byte span.
    /// @param[in] data Source bytes.
    /// @return true if all bytes were sent.
    bool try_send(std::span<const std::byte> data) noexcept { return try_send(data.data(), data.size()); }

    /// @brief Writes with a timeout.
    /// @param[in] data    Source byte sequence.
    /// @param[in] len     Number of bytes to write.
    /// @param[in] timeout Maximum time to wait for space.
    /// @return true if all @p len bytes were written within the timeout.
    /// @details On backends without timed semaphore support, falls back to a
    ///          non-blocking attempt.
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

    /// @brief Send all bytes from a span with a timeout.
    /// @param[in] data    Source bytes.
    /// @param[in] timeout Maximum wait.
    /// @return Same result as the pointer-based send_for overload.
    bool send_for(std::span<const std::byte> data, milliseconds timeout) noexcept
    {
        return send_for(data.data(), data.size(), timeout);
    }

    /// @brief Writes from ISR context (non-blocking).
    /// @warning ISR-safety depends on has_isr_stream_buffer / has_isr_semaphore.
    /// @param[in] data Source bytes.
    /// @param[in] len  Number of bytes to write.
    /// @retval osal::ok() Requested bytes were written.
    /// @par Other errors
    /// Backend errors are returned unchanged.
    /// @complexity O(len)
    /// @blocking   Never.
    [[nodiscard]] result send_isr(const void* data, std::size_t len) noexcept
    {
        return osal_stream_buffer_send_isr(&handle_, data, len);
    }

    /// @brief Send all bytes from a span in ISR context.
    /// @param[in] data Source bytes.
    /// @return Same result as the pointer-based send_isr overload.
    /// @warning The same ISR capability requirements as send_isr apply.
    result send_isr(std::span<const std::byte> data) noexcept { return send_isr(data.data(), data.size()); }

    // ---- receive (consumer API) --------------------------------------------

    /// @brief Blocks until TriggerLevel bytes are available, then reads up to
    ///        @p max_len bytes.
    /// @param[out] buf     Destination buffer.
    /// @param[in]  max_len Maximum bytes to read.
    /// @return  Number of bytes placed in @p buf (always ≥ TriggerLevel unless
    ///          the buffer is reset concurrently).
    /// @details Thread context only; destination storage must remain writable
    ///          for the duration of the call.
    /// @complexity O(n read)
    /// @blocking   Until TriggerLevel bytes are available.
    std::size_t receive(void* buf, std::size_t max_len) noexcept
    {
        return osal_stream_buffer_receive(&handle_, buf, max_len, WAIT_FOREVER);
    }

    /// @brief Receive bytes into a span, blocking until the trigger level is met.
    /// @param[out] buf Destination storage.
    /// @return Number of bytes read.
    std::size_t receive(std::span<std::byte> buf) noexcept { return receive(buf.data(), buf.size()); }

    /// @brief Attempts to read without blocking.
    /// @param[out] buf     Destination storage.
    /// @param[in]  max_len Maximum bytes to read.
    /// @return  Bytes read, or 0 if fewer than TriggerLevel bytes are available.
    /// @complexity O(n read)
    /// @blocking   Never.
    std::size_t try_receive(void* buf, std::size_t max_len) noexcept
    {
        return osal_stream_buffer_receive(&handle_, buf, max_len, NO_WAIT);
    }

    /// @brief Attempt a non-blocking receive into a span.
    /// @param[out] buf Destination storage.
    /// @return Bytes read, or zero below the trigger level.
    std::size_t try_receive(std::span<std::byte> buf) noexcept { return try_receive(buf.data(), buf.size()); }

    /// @brief Reads with a timeout.
    /// @param[out] buf     Destination storage.
    /// @param[in]  max_len Maximum bytes to read.
    /// @param[in]  timeout Maximum wait; a non-positive duration does not wait.
    /// @return  Bytes read within @p timeout, or 0 on timeout.
    /// @details On backends without timed semaphore support, falls back to a
    ///          non-blocking receive.
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

    /// @brief Receive into a span with a timeout.
    /// @param[out] buf     Destination storage.
    /// @param[in]  timeout Maximum wait.
    /// @return Number of bytes read, or zero if none are available.
    std::size_t receive_for(std::span<std::byte> buf, milliseconds timeout) noexcept
    {
        return receive_for(buf.data(), buf.size(), timeout);
    }

    /// @brief Reads from ISR context (non-blocking).
    /// @warning ISR-safety depends on has_isr_stream_buffer / has_isr_semaphore.
    /// @param[out] buf     Destination storage.
    /// @param[in]  max_len Maximum bytes to read.
    /// @return  Bytes read, or 0 if fewer than TriggerLevel bytes are available.
    /// @complexity O(n read)
    /// @blocking   Never.
    std::size_t receive_isr(void* buf, std::size_t max_len) noexcept
    {
        return osal_stream_buffer_receive_isr(&handle_, buf, max_len);
    }

    /// @brief Receive bytes into a span from ISR context.
    /// @param[out] buf Destination storage.
    /// @return Bytes read, or zero below the trigger level.
    /// @warning The same ISR capability requirements as receive_isr apply.
    std::size_t receive_isr(std::span<std::byte> buf) noexcept { return receive_isr(buf.data(), buf.size()); }

    // ---- control -----------------------------------------------------------

    /// @brief Discards all buffered data.
    /// @warning Must be called only when no concurrent send/receive is in progress.
    /// @return Success or the backend reset error.
    result reset() noexcept { return osal_stream_buffer_reset(&handle_); }

private:
    mutable active_traits::stream_buffer_handle_t handle_{};
    /// @brief Ring storage: N usable bytes + 1 sentinel byte (SPSC ring marker).
    std::uint8_t storage_[N + 1U]{};
    bool         valid_{osal_stream_buffer_create(&handle_, storage_, N, TriggerLevel).ok()};
};

/// @} // osal_stream_buffer

}  // namespace osal
