// SPDX-License-Identifier: Apache-2.0
/// @file emulated_stream_buffer.inl
/// @brief Portable emulated stream-buffer implementation.
/// @details Provides FreeRTOS-compatible stream-buffer semantics using a
///          lock-free SPSC byte ring backed by a caller-supplied static buffer,
///          with OS semaphores for blocking and ISR-safe signalling.
///
///          Design overview
///          ──────────────────────────────────────────────────────────────────
///          Internal ring (same SPSC pattern as ring_buffer.hpp, extended to
///          multi-byte operations):
///          • head_ — write index, producer-owned (std::atomic, release on
///            store).
///          • tail_ — read index,  consumer-owned (std::atomic, release on
///            store).
///          • ring_size = capacity + 1; one sentinel slot separates full/empty.
///          • No ABA issue: indices only ever increase (modulo ring_size).
///
///          Blocking semaphores (binary, max_count = 1):
///          • data_sem : given by producer after write (notifies consumer of
///            available data). Consumer takes to block when below trigger.
///          • space_sem: given by consumer after read (notifies producer of
///            freed space). Producer takes to block when ring is full.
///          Because consumer always reads ALL available bytes above the trigger
///          (not just the amount that triggered wakeup), a binary semaphore is
///          sufficient — the consumer drains the ring in a tight re-check loop
///          before blocking again.
///
///          ISR safety:
///          • send_isr  : non-blocking write + osal_semaphore_give_isr(data_sem).
///          • receive_isr: non-blocking read   + osal_semaphore_give_isr(space_sem).
///          • On backends with has_isr_semaphore == true the ISR paths are
///            genuinely interrupt-safe.  On others they are NOT ISR-safe (same
///            caveat as osal::semaphore::isr_give()).
///
///          Prerequisites at the point of inclusion:
///          • <osal/osal.hpp> already included with the correct OSAL_BACKEND_*
///          • osal_semaphore_create/destroy/give/take/give_isr already defined
///          • osal_clock_ticks already defined
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

// These headers define C++ templates and must never be included inside an
// extern "C" { } block.  If this .inl file is included from within such a
// block (as all backends do), temporarily close it, pull in the headers,
// then re-open it so the function definitions that follow keep C linkage.
#ifdef __cplusplus
}  // temporarily close extern "C" (opened by the including backend)
#endif
#include <atomic>
#include <algorithm>
#include <cstring>
#ifdef __cplusplus
extern "C"
{  // re-open for the function definitions below
#endif

    // ---------------------------------------------------------------------------
    // Pool sizing tuning macros
    // ---------------------------------------------------------------------------

#ifndef OSAL_EMULATED_STREAM_BUFFER_POOL_SIZE
/// @brief Maximum number of emulated stream buffers that can exist concurrently.
#define OSAL_EMULATED_STREAM_BUFFER_POOL_SIZE 8U
#endif

    namespace  // anonymous — internal to the including TU
    {

    // ---------------------------------------------------------------------------
    // Internal control object
    // ---------------------------------------------------------------------------

    struct emu_sb_obj
    {
        std::uint8_t* buf;        ///< Caller-supplied storage (capacity+1 bytes).
        std::size_t   ring_size;  ///< = capacity + 1 (one sentinel slot).
        std::size_t   capacity;   ///< = N (usable bytes).
        std::size_t   trigger;    ///< Bytes-available threshold to wake consumer.

        /// @brief Write index — producer-owned.  Range [0, ring_size).
        std::atomic<std::size_t> head;
        /// @brief Read index — consumer-owned.  Range [0, ring_size).
        std::atomic<std::size_t> tail;

        /// @brief Binary semaphore (max=1): given by producer; taken by consumer.
        osal::active_traits::semaphore_handle_t data_sem;
        /// @brief Binary semaphore (max=1): given by consumer; taken by producer.
        osal::active_traits::semaphore_handle_t space_sem;
    };

    static emu_sb_obj emu_sb_pool[OSAL_EMULATED_STREAM_BUFFER_POOL_SIZE];
    static bool       emu_sb_used[OSAL_EMULATED_STREAM_BUFFER_POOL_SIZE];

    static emu_sb_obj* emu_sb_acquire() noexcept
    {
        for (std::size_t i = 0U; i < OSAL_EMULATED_STREAM_BUFFER_POOL_SIZE; ++i)
        {
            if (!emu_sb_used[i])
            {
                emu_sb_used[i] = true;
                return &emu_sb_pool[i];
            }
        }
        return nullptr;
    }

    static void emu_sb_release(emu_sb_obj* p) noexcept
    {
        for (std::size_t i = 0U; i < OSAL_EMULATED_STREAM_BUFFER_POOL_SIZE; ++i)
        {
            if (&emu_sb_pool[i] == p)
            {
                emu_sb_used[i] = false;
                return;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // SPSC ring helpers  (identical acquire/release contract to ring_buffer.hpp)
    // ---------------------------------------------------------------------------

    /// @brief Bytes currently available to read.
    /// @note  Safe to call from consumer or observer thread.
    static std::size_t sb_available(const emu_sb_obj* s) noexcept
    {
        const std::size_t h = s->head.load(std::memory_order_acquire);
        const std::size_t t = s->tail.load(std::memory_order_acquire);
        return (h >= t) ? (h - t) : (s->ring_size - t + h);
    }

    /// @brief Free bytes that can be written without blocking.
    static std::size_t sb_free(const emu_sb_obj* s) noexcept
    {
        return s->capacity - sb_available(s);
    }

    /// @brief Write @p len bytes from @p data into the ring.
    /// @pre   sb_free(s) >= len.  Call only from the producer.
    static void sb_write(emu_sb_obj* s, const std::uint8_t* data, std::size_t len) noexcept
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

    /// @brief Read @p len bytes from the ring into @p buf.
    /// @pre   sb_available(s) >= len.  Call only from the consumer.
    static void sb_read(emu_sb_obj* s, std::uint8_t* buf, std::size_t len) noexcept
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

    /// @brief Discard @p len bytes from the head of the readable region.
    /// @pre   sb_available(s) >= len.  Call only from the consumer.
    [[maybe_unused]] static void sb_skip(emu_sb_obj* s, std::size_t len) noexcept
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

    /// @brief Peek at the next @p len bytes without advancing tail.
    /// @pre   sb_available(s) >= len.  Call only from the consumer.
    [[maybe_unused]] static void sb_peek(const emu_sb_obj* s, std::uint8_t* buf, std::size_t len) noexcept
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
    // C-linkage stream buffer functions
    // ---------------------------------------------------------------------------

    /// @brief Create an emulated stream buffer backed by caller-supplied storage.
    /// @details Uses a lock-free SPSC byte ring with binary semaphores for blocking
    ///          I/O and ISR-safe signalling.  The caller must supply @p capacity + 1
    ///          bytes of storage to accommodate the ring sentinel slot.
    /// @param handle        Output handle; populated on success.
    /// @param buffer        Caller-supplied storage; must be at least @p capacity + 1 bytes.
    /// @param capacity      Usable byte capacity of the stream buffer.
    /// @param trigger_level Minimum bytes available before a blocking receive unblocks;
    ///                      clamped to 1 if 0 is passed.
    /// @return `osal::ok()` on success, `error_code::invalid_argument` for bad parameters,
    ///         `error_code::out_of_resources` if the pool or semaphores are exhausted.
    osal::result osal_stream_buffer_create(osal::active_traits::stream_buffer_handle_t* handle, void* buffer,
                                           std::size_t capacity, std::size_t trigger_level) noexcept
    {
        if (!handle || !buffer || capacity == 0U)
        {
            return osal::error_code::invalid_argument;
        }

        emu_sb_obj* s = emu_sb_acquire();
        if (!s)
        {
            return osal::error_code::out_of_resources;
        }

        s->buf       = static_cast<std::uint8_t*>(buffer);
        s->ring_size = capacity + 1U;
        s->capacity  = capacity;
        s->trigger   = (trigger_level == 0U) ? 1U : trigger_level;
        s->head.store(0U, std::memory_order_relaxed);
        s->tail.store(0U, std::memory_order_relaxed);

        // Binary semaphores — max_count = 1, both start empty.
        const bool d_ok = osal_semaphore_create(&s->data_sem, 0U, 1U).ok();
        const bool p_ok = osal_semaphore_create(&s->space_sem, 0U, 1U).ok();
        if (!d_ok || !p_ok)
        {
            if (d_ok)
            {
                (void)osal_semaphore_destroy(&s->data_sem);
            }
            emu_sb_release(s);
            return osal::error_code::out_of_resources;
        }

        handle->native = s;
        return osal::ok();
    }

    /// @brief Destroy an emulated stream buffer and release its pool slot.
    /// @param handle Handle to destroy; silently ignored if null or uninitialized.
    /// @return `osal::ok()` on success, `error_code::not_initialized` if null.
    osal::result osal_stream_buffer_destroy(osal::active_traits::stream_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<emu_sb_obj*>(handle->native);
        (void)osal_semaphore_destroy(&s->data_sem);
        (void)osal_semaphore_destroy(&s->space_sem);
        emu_sb_release(s);
        handle->native = nullptr;
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Send (producer)
    // ---------------------------------------------------------------------------

    /// @brief Write @p len bytes into the stream buffer, blocking until space is available.
    /// @param handle        Stream buffer handle.
    /// @param data          Data to write.
    /// @param len           Number of bytes to write; must be <= @p capacity.
    /// @param timeout_ticks Maximum ticks to wait for space; use `osal::WAIT_FOREVER` for indefinite.
    /// @return `osal::ok()` on success, `error_code::timeout` if space is not available within
    ///         the timeout, `error_code::not_initialized` or `error_code::invalid_argument` for
    ///         bad parameters.
    osal::result osal_stream_buffer_send(osal::active_traits::stream_buffer_handle_t* handle, const void* data,
                                         std::size_t len, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        if (!data || len == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        auto* s = static_cast<emu_sb_obj*>(handle->native);
        if (len > s->capacity)
        {
            return osal::error_code::invalid_argument;
        }

        const osal::tick_t start = osal_clock_ticks();

        while (true)
        {
            if (sb_free(s) >= len)
            {
                sb_write(s, static_cast<const std::uint8_t*>(data), len);
                (void)osal_semaphore_give(&s->data_sem);  // wake consumer; idempotent if binary
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
    osal::result osal_stream_buffer_send_isr(osal::active_traits::stream_buffer_handle_t* handle, const void* data,
                                             std::size_t len) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        if (!data || len == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        auto* s = static_cast<emu_sb_obj*>(handle->native);
        if (sb_free(s) < len)
        {
            return osal::error_code::timeout;
        }

        sb_write(s, static_cast<const std::uint8_t*>(data), len);
        (void)osal_semaphore_give_isr(&s->data_sem);
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Receive (consumer)
    // ---------------------------------------------------------------------------

    /// @brief Read up to @p max_len bytes from the stream buffer, blocking until the
    ///        trigger level is reached or the timeout expires.
    /// @param handle        Stream buffer handle.
    /// @param buf           Output buffer for received bytes.
    /// @param max_len       Maximum bytes to read.
    /// @param timeout_ticks Maximum ticks to block; use `osal::WAIT_FOREVER` for indefinite.
    /// @return Number of bytes actually read, or 0 on timeout or bad parameters.
    std::size_t osal_stream_buffer_receive(osal::active_traits::stream_buffer_handle_t* handle, void* buf,
                                           std::size_t max_len, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native || !buf || max_len == 0U)
        {
            return 0U;
        }
        auto* s = static_cast<emu_sb_obj*>(handle->native);

        const osal::tick_t start = osal_clock_ticks();

        while (true)
        {
            const std::size_t avail = sb_available(s);
            if (avail >= s->trigger)
            {
                const std::size_t n = std::min(avail, max_len);
                sb_read(s, static_cast<std::uint8_t*>(buf), n);
                (void)osal_semaphore_give(&s->space_sem);  // wake producer; idempotent if binary
                return n;
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
    std::size_t osal_stream_buffer_receive_isr(osal::active_traits::stream_buffer_handle_t* handle, void* buf,
                                               std::size_t max_len) noexcept
    {
        if (!handle || !handle->native || !buf || max_len == 0U)
        {
            return 0U;
        }
        auto* s = static_cast<emu_sb_obj*>(handle->native);

        const std::size_t avail = sb_available(s);
        if (avail < s->trigger)
        {
            return 0U;
        }

        const std::size_t n = std::min(avail, max_len);
        sb_read(s, static_cast<std::uint8_t*>(buf), n);
        (void)osal_semaphore_give_isr(&s->space_sem);
        return n;
    }

    // ---------------------------------------------------------------------------
    // Query
    // ---------------------------------------------------------------------------

    /// @brief Return the number of bytes currently available to read.
    /// @param handle Stream buffer handle (const).
    /// @return Byte count, or 0 if @p handle is null.
    std::size_t osal_stream_buffer_available(const osal::active_traits::stream_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        return sb_available(static_cast<const emu_sb_obj*>(handle->native));
    }

    /// @brief Return the number of bytes that can be written without blocking.
    /// @param handle Stream buffer handle (const).
    /// @return Free byte count, or 0 if @p handle is null.
    std::size_t osal_stream_buffer_free_space(const osal::active_traits::stream_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        return sb_free(static_cast<const emu_sb_obj*>(handle->native));
    }

    // ---------------------------------------------------------------------------
    // Reset
    // ---------------------------------------------------------------------------

    /// @brief Discard all buffered data by advancing the consumer tail to the producer head.
    /// @param handle Stream buffer handle.
    /// @return `osal::ok()` on success, `error_code::not_initialized` if null.
    osal::result osal_stream_buffer_reset(osal::active_traits::stream_buffer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<emu_sb_obj*>(handle->native);
        // Flush by advancing tail to head — consumer discards all pending data.
        s->tail.store(s->head.load(std::memory_order_acquire), std::memory_order_release);
        return osal::ok();
    }
