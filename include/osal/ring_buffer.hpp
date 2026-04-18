// SPDX-License-Identifier: Apache-2.0
/// @file ring_buffer.hpp
/// @brief Lock-free single-producer / single-consumer ring buffer
/// @details A fixed-capacity, cache-friendly SPSC queue that requires NO OS
///          primitives — only standard @c std::atomic with acquire/release
///          semantics under MicrOSAL's required feature-set baseline.
///          Ideal for ISR-to-task or task-to-task data transfer at high
///          throughput without any mutex or semaphore overhead.
///
///          Constraints on T:
///          - std::is_trivially_copyable<T>
///          - std::is_default_constructible<T>
///
///          The capacity is fixed at compile time via template parameter N.
///          Actual usable slots = N (one sentinel slot is added internally).
///
///          Thread-safety guarantee:
///          - Exactly ONE producer thread may call push() / try_push().
///          - Exactly ONE consumer thread may call pop()  / try_pop() / peek().
///          - No other synchronisation is required between the two threads.
///          - Calling from more than one producer or consumer is UNDEFINED.
///
///          Usage:
///          @code
///          osal::ring_buffer<uint16_t, 64> rb;
///
///          // ISR / producer:
///          rb.try_push(adc_sample);
///
///          // Task / consumer:
///          uint16_t sample;
///          if (rb.try_pop(sample)) { process(sample); }
///          @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_ring_buffer
#pragma once

#include "concepts.hpp"

#include "detail/atomic_compat.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace osal
{

/// @defgroup osal_ring_buffer OSAL Ring Buffer
/// @brief Lock-free SPSC ring buffer (no OS dependency).
/// @{

/// @brief Lock-free single-producer / single-consumer ring buffer.
/// @tparam T  Element type satisfying @c osal::ring_buffer_element.
/// @tparam N  Number of usable slots (actual internal array is N+1).
template<ring_buffer_element T, std::size_t N>
    requires positive_extent<N>
class ring_buffer
{
public:
    // ---- construction ------------------------------------------------------

    /// @brief Constructs an empty ring buffer.
    ring_buffer() noexcept = default;

    ~ring_buffer() noexcept = default;

    ring_buffer(const ring_buffer&)            = delete;
    ring_buffer& operator=(const ring_buffer&) = delete;
    ring_buffer(ring_buffer&&)                 = delete;
    ring_buffer& operator=(ring_buffer&&)      = delete;

    // ---- producer API (single thread only) ---------------------------------

    /// @brief Push an item into the buffer.
    /// @param item  The item to enqueue.
    /// @return true if enqueued; false if the buffer is full.
    bool try_push(const T& item) noexcept
    {
        const std::size_t h    = head_.load(std::memory_order_relaxed);  // NOLINT(cppcoreguidelines-init-variables)
        const std::size_t next = increment(h);
        if (next == tail_.load(std::memory_order_acquire))
        {
            return false;  // NOLINT(readability-simplify-boolean-expr) full
        }
        buf_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// @brief Push up to @p count items into the buffer.
    /// @param items  Source array of items to enqueue.
    /// @param count  Number of items to attempt to enqueue.
    /// @return Number of items actually enqueued (may be less than @p count).
    std::size_t try_push_n(const T* items, std::size_t count) noexcept
    {
        const std::size_t h     = head_.load(std::memory_order_relaxed);  // NOLINT(cppcoreguidelines-init-variables)
        const std::size_t t     = tail_.load(std::memory_order_acquire);  // NOLINT(cppcoreguidelines-init-variables)
        const std::size_t avail = (t > h) ? (t - h - 1U) : (kCapacity - h + t - 1U);
        const std::size_t n     = min_of(count, avail);
        if (n == 0U)
        {
            return 0U;
        }
        const std::size_t first = min_of(n, kCapacity - h);
        std::memcpy(&buf_[h], items, first * sizeof(T));
        if (first < n)
        {
            std::memcpy(&buf_[0], items + first, (n - first) * sizeof(T));
        }
        const std::size_t new_h = (h + n < kCapacity) ? (h + n) : (h + n - kCapacity);
        head_.store(new_h, std::memory_order_release);
        return n;
    }

    /// @brief Push up to @p items.size() items into the buffer.
    /// @param items  Span of items to enqueue.
    /// @return Number of items actually enqueued.
    std::size_t try_push_n(std::span<const T> items) noexcept { return try_push_n(items.data(), items.size()); }

    // ---- consumer API (single thread only) ---------------------------------

    /// @brief Pop the oldest item from the buffer.
    /// @param[out] item  Receives the dequeued item.
    /// @return true if an item was dequeued; false if the buffer was empty.
    bool try_pop(T& item) noexcept
    {
        const std::size_t t = tail_.load(std::memory_order_relaxed);  // NOLINT(cppcoreguidelines-init-variables)
        if (t == head_.load(std::memory_order_acquire))
        {
            return false;  // NOLINT(readability-simplify-boolean-expr) empty
        }
        item = buf_[t];
        tail_.store(increment(t), std::memory_order_release);
        return true;
    }

    /// @brief Pop up to @p count items from the buffer.
    /// @param[out] items  Destination array for dequeued items.
    /// @param      count  Maximum number of items to dequeue.
    /// @return Number of items actually dequeued (may be less than @p count).
    std::size_t try_pop_n(T* items, std::size_t count) noexcept
    {
        const std::size_t t     = tail_.load(std::memory_order_relaxed);  // NOLINT(cppcoreguidelines-init-variables)
        const std::size_t h     = head_.load(std::memory_order_acquire);  // NOLINT(cppcoreguidelines-init-variables)
        const std::size_t avail = (h >= t) ? (h - t) : (kCapacity - t + h);
        const std::size_t n     = min_of(count, avail);
        if (n == 0U)
        {
            return 0U;
        }
        const std::size_t first = min_of(n, kCapacity - t);
        std::memcpy(items, &buf_[t], first * sizeof(T));
        if (first < n)
        {
            std::memcpy(items + first, &buf_[0], (n - first) * sizeof(T));
        }
        const std::size_t new_t = (t + n < kCapacity) ? (t + n) : (t + n - kCapacity);
        tail_.store(new_t, std::memory_order_release);
        return n;
    }

    /// @brief Pop up to @p items.size() items from the buffer.
    /// @param[out] items  Span to receive dequeued items.
    /// @return Number of items actually dequeued.
    std::size_t try_pop_n(std::span<T> items) noexcept { return try_pop_n(items.data(), items.size()); }

    /// @brief Peek at the oldest item without removing it.
    /// @param[out] item  Receives a copy of the head item.
    /// @return true if an item was available; false if the buffer was empty.
    bool peek(T& item) const noexcept
    {
        const std::size_t t = tail_.load(std::memory_order_relaxed);  // NOLINT(cppcoreguidelines-init-variables)
        if (t == head_.load(std::memory_order_acquire))
        {
            return false;  // NOLINT(readability-simplify-boolean-expr)
        }
        item = buf_[t];
        return true;
    }

    // ---- query (safe from either thread) -----------------------------------

    /// @brief Returns the number of items currently in the buffer.
    /// @note  This is a snapshot; the value may be stale by the time
    ///        the caller acts on it.
    [[nodiscard]] std::size_t size() const noexcept
    {
        const std::size_t h = head_.load(std::memory_order_acquire);  // NOLINT(cppcoreguidelines-init-variables)
        const std::size_t t = tail_.load(std::memory_order_acquire);  // NOLINT(cppcoreguidelines-init-variables)
        if (h >= t)
        {
            return h - t;
        }
        return kCapacity - t + h;
    }

    /// @brief Returns the number of free slots.
    [[nodiscard]] std::size_t free() const noexcept { return N - size(); }

    /// @brief Returns true if the buffer is empty.
    [[nodiscard]] bool empty() const noexcept
    {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    /// @brief Returns true if the buffer is full.
    [[nodiscard]] bool full() const noexcept
    {
        return increment(head_.load(std::memory_order_acquire)) == tail_.load(std::memory_order_acquire);
    }

    /// @brief Compile-time capacity (number of usable slots).
    static constexpr std::size_t capacity() noexcept { return N; }

    /// @brief Discard all items.  Must be called from the consumer thread
    ///        (or when no concurrent access is happening).
    void reset() noexcept { tail_.store(head_.load(std::memory_order_relaxed), std::memory_order_release); }

private:
    static constexpr std::size_t kCapacity = N + 1;  // one sentinel slot

    /// @brief Advance an index, wrapping around.
    static constexpr std::size_t increment(std::size_t idx) noexcept { return (idx + 1 == kCapacity) ? 0 : idx + 1; }

    /// @brief Branchless min without pulling in <algorithm>.
    static constexpr std::size_t min_of(std::size_t a, std::size_t b) noexcept { return (a < b) ? a : b; }

    T                        buf_[kCapacity]{};
    std::atomic<std::size_t> head_{0U};
    std::atomic<std::size_t> tail_{0U};
};

/// @} // osal_ring_buffer

}  // namespace osal
