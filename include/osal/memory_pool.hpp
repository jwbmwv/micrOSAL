// SPDX-License-Identifier: Apache-2.0
/// @file memory_pool.hpp
/// @brief OSAL memory pool — deterministic fixed-size block allocator
/// @details Provides osal::memory_pool for allocating and freeing fixed-size
///          memory blocks from a pre-allocated buffer, with O(1) alloc/free and
///          zero fragmentation.
///
///          On backends with native block pools (Zephyr k_mem_slab, ThreadX/PX5
///          tx_block_pool, VxWorks memPartLib, Micrium OSMemCreate, ChibiOS
///          chPoolInit, embOS OS_MEMPOOL) the native primitive is used.
///          On all other backends, a portable bitmap-based allocator is provided.
///
///          The caller supplies the backing storage buffer.
///
///          Usage:
///          @code
///          alignas(alignof(my_struct)) static uint8_t pool_buf[sizeof(my_struct) * 32];
///          osal::memory_pool pool{pool_buf, sizeof(pool_buf), sizeof(my_struct), 32};
///
///          void* blk = pool.allocate();
///          if (blk) {
///              auto* obj = new (blk) my_struct{};
///              // ...
///              obj->~my_struct();
///              pool.deallocate(blk);
///          }
///          @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_memory_pool
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>

extern "C"
{
    /// @brief Create a fixed-size block pool.
    /// @param handle     Output handle.
    /// @param buffer     Caller-supplied backing storage.
    /// @param buf_bytes  Total size of buffer in bytes.
    /// @param block_size Size of each block in bytes.
    /// @param block_count Number of blocks.
    /// @param name       Debug name (may be nullptr).
    osal::result osal_memory_pool_create(osal::active_traits::memory_pool_handle_t* handle, void* buffer,
                                         std::size_t buf_bytes, std::size_t block_size, std::size_t block_count,
                                         const char* name) noexcept;

    osal::result osal_memory_pool_destroy(osal::active_traits::memory_pool_handle_t* handle) noexcept;

    /// @brief Allocate one block (non-blocking).
    /// @return Pointer to the block, or nullptr if the pool is exhausted.
    void* osal_memory_pool_allocate(osal::active_traits::memory_pool_handle_t* handle) noexcept;

    /// @brief Allocate one block with timeout.
    /// @return Pointer to the block, or nullptr on timeout / pool exhausted.
    void* osal_memory_pool_allocate_timed(osal::active_traits::memory_pool_handle_t* handle,
                                          osal::tick_t                               timeout_ticks) noexcept;

    /// @brief Free a previously allocated block back to the pool.
    osal::result osal_memory_pool_deallocate(osal::active_traits::memory_pool_handle_t* handle, void* block) noexcept;

    /// @brief Number of blocks currently available.
    std::size_t osal_memory_pool_available(const osal::active_traits::memory_pool_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_memory_pool OSAL Memory Pool
/// @brief Deterministic fixed-size block allocator.
/// @{

// ---------------------------------------------------------------------------
// memory_pool_config — constexpr-constructible, place in .rodata / FLASH
// ---------------------------------------------------------------------------

/// @brief Immutable configuration for memory pool creation.
/// @details Declare as @c const to place in .rodata (FLASH).  The @c buffer
///          pointer targets RAM, but the config descriptor itself lives in FLASH.
///
///          @code
///          alignas(my_struct) static uint8_t pool_buf[sizeof(my_struct) * 32];
///          const osal::memory_pool_config pool_cfg{
///              pool_buf, sizeof(pool_buf), sizeof(my_struct), 32, "my_pool"};
///          osal::memory_pool pool{pool_cfg};
///          @endcode
struct memory_pool_config
{
    void*       buffer      = nullptr;  ///< Caller-supplied backing storage.
    std::size_t buf_bytes   = 0;        ///< Total size of buffer in bytes.
    std::size_t block_size  = 0;        ///< Size of each block in bytes.
    std::size_t block_count = 0;        ///< Number of blocks.
    const char* name        = nullptr;  ///< Debug name (may be nullptr).
};

/// @brief OSAL memory pool — fixed-size block allocator.
///
/// @details Allocates and frees equal-sized blocks from a caller-supplied
///          buffer with O(1) time, zero fragmentation, and no heap usage.
///
///          On RTOS backends with native block pools, the native implementation
///          is used.  Otherwise a portable bitmap-based allocator is provided.
class memory_pool
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs a memory pool.
    /// @param buffer       Caller-supplied backing storage (must be
    ///                     aligned to block_size alignment requirements).
    /// @param buf_bytes    Size of buffer in bytes.
    /// @param block_size   Size of each block in bytes.
    /// @param block_count  Number of blocks that fit in the buffer.
    /// @param name         Debug name (may be nullptr).
    memory_pool(void* buffer, std::size_t buf_bytes, std::size_t block_size, std::size_t block_count,
                const char* name = nullptr) noexcept
        : valid_(false), block_size_(block_size), handle_{}
    {
        valid_ = osal_memory_pool_create(&handle_, buffer, buf_bytes, block_size, block_count, name).ok();
    }

    /// @brief Constructs from an immutable config (config may reside in FLASH).
    /// @param cfg  Configuration — typically declared @c const.
    /// @complexity O(1)
    explicit memory_pool(const memory_pool_config& cfg) noexcept : valid_(false), block_size_(cfg.block_size), handle_{}
    {
        valid_ = osal_memory_pool_create(&handle_, cfg.buffer, cfg.buf_bytes, cfg.block_size, cfg.block_count, cfg.name)
                     .ok();
    }

    /// @brief Destroys the memory pool.
    ~memory_pool() noexcept
    {
        if (valid_)
        {
            (void)osal_memory_pool_destroy(&handle_);
            valid_ = false;
        }
    }

    memory_pool(const memory_pool&)            = delete;
    memory_pool& operator=(const memory_pool&) = delete;
    memory_pool(memory_pool&&)                 = delete;
    memory_pool& operator=(memory_pool&&)      = delete;

    // ---- allocate / deallocate ---------------------------------------------

    /// @brief Allocate one block (non-blocking).
    /// @return Pointer to the block, or nullptr if exhausted.
    [[nodiscard]] void* allocate() noexcept { return osal_memory_pool_allocate(&handle_); }

    /// @brief Allocate one block with timeout.
    /// @param timeout  Maximum time to wait for a free block.
    /// @return Pointer to the block, or nullptr on timeout.
    [[nodiscard]] void* allocate_for(milliseconds timeout) noexcept
    {
        const tick_t ticks = clock_utils::ms_to_ticks(timeout);
        return osal_memory_pool_allocate_timed(&handle_, ticks);
    }

    /// @brief Return a previously allocated block to the pool.
    /// @param block  Pointer previously returned by allocate().
    result deallocate(void* block) noexcept { return osal_memory_pool_deallocate(&handle_, block); }

    // ---- query -------------------------------------------------------------

    /// @brief Number of blocks currently available.
    [[nodiscard]] std::size_t available() const noexcept { return osal_memory_pool_available(&handle_); }

    /// @brief Block size in bytes.
    [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }

    /// @brief Returns true if the pool was successfully created.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    bool                                        valid_;
    std::size_t                                 block_size_;
    mutable active_traits::memory_pool_handle_t handle_;
};

/// @} // osal_memory_pool

}  // namespace osal
