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
#include "detail/cpp_compat.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#include <expected>
#endif

extern "C"
{
    /// @brief Create a fixed-size block pool.
    /// @param[out] handle      Output handle.
    /// @param[in]  buffer      Caller-supplied backing storage.
    /// @param[in]  buf_bytes   Total size of buffer in bytes.
    /// @param[in]  block_size  Size of each block in bytes.
    /// @param[in]  block_count Number of blocks.
    /// @param[in]  name        Optional debug name; may be nullptr.
    /// @return The backend result; consult its error code if creation fails.
    /// @details The buffer must remain alive and suitably aligned for the pool
    ///          until it is destroyed; arguments must satisfy backend requirements.
    osal::result osal_memory_pool_create(osal::active_traits::memory_pool_handle_t* handle, void* buffer,
                                         std::size_t buf_bytes, std::size_t block_size, std::size_t block_count,
                                         const char* name) noexcept;

    /// @brief Destroy a previously created memory pool.
    /// @param[in,out] handle Handle to destroy.
    /// @return The backend result; consult its error code if destruction fails.
    osal::result osal_memory_pool_destroy(osal::active_traits::memory_pool_handle_t* handle) noexcept;

    /// @brief Allocate one block (non-blocking).
    /// @param[in,out] handle Initialised pool handle.
    /// @return Pointer to the block, or nullptr if the pool is exhausted.
    void* osal_memory_pool_allocate(osal::active_traits::memory_pool_handle_t* handle) noexcept;

    /// @brief Allocate one block with timeout.
    /// @param[in,out] handle        Initialised pool handle.
    /// @param[in]     timeout_ticks Maximum wait in ticks; WAIT_FOREVER waits indefinitely.
    /// @return Pointer to the block, or nullptr on timeout / pool exhausted.
    void* osal_memory_pool_allocate_timed(osal::active_traits::memory_pool_handle_t* handle,
                                          osal::tick_t                               timeout_ticks) noexcept;

    /// @brief Free a previously allocated block back to the pool.
    /// @param[in,out] handle Initialised pool handle.
    /// @param[in]     block  Block previously allocated from this pool.
    /// @return The backend result; consult its error code if the block cannot be returned.
    osal::result osal_memory_pool_deallocate(osal::active_traits::memory_pool_handle_t* handle, void* block) noexcept;

    /// @brief Number of blocks currently available.
    /// @param[in] handle Initialised pool handle.
    /// @return Current available-block count.
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
    void*       buffer      = nullptr;  ///< Caller-supplied backing storage, kept alive by the caller.
    std::size_t buf_bytes   = 0;        ///< Total size of the backing storage in bytes.
    std::size_t block_size  = 0;        ///< Size of each fixed-size block in bytes.
    std::size_t block_count = 0;        ///< Number of blocks created in the storage.
    const char* name        = nullptr;  ///< Optional debug name; may be nullptr.
};

/// @brief OSAL memory pool — fixed-size block allocator.
///
/// @details Allocates and frees equal-sized blocks from a caller-supplied
///          buffer with O(1) time, zero fragmentation, and no heap usage.
///
///          On RTOS backends with native block pools, the native implementation
///          is used.  Otherwise a portable bitmap-based allocator is provided.
///          No ISR-specific allocation contract is provided; follow the active
///          backend's restrictions.
class memory_pool
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs a memory pool over caller-supplied storage.
    /// @param[in] buffer      Caller-supplied backing storage, kept alive for the pool lifetime.
    /// @param[in] buf_bytes   Size of the buffer in bytes.
    /// @param[in] block_size  Size of each block in bytes.
    /// @param[in] block_count Number of blocks.
    /// @param[in] name        Optional debug name; may be nullptr.
    /// @details Storage must be suitably aligned and large enough for the
    ///          requested blocks according to backend requirements. Check valid()
    ///          before using the pool.
    memory_pool(void* buffer, std::size_t buf_bytes, std::size_t block_size, std::size_t block_count,
                const char* name = nullptr) noexcept
        : valid_(false), block_size_(block_size)
    {
        valid_ = osal_memory_pool_create(&handle_, buffer, buf_bytes, block_size, block_count, name).ok();
    }

    /// @brief Constructs from an immutable configuration descriptor.
    /// @param[in] cfg Configuration — typically declared @c const.
    /// @complexity O(1)
    /// @details The descriptor may reside in read-only memory, but its backing
    ///          buffer must remain writable and alive until pool destruction.
    explicit memory_pool(const memory_pool_config& cfg) noexcept : valid_(false), block_size_(cfg.block_size)
    {
        valid_ = osal_memory_pool_create(&handle_, cfg.buffer, cfg.buf_bytes, cfg.block_size, cfg.block_count, cfg.name)
                     .ok();
    }

    /// @brief Destroys the pool if it was initialized.
    /// @details No allocated blocks may still be in use, and no concurrent
    ///          operations may remain; backing storage remains caller-owned.
    ~memory_pool() noexcept
    {
        if (valid_)
        {
            (void)osal_memory_pool_destroy(&handle_);
            valid_ = false;
        }
    }

    /// @brief Copy construction is disabled; the pool and backing storage have one owner.
    memory_pool(const memory_pool&) = delete;
    /// @brief Copy assignment is disabled; the pool and backing storage have one owner.
    memory_pool& operator=(const memory_pool&) = delete;
    /// @brief Move construction is disabled; the backend pool handle is not transferable.
    memory_pool(memory_pool&&) = delete;
    /// @brief Move assignment is disabled; the backend pool handle is not transferable.
    memory_pool& operator=(memory_pool&&) = delete;

    // ---- allocate / deallocate ---------------------------------------------

    /// @brief Allocate one block (non-blocking).
    /// @return Pointer to a block, or nullptr if allocation fails or the pool
    ///         is not initialized.
    /// @details Never waits. The caller owns the block until deallocate().
    [[nodiscard]] void* allocate() noexcept { return osal_memory_pool_allocate(&handle_); }

    /// @brief Allocate one block, waiting up to a timeout where supported.
    /// @param[in] timeout Maximum wait; a non-positive duration does not wait.
    ///                    Backend timed-allocation support determines whether waiting is available.
    /// @return Pointer to a block, or nullptr if unavailable, timed out, or uninitialized.
    /// @details The returned block belongs to the caller until deallocate().
    [[nodiscard]] void* allocate_for(milliseconds timeout) noexcept
    {
        const tick_t ticks = clock_utils::ms_to_ticks(timeout);
        return osal_memory_pool_allocate_timed(&handle_, ticks);
    }

    /// @brief Return a previously allocated block to the pool.
    /// @param[in] block Pointer previously returned by this pool's allocate().
    /// @return Success or the backend result for an invalid/unreturnable block.
    /// @details The block must not still be in use by the caller or another owner.
    [[nodiscard]] result deallocate(void* block) noexcept { return osal_memory_pool_deallocate(&handle_, block); }

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
    /// @brief Non-blocking allocation returning a value-or-error result (C++23).
    /// @retval error_code::not_initialized  The pool was not successfully created.
    /// @retval error_code::out_of_resources The initialized pool returned no block.
    /// @note The pointer-returning backend cannot provide a more specific allocation error.
    [[nodiscard]] std::expected<void*, error_code> allocate_expected() noexcept
    {
        if (!valid())
        {
            return std::unexpected(error_code::not_initialized);
        }
        void* blk = allocate();
        if (blk != nullptr)
        {
            return blk;
        }
        return std::unexpected(error_code::out_of_resources);
    }
#endif

    // ---- query -------------------------------------------------------------

    /// @brief Number of blocks currently available.
    /// @return Current available block count; may be zero when uninitialized.
    [[nodiscard]] std::size_t available() const noexcept { return osal_memory_pool_available(&handle_); }

    /// @brief Block size in bytes.
    /// @return Configured block size.
    [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }

    /// @brief Returns true if the pool was successfully created.
    /// @return true if construction initialized the pool successfully.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    bool                                        valid_;
    std::size_t                                 block_size_;
    mutable active_traits::memory_pool_handle_t handle_{};
};

/// @} // osal_memory_pool

}  // namespace osal
