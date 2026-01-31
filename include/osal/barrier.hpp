// SPDX-License-Identifier: Apache-2.0
/// @file barrier.hpp
/// @brief OSAL thread barrier — rendezvous / synchronisation point
/// @details Provides osal::barrier, which blocks a configurable number of threads
///          until all of them have called wait().  On the final (N-th) call,
///          all blocked threads are released simultaneously.
///
///          The last (N-th) arriving thread receives
///          error_code::barrier_serial (mirroring POSIX PTHREAD_BARRIER_SERIAL_THREAD)
///          so it can act as the "serial" thread responsible for resetting shared
///          state; all other threads receive result::ok().
///
///          Availability:
///          - Natively supported on POSIX, Linux, NuttX, QNX, RTEMS, and
///            INTEGRITY (pthread_barrier_t).
///          - On all other backends wait() returns error_code::not_supported.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_barrier
#pragma once

#include "backends.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstdint>

extern "C"
{
    osal::result osal_barrier_create(osal::active_traits::barrier_handle_t* handle, unsigned count) noexcept;
    osal::result osal_barrier_destroy(osal::active_traits::barrier_handle_t* handle) noexcept;

    /// @brief Blocks until @a count threads have called osal_barrier_wait().
    /// @return result::ok() for the first N-1 threads;
    ///         error_code::barrier_serial for the last thread (the "serial" thread
    ///         conventionally responsible for resetting shared state before others proceed).
    osal::result osal_barrier_wait(osal::active_traits::barrier_handle_t* handle) noexcept;
}  // extern "C"

namespace osal
{

/// @defgroup osal_barrier OSAL Barrier
/// @brief Thread rendezvous / synchronisation point.
/// @{

/// @brief OSAL thread barrier.
/// @details Blocks each calling thread until exactly @a count threads have
///          called wait().  The last (N-th) thread to arrive receives a
///          barrier_serial result; all others receive ok().
///
/// @note  Non-copyable, non-movable; no dynamic allocation; OS handle stored inline.
class barrier
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the barrier.
    /// @param count  Number of threads that must call wait() before any
    ///               are released.  Must be >= 1.
    /// @complexity O(1)
    /// @blocking   Never.
    explicit barrier(unsigned count) noexcept : valid_(false), handle_{}
    {
        if constexpr (active_capabilities::has_barrier)
        {
            valid_ = osal_barrier_create(&handle_, count).ok();
        }
    }

    /// @brief Destructor.
    ~barrier() noexcept
    {
        if constexpr (active_capabilities::has_barrier)
        {
            if (valid_)
            {
                (void)osal_barrier_destroy(&handle_);
                valid_ = false;
            }
        }
    }

    barrier(const barrier&)            = delete;
    barrier& operator=(const barrier&) = delete;
    barrier(barrier&&)                 = delete;
    barrier& operator=(barrier&&)      = delete;

    // ---- operations --------------------------------------------------------

    /// @brief Waits until all @a count threads have arrived at this barrier.
    /// @return result::ok() for the first N-1 threads;
    ///         result(error_code::barrier_serial) for the last thread (the
    ///         "serial" thread — conventionally responsible for resetting shared
    ///         state before others proceed);
    ///         error_code::not_supported on backends without native barrier support.
    /// @complexity O(1)
    /// @blocking   Until all participants arrive (or not_supported).
    result wait() noexcept
    {
        if constexpr (active_capabilities::has_barrier)
        {
            return osal_barrier_wait(&handle_);
        }
        else
        {
            return error_code::not_supported;
        }
    }

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the barrier was successfully initialised.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    bool                            valid_;
    active_traits::barrier_handle_t handle_;
};

/// @} // osal_barrier

}  // namespace osal
