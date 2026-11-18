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
    /// @brief Create a read-write lock.
    /// @param[out] handle Handle storage to initialise.
    /// @return The backend result; consult its error code if creation fails.
    osal::result osal_rwlock_create(osal::active_traits::rwlock_handle_t* handle) noexcept;

    /// @brief Destroy a read-write lock.
    /// @param[in,out] handle Handle to destroy.
    /// @return The backend result; consult its error code if destruction fails.
    osal::result osal_rwlock_destroy(osal::active_traits::rwlock_handle_t* handle) noexcept;

    /// @brief Acquire the lock for reading (shared).
    /// @param[in,out] handle        Initialised lock handle.
    /// @param[in]     timeout_ticks Maximum wait in ticks; WAIT_FOREVER waits indefinitely.
    /// @return The backend result; consult its error code if acquisition fails.
    osal::result osal_rwlock_read_lock(osal::active_traits::rwlock_handle_t* handle,
                                       osal::tick_t                          timeout_ticks) noexcept;

    /// @brief Release a read lock.
    /// @param[in,out] handle Initialised lock handle held for reading by this caller.
    /// @return The backend result; consult its error code if release fails.
    osal::result osal_rwlock_read_unlock(osal::active_traits::rwlock_handle_t* handle) noexcept;

    /// @brief Acquire the lock for writing (exclusive).
    /// @param[in,out] handle        Initialised lock handle.
    /// @param[in]     timeout_ticks Maximum wait in ticks; WAIT_FOREVER waits indefinitely.
    /// @return The backend result; consult its error code if acquisition fails.
    osal::result osal_rwlock_write_lock(osal::active_traits::rwlock_handle_t* handle,
                                        osal::tick_t                          timeout_ticks) noexcept;

    /// @brief Release a write lock.
    /// @param[in,out] handle Initialised lock handle held for writing by this caller.
    /// @return The backend result; consult its error code if release fails.
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

    /// @brief Constructs and initializes a read-write lock.
    /// @details Check valid() before using it.
    rwlock() noexcept = default;  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

    /// @brief Destroys the lock if initialization succeeded.
    /// @details All readers and writers must have released the lock before its
    ///          lifetime ends.
    ~rwlock() noexcept
    {
        if (valid_)
        {
            (void)osal_rwlock_destroy(&handle_);
            valid_ = false;
        }
    }

    /// @brief Copy construction is disabled; the backend handle has one owner.
    rwlock(const rwlock&) = delete;
    /// @brief Copy assignment is disabled; the backend handle has one owner.
    rwlock& operator=(const rwlock&) = delete;
    /// @brief Move construction is disabled; the backend handle is not transferable.
    rwlock(rwlock&&) = delete;
    /// @brief Move assignment is disabled; the backend handle is not transferable.
    rwlock& operator=(rwlock&&) = delete;

    // ---- read (shared) lock ------------------------------------------------

    /// @brief Acquire the lock for reading (blocking).
    /// @return Success or the backend error result.
    /// @details Thread-context operation; successful acquisition must be paired
    ///          with read_unlock() by the owning thread.
    [[nodiscard]] result read_lock() noexcept { return osal_rwlock_read_lock(&handle_, WAIT_FOREVER); }

    /// @brief Try to acquire the read lock with a timeout.
    /// @param[in] timeout Maximum wait; non-positive values do not wait.
    /// @return Success or the backend error result, including timeout where reported.
    /// @details Successful acquisition must be paired with read_unlock() by the
    ///          owning thread.
    result read_lock_for(milliseconds timeout) noexcept
    {
        const tick_t ticks = clock_utils::ms_to_ticks(timeout);
        return osal_rwlock_read_lock(&handle_, ticks);
    }

    /// @brief Release the read lock.
    /// @return Success or the backend error result.
    [[nodiscard]] result read_unlock() noexcept { return osal_rwlock_read_unlock(&handle_); }

    // ---- write (exclusive) lock --------------------------------------------

    /// @brief Acquire the lock for writing (blocking).
    /// @return Success or the backend error result.
    /// @details Thread-context operation; successful acquisition must be paired
    ///          with write_unlock() by the owning thread.
    [[nodiscard]] result write_lock() noexcept { return osal_rwlock_write_lock(&handle_, WAIT_FOREVER); }

    /// @brief Try to acquire the write lock with a timeout.
    /// @param[in] timeout Maximum wait; non-positive values do not wait.
    /// @return Success or the backend error result, including timeout where reported.
    /// @details Successful acquisition must be paired with write_unlock() by the
    ///          owning thread.
    result write_lock_for(milliseconds timeout) noexcept
    {
        const tick_t ticks = clock_utils::ms_to_ticks(timeout);
        return osal_rwlock_write_lock(&handle_, ticks);
    }

    /// @brief Release the write lock.
    /// @return Success or the backend error result.
    [[nodiscard]] result write_unlock() noexcept { return osal_rwlock_write_unlock(&handle_); }

    // ---- RAII guards -------------------------------------------------------

    /// @brief RAII guard for read (shared) access.
    /// @details Holds a reference to @p rw, attempts to acquire its read lock at
    ///          construction, and attempts to release it at destruction. The
    ///          lock must outlive the guard. Acquisition errors are not exposed.
    class read_guard
    {
    public:
        /// @brief Constructs the guard for the specified lock.
        /// @param[in,out] rw Lock to acquire and release.
        explicit read_guard(rwlock& rw) noexcept : rw_(rw) { (void)rw_.read_lock(); }
        /// @brief Attempts to release the read lock.
        ~read_guard() noexcept { (void)rw_.read_unlock(); }
        /// @brief Copy construction is disabled to prevent duplicate unlocks.
        read_guard(const read_guard&) = delete;
        /// @brief Copy assignment is disabled to prevent duplicate unlocks.
        read_guard& operator=(const read_guard&) = delete;
        /// @brief Move construction is disabled to preserve guard ownership.
        read_guard(read_guard&&) = delete;
        /// @brief Move assignment is disabled to preserve guard ownership.
        read_guard& operator=(read_guard&&) = delete;

    private:
        rwlock& rw_;
    };

    /// @brief RAII guard for write (exclusive) access.
    /// @details Holds a reference to @p rw, attempts to acquire its write lock
    ///          at construction, and attempts to release it at destruction.
    ///          The lock must outlive the guard. Acquisition errors are not exposed.
    class write_guard
    {
    public:
        /// @brief Constructs the guard for the specified lock.
        /// @param[in,out] rw Lock to acquire and release.
        explicit write_guard(rwlock& rw) noexcept : rw_(rw) { (void)rw_.write_lock(); }
        /// @brief Attempts to release the write lock.
        ~write_guard() noexcept { (void)rw_.write_unlock(); }
        /// @brief Copy construction is disabled to prevent duplicate unlocks.
        write_guard(const write_guard&) = delete;
        /// @brief Copy assignment is disabled to prevent duplicate unlocks.
        write_guard& operator=(const write_guard&) = delete;
        /// @brief Move construction is disabled to preserve guard ownership.
        write_guard(write_guard&&) = delete;
        /// @brief Move assignment is disabled to preserve guard ownership.
        write_guard& operator=(write_guard&&) = delete;

    private:
        rwlock& rw_;
    };

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the rwlock was successfully created.
    /// @return true if construction initialized the lock successfully.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    active_traits::rwlock_handle_t handle_{};
    bool                           valid_{osal_rwlock_create(&handle_).ok()};
};

/// @} // osal_rwlock

}  // namespace osal
