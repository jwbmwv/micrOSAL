// SPDX-License-Identifier: Apache-2.0
/// @file rwlock.hpp
/// @brief OSAL read-write lock — multiple readers / single writer
/// @details Provides osal::rwlock for shared (read) and exclusive (write)
///          access patterns.  Multiple readers may hold the lock concurrently;
///          a writer requires exclusive access.
///
///          On backends with native rwlock support (POSIX, Linux, NuttX, QNX,
///          RTEMS, INTEGRITY — all using pthread_rwlock_t) the native
///          primitive is used.
///          On all other backends, an emulated implementation using
///          OSAL mutex + condvar + counter is provided automatically.
///
///          Usage:
///          @code
///          osal::rwlock rw;
///
///          // Reader thread:
///          {
///              osal::rwlock::read_guard rg{rw};
///              // read shared data ...
///          }
///
///          // Writer thread:
///          {
///              osal::rwlock::write_guard wg{rw};
///              // modify shared data ...
///          }
///          @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_rwlock
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>

extern "C"
{
    osal::result osal_rwlock_create(osal::active_traits::rwlock_handle_t* handle) noexcept;

    osal::result osal_rwlock_destroy(osal::active_traits::rwlock_handle_t* handle) noexcept;

    /// @brief Acquire the lock for reading (shared).
    osal::result osal_rwlock_read_lock(osal::active_traits::rwlock_handle_t* handle,
                                       osal::tick_t                          timeout_ticks) noexcept;

    /// @brief Release a read lock.
    osal::result osal_rwlock_read_unlock(osal::active_traits::rwlock_handle_t* handle) noexcept;

    /// @brief Acquire the lock for writing (exclusive).
    osal::result osal_rwlock_write_lock(osal::active_traits::rwlock_handle_t* handle,
                                        osal::tick_t                          timeout_ticks) noexcept;

    /// @brief Release a write lock.
    osal::result osal_rwlock_write_unlock(osal::active_traits::rwlock_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_rwlock OSAL Read-Write Lock
/// @brief Multiple-reader / single-writer lock.
/// @{

/// @brief OSAL read-write lock.
///
/// @details Allows concurrent read access and exclusive write access.
///          On POSIX-family backends the native pthread_rwlock_t is used.
///          On all other backends, an emulated implementation is provided
///          using mutex + condvar + reader counter.
class rwlock  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs a read-write lock.
    rwlock() noexcept = default;  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

    /// @brief Destroys the read-write lock.
    ~rwlock() noexcept
    {
        if (valid_)
        {
            (void)osal_rwlock_destroy(&handle_);
            valid_ = false;
        }
    }

    rwlock(const rwlock&)            = delete;
    rwlock& operator=(const rwlock&) = delete;
    rwlock(rwlock&&)                 = delete;
    rwlock& operator=(rwlock&&)      = delete;

    // ---- read (shared) lock ------------------------------------------------

    /// @brief Acquire the lock for reading (blocking).
    [[nodiscard]] result read_lock() noexcept { return osal_rwlock_read_lock(&handle_, WAIT_FOREVER); }

    /// @brief Try to acquire the read lock with a timeout.
    result read_lock_for(milliseconds timeout) noexcept
    {
        const tick_t ticks = clock_utils::ms_to_ticks(timeout);
        return osal_rwlock_read_lock(&handle_, ticks);
    }

    /// @brief Release the read lock.
    [[nodiscard]] result read_unlock() noexcept { return osal_rwlock_read_unlock(&handle_); }

    // ---- write (exclusive) lock --------------------------------------------

    /// @brief Acquire the lock for writing (blocking).
    [[nodiscard]] result write_lock() noexcept { return osal_rwlock_write_lock(&handle_, WAIT_FOREVER); }

    /// @brief Try to acquire the write lock with a timeout.
    result write_lock_for(milliseconds timeout) noexcept
    {
        const tick_t ticks = clock_utils::ms_to_ticks(timeout);
        return osal_rwlock_write_lock(&handle_, ticks);
    }

    /// @brief Release the write lock.
    [[nodiscard]] result write_unlock() noexcept { return osal_rwlock_write_unlock(&handle_); }

    // ---- RAII guards -------------------------------------------------------

    /// @brief RAII guard for read (shared) access.
    class read_guard
    {
    public:
        explicit read_guard(rwlock& rw) noexcept : rw_(rw) { (void)rw_.read_lock(); }
        ~read_guard() noexcept { (void)rw_.read_unlock(); }
        read_guard(const read_guard&)            = delete;
        read_guard& operator=(const read_guard&) = delete;
        read_guard(read_guard&&)                 = delete;
        read_guard& operator=(read_guard&&)      = delete;

    private:
        rwlock& rw_;
    };

    /// @brief RAII guard for write (exclusive) access.
    class write_guard
    {
    public:
        explicit write_guard(rwlock& rw) noexcept : rw_(rw) { (void)rw_.write_lock(); }
        ~write_guard() noexcept { (void)rw_.write_unlock(); }
        write_guard(const write_guard&)            = delete;
        write_guard& operator=(const write_guard&) = delete;
        write_guard(write_guard&&)                 = delete;
        write_guard& operator=(write_guard&&)      = delete;

    private:
        rwlock& rw_;
    };

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the rwlock was successfully created.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    active_traits::rwlock_handle_t handle_{};
    bool                           valid_{osal_rwlock_create(&handle_).ok()};
};

/// @} // osal_rwlock

}  // namespace osal
