// SPDX-License-Identifier: Apache-2.0
/// @file emulated_message_buffer.inl
/// @brief Portable emulated message-buffer implementation.
/// @details Provides FreeRTOS-compatible message-buffer semantics on top of
///          the same SPSC lock-free ring as the stream-buffer, with each
///          message framed as a 16-bit length prefix followed by the payload.
///
///          Protocol on the wire
///          ──────────────────────────────────────────────────────────────────
///          [ uint16_t length (LE) ][ payload bytes ... ]
///
///          Atomicity guarantee (SPSC)
///          ──────────────────────────────────────────────────────────────────
///          The producer writes the full frame (header + payload) before
///          storing the updated head_ index; the consumer will never see a
///          partial frame because it first checks available >=
///          sizeof(header) + header.length.
///          A partial frame CAN appear transiently (between the header write
///          and the payload write's head_ store), but the consumer handles
///          this correctly by re-checking and blocking when the incomplete
///          frame is detected.
///
///          Blocking semaphores (binary, max_count = 1):
///          • data_sem : given per complete message sent.  Binary is sufficient
///            because the consumer drains ALL complete messages in a tight loop
///            before blocking again.
///          • space_sem: given per complete message received.  Binary is
///            sufficient for the same reason.
///
///          Note on naming: this file uses the prefix "emu_mb_" for its ring
///          helpers and pool objects so that both this file and
///          emulated_stream_buffer.inl can be included into the same
///          translation unit without name collisions in the anonymous
///          namespace.
///
///          Prerequisites at the point of inclusion:
///          • <osal/osal.hpp> already included with the correct OSAL_BACKEND_*
///          • osal_semaphore_create/destroy/give/take/give_isr already defined
///          • osal_clock_ticks already defined
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#pragma once

// These headers define C++ templates and must never be included inside an
// extern "C" { } block.  If this .inl file is included from within such a
// block (as all backends do), temporarily close it, pull in the headers,
// then re-open it so the function definitions that follow keep C linkage.
#ifdef __cplusplus
}  // temporarily close extern "C" (opened by the including backend)
#endif
#include <osal/detail/atomic_compat.hpp>
#include <algorithm>
#include <cstring>
#ifdef __cplusplus
extern "C"
{  // re-open for the function definitions below
#endif

    // ---------------------------------------------------------------------------
    // Pool sizing tuning macro
    // ---------------------------------------------------------------------------

#ifndef OSAL_EMULATED_MESSAGE_BUFFER_POOL_SIZE
/// @brief Maximum number of emulated message buffers that can exist concurrently.
#define OSAL_EMULATED_MESSAGE_BUFFER_POOL_SIZE 8U
#endif

    // osal_mb_length_t and kMsgHeaderBytes are defined in message_buffer.hpp,
    // which is included before this .inl via osal.hpp.

    namespace  // anonymous — internal to the including TU
    {

    // ---------------------------------------------------------------------------
    // Internal control object
    // ---------------------------------------------------------------------------

    struct emu_mb_obj
    {
        std::uint8_t* buf;        ///< Caller-supplied storage (capacity+1 bytes).
        std::size_t   ring_size;  ///< = capacity + 1 (one sentinel slot).
        std::size_t   capacity;   ///< = N (usable bytes including framing overhead).

        /// @brief Write index — producer-owned.  Range [0, ring_size).
        std::atomic<std::size_t> head;
        /// @brief Read index — consumer-owned.  Range [0, ring_size).
        std::atomic<std::size_t> tail;

        /// @brief Binary semaphore (max=1): given by producer per complete message.
        osal::active_traits::semaphore_handle_t data_sem;
        /// @brief Binary semaphore (max=1): given by consumer per complete message.
        osal::active_traits::semaphore_handle_t space_sem;
    };

    static emu_mb_obj emu_mb_pool[OSAL_EMULATED_MESSAGE_BUFFER_POOL_SIZE];
    static bool       emu_mb_used[OSAL_EMULATED_MESSAGE_BUFFER_POOL_SIZE];

    static emu_mb_obj* emu_mb_acquire() noexcept
    {
        for (std::size_t i = 0U; i < OSAL_EMULATED_MESSAGE_BUFFER_POOL_SIZE; ++i)
        {
            if (!emu_mb_used[i])
            {
                emu_mb_used[i] = true;
                return &emu_mb_pool[i];
            }
        }
        return nullptr;
    }

    static void emu_mb_release_slot(emu_mb_obj* p) noexcept
    {
        for (std::size_t i = 0U; i < OSAL_EMULATED_MESSAGE_BUFFER_POOL_SIZE; ++i)
        {
            if (&emu_mb_pool[i] == p)
            {
                emu_mb_used[i] = false;
                return;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // SPSC ring helpers — "emu_mb_" prefix to avoid collision with stream-buffer
    // ---------------------------------------------------------------------------

    static std::size_t emu_mb_available(const emu_mb_obj* s) noexcept
    {
        const std::size_t h = s->head.load(std::memory_order_acquire);
        const std::size_t t = s->tail.load(std::memory_order_acquire);
        return (h >= t) ? (h - t) : (s->ring_size - t + h);
    }

    static std::size_t emu_mb_free(const emu_mb_obj* s) noexcept
    {
        return s->capacity - emu_mb_available(s);
    }

    static void emu_mb_write(emu_mb_obj* s, const std::uint8_t* data, std::size_t len) noexcept
    {
        std::size_t h = s->head.load(std::memory_order_relaxed);
        for (std::size_t i = 0U; i < len; ++i)
        {
            s->buf[h] = data[i];
            if (++h == s->ring_size)
            {
                h = 0U;
            }
        }
        s->head.store(h, std::memory_order_release);
    }

    static void emu_mb_read(emu_mb_obj* s, std::uint8_t* buf, std::size_t len) noexcept
    {
        std::size_t t = s->tail.load(std::memory_order_relaxed);
        for (std::size_t i = 0U; i < len; ++i)
        {
            buf[i] = s->buf[t];
            if (++t == s->ring_size)
            {
                t = 0U;
            }
        }
        s->tail.store(t, std::memory_order_release);
    }

    static void emu_mb_skip(emu_mb_obj* s, std::size_t len) noexcept
    {
        std::size_t t = s->tail.load(std::memory_order_relaxed);
        for (std::size_t i = 0U; i < len; ++i)
        {
            if (++t == s->ring_size)
            {
                t = 0U;
            }
        }
        s->tail.store(t, std::memory_order_release);
    }

    static void emu_mb_peek(const emu_mb_obj* s, std::uint8_t* buf, std::size_t len) noexcept
    {
        std::size_t t = s->tail.load(std::memory_order_relaxed);
        for (std::size_t i = 0U; i < len; ++i)
        {
            buf[i] = s->buf[t];
            if (++t == s->ring_size)
            {
                t = 0U;
            }
        }
    }

    }  // anonymous namespace

    // ---------------------------------------------------------------------------
    // C-linkage message buffer functions
    // ---------------------------------------------------------------------------

    /// @brief Create an emulated message buffer backed by caller-supplied storage.
    /// @details Uses the same lock-free SPSC ring as the stream buffer, but frames
    ///          each message as a 16-bit length prefix followed by the payload.
    ///          The caller must supply @p capacity + 1 bytes of storage.
    /// @param handle   Output handle; populated on success.
    /// @param buffer   Caller-supplied storage; must be at least @p capacity + 1 bytes.
    /// @param capacity Total ring capacity in bytes (includes framing overhead);
    ///                 must be > `kMsgHeaderBytes` (2 bytes).
    /// @return `osal::ok()` on success, `error_code::invalid_argument` for bad parameters,
    ///         `error_code::out_of_resources` if the pool or semaphores are exhausted.
    osal::result osal_message_buffer_create(osal::active_traits::message_buffer_handle_t* handle, void* buffer,
                                            std::size_t capacity) noexcept
    {
        if (!handle || !buffer || capacity <= kMsgHeaderBytes)
        {
            return osal::error_code::invalid_argument;
        }

        emu_mb_obj* s = emu_mb_acquire();
        if (!s)
        {
            return osal::error_code::out_of_resources;
        }

        s->buf       = static_cast<std::uint8_t*>(buffer);
        s->ring_size = capacity + 1U;
        s->capacity  = capacity;
        s->head.store(0U, std::memory_order_relaxed);
        s->tail.store(0U, std::memory_order_relaxed);

        const bool d_ok = osal_semaphore_create(&s->data_sem, 0U, 1U).ok();
        const bool p_ok = osal_semaphore_create(&s->space_sem, 0U, 1U).ok();
        if (!d_ok || !p_ok)
        {
            if (d_ok)
            {
                (void)osal_semaphore_destroy(&s->data_sem);
            }
            emu_mb_release_slot(s);
            return osal::error_code::out_of_resources;
        }

        handle->native = s;
        return osal::ok();
    }

    /// @brief Destroy an emulated message buffer and release its pool slot.
    /// @param handle Handle to destroy; silently ignored if null or uninitialized.
    /// @return `osal::ok()` on success, `error_code::not_initialized` if null.
    osal::result osal_message_buffer_destroy(osal::active_traits::message_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<emu_mb_obj*>(handle->native);
        (void)osal_semaphore_destroy(&s->data_sem);
        (void)osal_semaphore_destroy(&s->space_sem);
        emu_mb_release_slot(s);
        handle->native = nullptr;
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Send (producer)
    // ---------------------------------------------------------------------------

    /// @brief Send a message into the buffer, blocking until space is available.
    /// @details Writes a 16-bit length header followed by @p len payload bytes as a
    ///          single atomic frame.  The consumer will never see a partial message.
    /// @param handle        Message buffer handle.
    /// @param msg           Message data to write.
    /// @param len           Length of @p msg in bytes; must fit in `osal_mb_length_t`.
    /// @param timeout_ticks Maximum ticks to wait for space; use `osal::WAIT_FOREVER` for indefinite.
    /// @return `osal::ok()` on success, `error_code::timeout` on expiry,
    ///         `error_code::invalid_argument` for oversized messages or null parameters,
    ///         `error_code::not_initialized` if @p handle is null.
    osal::result osal_message_buffer_send(osal::active_traits::message_buffer_handle_t* handle, const void* msg,
                                          std::size_t len, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        if (!msg || len == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        if (len > static_cast<std::size_t>(~osal_mb_length_t(0)))
        {
            return osal::error_code::invalid_argument;
        }
        auto* s = static_cast<emu_mb_obj*>(handle->native);

        const std::size_t frame_size = kMsgHeaderBytes + len;
        if (frame_size > s->capacity)
        {
            return osal::error_code::invalid_argument;
        }

        const osal::tick_t start = osal_clock_ticks();

        while (true)
        {
            if (emu_mb_free(s) >= frame_size)
            {
                // Write header then payload.  head_ is updated ONCE per call to
                // emu_mb_write so a partially-written frame is never visible as
                // "complete" to the consumer.
                const osal_mb_length_t hdr = static_cast<osal_mb_length_t>(len);
                emu_mb_write(s, reinterpret_cast<const std::uint8_t*>(&hdr), kMsgHeaderBytes);
                emu_mb_write(s, static_cast<const std::uint8_t*>(msg), len);
                (void)osal_semaphore_give(&s->data_sem);  // notify consumer
                return osal::ok();
            }

            if (timeout_ticks == osal::NO_WAIT)
            {
                return osal::error_code::timeout;
            }

            const osal::tick_t elapsed = osal_clock_ticks() - start;  // wrap-safe
            if (timeout_ticks != osal::WAIT_FOREVER && elapsed >= timeout_ticks)
            {
                return osal::error_code::timeout;
            }
            const osal::tick_t remaining =
                (timeout_ticks == osal::WAIT_FOREVER) ? osal::WAIT_FOREVER : (timeout_ticks - elapsed);

            (void)osal_semaphore_take(&s->space_sem, remaining);
        }
    }

    /// @brief Send from ISR context.  Never blocks.
    osal::result osal_message_buffer_send_isr(osal::active_traits::message_buffer_handle_t* handle, const void* msg,
                                              std::size_t len) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        if (!msg || len == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        auto*             s          = static_cast<emu_mb_obj*>(handle->native);
        const std::size_t frame_size = kMsgHeaderBytes + len;
        if (emu_mb_free(s) < frame_size)
        {
            return osal::error_code::timeout;
        }

        const osal_mb_length_t hdr = static_cast<osal_mb_length_t>(len);
        emu_mb_write(s, reinterpret_cast<const std::uint8_t*>(&hdr), kMsgHeaderBytes);
        emu_mb_write(s, static_cast<const std::uint8_t*>(msg), len);
        (void)osal_semaphore_give_isr(&s->data_sem);
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Receive (consumer)
    // ---------------------------------------------------------------------------

    /// @brief Receive one message from the buffer, blocking until a complete message arrives.
    /// @details Reads the 16-bit length header, then copies at most @p max_len bytes of
    ///          payload into @p buf (excess payload is discarded).
    /// @param handle        Message buffer handle.
    /// @param buf           Output buffer for the message payload.
    /// @param max_len       Capacity of @p buf in bytes.
    /// @param timeout_ticks Maximum ticks to wait; use `osal::WAIT_FOREVER` for indefinite.
    /// @return Number of payload bytes copied (capped at @p max_len), or 0 on timeout
    ///         or bad parameters.
    std::size_t osal_message_buffer_receive(osal::active_traits::message_buffer_handle_t* handle, void* buf,
                                            std::size_t max_len, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native || !buf || max_len == 0U)
        {
            return 0U;
        }
        auto* s = static_cast<emu_mb_obj*>(handle->native);

        const osal::tick_t start = osal_clock_ticks();

        while (true)
        {
            const std::size_t avail = emu_mb_available(s);
            if (avail >= kMsgHeaderBytes)
            {
                // Peek at the length header without advancing the read index.
                osal_mb_length_t msg_len = 0U;
                emu_mb_peek(s, reinterpret_cast<std::uint8_t*>(&msg_len), kMsgHeaderBytes);
                const std::size_t frame_size = kMsgHeaderBytes + static_cast<std::size_t>(msg_len);

                if (avail >= frame_size)
                {
                    // Full message present — consume it.
                    emu_mb_skip(s, kMsgHeaderBytes);  // advance past the header

                    if (msg_len <= static_cast<osal_mb_length_t>(max_len))
                    {
                        // Caller-supplied buffer is large enough → copy all payload.
                        emu_mb_read(s, static_cast<std::uint8_t*>(buf), static_cast<std::size_t>(msg_len));
                    }
                    else
                    {
                        // Truncate: copy max_len bytes, discard the rest.
                        emu_mb_read(s, static_cast<std::uint8_t*>(buf), max_len);
                        emu_mb_skip(s, static_cast<std::size_t>(msg_len) - max_len);
                    }

                    (void)osal_semaphore_give(&s->space_sem);  // notify producer

                    // If more complete messages are still queued, re-arm data_sem so
                    // the next caller (or this caller in a loop) does not have to wait.
                    if (emu_mb_available(s) >= kMsgHeaderBytes)
                    {
                        (void)osal_semaphore_give(&s->data_sem);
                    }
                    return std::min(static_cast<std::size_t>(msg_len), max_len);
                }
                // Header visible but payload not yet written — fall through to wait.
            }

            if (timeout_ticks == osal::NO_WAIT)
            {
                return 0U;
            }

            const osal::tick_t elapsed = osal_clock_ticks() - start;  // wrap-safe
            if (timeout_ticks != osal::WAIT_FOREVER && elapsed >= timeout_ticks)
            {
                return 0U;
            }
            const osal::tick_t remaining =
                (timeout_ticks == osal::WAIT_FOREVER) ? osal::WAIT_FOREVER : (timeout_ticks - elapsed);

            (void)osal_semaphore_take(&s->data_sem, remaining);
        }
    }

    /// @brief Receive from ISR context.  Never blocks.
    std::size_t osal_message_buffer_receive_isr(osal::active_traits::message_buffer_handle_t* handle, void* buf,
                                                std::size_t max_len) noexcept
    {
        if (!handle || !handle->native || !buf || max_len == 0U)
        {
            return 0U;
        }
        auto* s = static_cast<emu_mb_obj*>(handle->native);

        const std::size_t avail = emu_mb_available(s);
        if (avail < kMsgHeaderBytes)
        {
            return 0U;
        }

        osal_mb_length_t msg_len = 0U;
        emu_mb_peek(s, reinterpret_cast<std::uint8_t*>(&msg_len), kMsgHeaderBytes);
        const std::size_t frame_size = kMsgHeaderBytes + static_cast<std::size_t>(msg_len);
        if (avail < frame_size)
        {
            return 0U;
        }  // incomplete frame

        emu_mb_skip(s, kMsgHeaderBytes);

        if (msg_len <= static_cast<osal_mb_length_t>(max_len))
        {
            emu_mb_read(s, static_cast<std::uint8_t*>(buf), static_cast<std::size_t>(msg_len));
        }
        else
        {
            emu_mb_read(s, static_cast<std::uint8_t*>(buf), max_len);
            emu_mb_skip(s, static_cast<std::size_t>(msg_len) - max_len);
        }
        (void)osal_semaphore_give_isr(&s->space_sem);
        return std::min(static_cast<std::size_t>(msg_len), max_len);
    }

    // ---------------------------------------------------------------------------
    // Query + Reset
    // ---------------------------------------------------------------------------

    /// @brief Return the payload size of the next complete message, or 0 if none.
    /// @param handle Message buffer handle (const).
    /// @return Payload byte count of the oldest complete message, or 0.
    std::size_t osal_message_buffer_available(const osal::active_traits::message_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        const auto*       s     = static_cast<const emu_mb_obj*>(handle->native);
        const std::size_t avail = emu_mb_available(s);
        // Return the number of bytes in the first complete message, or 0 if none.
        if (avail < kMsgHeaderBytes)
        {
            return 0U;
        }
        osal_mb_length_t msg_len = 0U;
        emu_mb_peek(s, reinterpret_cast<std::uint8_t*>(&msg_len), kMsgHeaderBytes);
        const std::size_t frame_size = kMsgHeaderBytes + static_cast<std::size_t>(msg_len);
        return (avail >= frame_size) ? static_cast<std::size_t>(msg_len) : 0U;
    }

    /// @brief Return the maximum payload size that can currently be sent without blocking.
    /// @details Computed as `max(0, ring_free - kMsgHeaderBytes)` where `kMsgHeaderBytes` is 2.
    /// @param handle Message buffer handle (const).
    /// @return Maximum payload bytes, or 0 if the ring cannot accommodate even the header.
    std::size_t osal_message_buffer_free_space(const osal::active_traits::message_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        const auto*       s = static_cast<const emu_mb_obj*>(handle->native);
        const std::size_t f = emu_mb_free(s);
        // A message of size m requires kMsgHeaderBytes + m bytes, so the usable
        // payload capacity is max(0, free - kMsgHeaderBytes).
        return (f > kMsgHeaderBytes) ? (f - kMsgHeaderBytes) : 0U;
    }

    /// @brief Discard all buffered messages by advancing the consumer tail to the producer head.
    /// @param handle Message buffer handle.
    /// @return `osal::ok()` on success, `error_code::not_initialized` if null.
    osal::result osal_message_buffer_reset(osal::active_traits::message_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<emu_mb_obj*>(handle->native);
        s->tail.store(s->head.load(std::memory_order_acquire), std::memory_order_release);
        return osal::ok();
    }
