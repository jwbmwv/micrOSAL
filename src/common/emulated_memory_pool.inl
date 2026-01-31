// SPDX-License-Identifier: Apache-2.0
/// @file emulated_memory_pool.inl
/// @brief Portable emulated memory-pool implementation.
/// @details Uses the OSAL's own extern "C" primitives (mutex, semaphore) so the
///          same code works on every backend that implements those primitives.
///          #include this file inside the `extern "C"` block of any backend .cpp
///          that does NOT have a native fixed-size block pool API.
///
///          The implementation uses a bitmap to track free/allocated blocks and
///          a counting semaphore for blocking (timed) allocation.  Allocation
///          scans the bitmap for a free bit; deallocation computes the block
///          index from the pointer and clears the bit.
///
///          Prerequisites at the point of inclusion:
///          - <osal/osal.hpp> already included with the correct OSAL_BACKEND_*
///          - osal_mutex_*, osal_semaphore_* already defined
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

#include <atomic>
#include <cstring>

// ---------------------------------------------------------------------------
// Memory pool (emulated — bitmap + OSAL mutex + counting semaphore)
// ---------------------------------------------------------------------------

#ifndef OSAL_EMULATED_MPOOL_POOL_SIZE
/// @brief Maximum number of emulated memory pools that can exist concurrently.
#define OSAL_EMULATED_MPOOL_POOL_SIZE 8U
#endif

#ifndef OSAL_EMULATED_MPOOL_MAX_BLOCKS
/// @brief Maximum number of blocks any single emulated memory pool can manage.
///        Determines the bitmap array size.  ceil(MAX_BLOCKS/32) uint32_t words
///        are reserved per pool object.
#define OSAL_EMULATED_MPOOL_MAX_BLOCKS 256U
#endif

namespace  // anonymous — internal to the including TU
{

static constexpr std::size_t kMpoolBitmapWords = (OSAL_EMULATED_MPOOL_MAX_BLOCKS + 31U) / 32U;

struct emulated_pool_obj
{
    std::uint8_t* buffer;                     ///< Caller-supplied backing storage.
    std::size_t   block_size;                 ///< Size of each block in bytes.
    std::size_t   block_count;                ///< Total number of blocks.
    std::uint32_t bitmap[kMpoolBitmapWords];  ///< 1 = allocated, 0 = free.
    std::size_t   bitmap_words;               ///< Number of uint32_t entries actually used.
    std::size_t   available;                  ///< Current number of free blocks.

    osal::active_traits::mutex_handle_t     guard;  ///< Protects bitmap + available.
    osal::active_traits::semaphore_handle_t sem;    ///< Counting sem — free block count.
};

static emulated_pool_obj emu_mpool_pool[OSAL_EMULATED_MPOOL_POOL_SIZE];
static std::atomic_bool  emu_mpool_used[OSAL_EMULATED_MPOOL_POOL_SIZE];

static emulated_pool_obj* emu_mpool_acquire() noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_MPOOL_POOL_SIZE; ++i)
    {
        if (!emu_mpool_used[i].exchange(true, std::memory_order_acq_rel))
        {
            return &emu_mpool_pool[i];
        }
    }
    return nullptr;
}

static void emu_mpool_release(emulated_pool_obj* p) noexcept
{
    for (std::size_t i = 0U; i < OSAL_EMULATED_MPOOL_POOL_SIZE; ++i)
    {
        if (&emu_mpool_pool[i] == p)
        {
            emu_mpool_used[i].store(false, std::memory_order_release);
            return;
        }
    }
}

/// Find the first zero bit in the bitmap and set it.  Returns the block index,
/// or block_count if none is free (should not happen when called under sem).
static std::size_t pool_bitmap_alloc(emulated_pool_obj* p) noexcept
{
    for (std::size_t w = 0; w < p->bitmap_words; ++w)
    {
        if (p->bitmap[w] != 0xFFFFFFFFU)
        {
            for (unsigned b = 0; b < 32U; ++b)
            {
                const std::size_t idx = w * 32U + b;
                if (idx >= p->block_count)
                {
                    return p->block_count;
                }
                if ((p->bitmap[w] & (1U << b)) == 0U)
                {
                    p->bitmap[w] |= (1U << b);
                    --p->available;
                    return idx;
                }
            }
        }
    }
    return p->block_count;
}

/// Clear the bit for block @p idx.
static void pool_bitmap_free(emulated_pool_obj* p, std::size_t idx) noexcept
{
    const std::size_t w = idx / 32U;
    const unsigned    b = static_cast<unsigned>(idx % 32U);
    p->bitmap[w] &= ~(1U << b);
    ++p->available;
}

}  // anonymous namespace

// --- public extern "C" functions -------------------------------------------

/// @brief Create an emulated fixed-size memory pool backed by caller-supplied storage.
/// @details Uses a bitmap to track free blocks and a counting semaphore for timed
///          allocation.  No dynamic allocation is performed by the pool itself.
/// @param handle      Output handle; populated on success.
/// @param buffer      Caller-supplied backing storage of at least @p buf_bytes bytes.
/// @param buf_bytes   Total size of @p buffer in bytes; must be >= `block_size * block_count`.
/// @param block_size  Size of each allocation block in bytes.
/// @param block_count Number of blocks in the pool; must not exceed `OSAL_EMULATED_MPOOL_MAX_BLOCKS`.
/// @param name        Optional human-readable name (ignored by the emulation).
/// @return `osal::ok()` on success, `error_code::invalid_argument` for bad parameters,
///         `error_code::out_of_resources` if the pool or internal semaphore cannot be created.
osal::result osal_memory_pool_create(osal::active_traits::memory_pool_handle_t* handle, void* buffer,
                                     std::size_t buf_bytes, std::size_t block_size, std::size_t block_count,
                                     const char* /*name*/) noexcept
{
    if (!handle || !buffer || block_size == 0 || block_count == 0)
    {
        return osal::error_code::invalid_argument;
    }
    if (buf_bytes < block_size * block_count)
    {
        return osal::error_code::invalid_argument;
    }

    auto* pool = emu_mpool_acquire();
    if (!pool)
    {
        return osal::error_code::out_of_resources;
    }

    pool->buffer       = static_cast<std::uint8_t*>(buffer);
    pool->block_size   = block_size;
    pool->block_count  = block_count;
    pool->available    = block_count;
    pool->bitmap_words = (block_count + 31U) / 32U;

    if (pool->bitmap_words > kMpoolBitmapWords)
    {
        emu_mpool_release(pool);
        return osal::error_code::invalid_argument;  // Too many blocks for static bitmap.
    }
    std::memset(pool->bitmap, 0, pool->bitmap_words * sizeof(std::uint32_t));

    // Create the mutex (non-recursive).
    if (!osal_mutex_create(&pool->guard, false).ok())
    {
        emu_mpool_release(pool);
        return osal::error_code::out_of_resources;
    }

    // Counting semaphore — initial = block_count, max = block_count.
    if (!osal_semaphore_create(&pool->sem, static_cast<unsigned>(block_count), static_cast<unsigned>(block_count)).ok())
    {
        osal_mutex_destroy(&pool->guard);
        emu_mpool_release(pool);
        return osal::error_code::out_of_resources;
    }

    handle->native = pool;
    return osal::ok();
}

/// @brief Destroy an emulated memory pool and release its pool slot.
/// @param handle Handle to destroy; silently ignored if null or uninitialized.
/// @return Always `osal::ok()`.
osal::result osal_memory_pool_destroy(osal::active_traits::memory_pool_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }
    auto* pool = static_cast<emulated_pool_obj*>(handle->native);

    osal_semaphore_destroy(&pool->sem);
    osal_mutex_destroy(&pool->guard);
    emu_mpool_release(pool);
    handle->native = nullptr;
    return osal::ok();
}

/// @brief Allocate one block from the pool (non-blocking).
/// @details Attempts a non-blocking semaphore take and, if successful, claims the
///          first free bitmap slot.
/// @param handle Pool handle.
/// @return Pointer to the allocated block on success, or `nullptr` if the pool is
///         exhausted or @p handle is null.
void* osal_memory_pool_allocate(osal::active_traits::memory_pool_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return nullptr;
    }
    auto* pool = static_cast<emulated_pool_obj*>(handle->native);

    // Non-blocking: try to take the semaphore.
    if (!osal_semaphore_try_take(&pool->sem).ok())
    {
        return nullptr;  // Pool exhausted.
    }

    osal_mutex_lock(&pool->guard, osal::WAIT_FOREVER);
    const std::size_t idx = pool_bitmap_alloc(pool);
    osal_mutex_unlock(&pool->guard);

    if (idx >= pool->block_count)
    {
        // Should not happen (sem said a slot was free).  Give the sem back.
        osal_semaphore_give(&pool->sem);
        return nullptr;
    }

    return pool->buffer + idx * pool->block_size;
}

/// @brief Allocate one block from the pool, blocking up to @p timeout_ticks.
/// @param handle        Pool handle.
/// @param timeout_ticks Maximum ticks to wait for a free block;
///                      use `osal::WAIT_FOREVER` to block indefinitely.
/// @return Pointer to the allocated block on success, or `nullptr` on timeout
///         or if @p handle is null.
void* osal_memory_pool_allocate_timed(osal::active_traits::memory_pool_handle_t* handle,
                                      osal::tick_t                               timeout_ticks) noexcept
{
    if (!handle || !handle->native)
    {
        return nullptr;
    }
    auto* pool = static_cast<emulated_pool_obj*>(handle->native);

    // Blocking take — waits up to timeout_ticks for a free block.
    if (!osal_semaphore_take(&pool->sem, timeout_ticks).ok())
    {
        return nullptr;  // Timeout — pool exhausted.
    }

    osal_mutex_lock(&pool->guard, osal::WAIT_FOREVER);
    const std::size_t idx = pool_bitmap_alloc(pool);
    osal_mutex_unlock(&pool->guard);

    if (idx >= pool->block_count)
    {
        osal_semaphore_give(&pool->sem);
        return nullptr;
    }

    return pool->buffer + idx * pool->block_size;
}

/// @brief Return a previously allocated block to the pool.
/// @details Validates that @p block is pointer-aligned within the pool's backing
///          buffer and not already free (double-free detection).
/// @param handle Pool handle.
/// @param block  Pointer previously returned by `osal_memory_pool_allocate[_timed]`.
/// @return `osal::ok()` on success, `error_code::invalid_argument` for a bad pointer
///         or double-free, or if @p handle is null.
osal::result osal_memory_pool_deallocate(osal::active_traits::memory_pool_handle_t* handle, void* block) noexcept
{
    if (!handle || !handle->native || !block)
    {
        return osal::error_code::invalid_argument;
    }
    auto* pool = static_cast<emulated_pool_obj*>(handle->native);

    auto* blk = static_cast<std::uint8_t*>(block);
    if (blk < pool->buffer || blk >= pool->buffer + pool->block_count * pool->block_size)
    {
        return osal::error_code::invalid_argument;
    }

    const std::size_t offset = static_cast<std::size_t>(blk - pool->buffer);
    if (offset % pool->block_size != 0)
    {
        return osal::error_code::invalid_argument;  // Not block-aligned.
    }

    const std::size_t idx = offset / pool->block_size;

    osal_mutex_lock(&pool->guard, osal::WAIT_FOREVER);
    const std::size_t w = idx / 32U;
    const unsigned    b = static_cast<unsigned>(idx % 32U);
    if ((pool->bitmap[w] & (1U << b)) == 0U)
    {
        // Double-free detected.
        osal_mutex_unlock(&pool->guard);
        return osal::error_code::invalid_argument;
    }
    pool_bitmap_free(pool, idx);
    osal_mutex_unlock(&pool->guard);

    // Signal that a block is available.
    osal_semaphore_give(&pool->sem);
    return osal::ok();
}

/// @brief Query the number of currently free blocks in the pool.
/// @param handle Pool handle (const).
/// @return Number of free blocks, or 0 if @p handle is null.
std::size_t osal_memory_pool_available(const osal::active_traits::memory_pool_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return 0U;
    }
    auto* pool = static_cast<const emulated_pool_obj*>(handle->native);
    return pool->available;
}
