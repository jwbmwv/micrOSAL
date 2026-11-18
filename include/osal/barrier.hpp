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
///          - All other current backends use the shared emulated barrier built
///            from osal::mutex + osal::condvar.
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
    /// @brief Create a barrier for a fixed number of participants.
    /// @param[out] handle Handle storage to initialise.
    /// @param[in]  count  Required number of participants; must be nonzero.
    /// @return The backend result; consult its error code if creation fails.
    osal::result osal_barrier_create(osal::active_traits::barrier_handle_t* handle, unsigned count) noexcept;

    /// @brief Destroy a previously created barrier.
    /// @param[in,out] handle Handle to destroy.
    /// @return The backend result; consult its error code if destruction fails.
    osal::result osal_barrier_destroy(osal::active_traits::barrier_handle_t* handle) noexcept;

    /// @brief Blocks until @a count threads have called osal_barrier_wait().
    /// @param[in,out] handle Initialised barrier handle.
    /// @retval osal::ok()                       Returned to the first N-1 threads.
    /// @retval osal::error_code::barrier_serial Returned to the final serial thread, conventionally
    ///                                          responsible for resetting shared state before others proceed.
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
    /// @param[in] count Number of threads that must call wait() before any
    ///                  are released.  Must be >= 1.
    /// @details The barrier must outlive all participating calls to wait().
    ///          Initialization failure is reported by valid(); wait() on an
    ///          unsupported backend returns not_supported.
    /// @complexity O(1)
    /// @blocking   Never.
    explicit barrier(unsigned count) noexcept
    {
        if constexpr (active_capabilities::has_barrier)
        {
            valid_ = osal_barrier_create(&handle_, count).ok();
        }
    }

    /// @brief Destroys the barrier if initialization succeeded.
    /// @details All participating threads must have stopped using the barrier
    ///          before its lifetime ends.
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

    /// @brief Copy construction is disabled; the barrier handle has one owner.
    barrier(const barrier&) = delete;
    /// @brief Copy assignment is disabled; the barrier handle has one owner.
    barrier& operator=(const barrier&) = delete;
    /// @brief Move construction is disabled; the barrier handle is not transferable.
    barrier(barrier&&) = delete;
    /// @brief Move assignment is disabled; the barrier handle is not transferable.
    barrier& operator=(barrier&&) = delete;

    // ---- operations --------------------------------------------------------

    /// @brief Waits until all @a count threads have arrived at this barrier.
    /// @details Requires a successfully initialized barrier. This is a
    ///          thread-context blocking operation; it has no timeout.
    /// @retval osal::ok()                       Returned to the first N-1 threads.
    /// @retval osal::error_code::barrier_serial Returned to the final serial thread, conventionally
    ///                                          responsible for resetting shared state before others proceed.
    /// @complexity O(1)
    /// @blocking   Until all participants arrive.
    [[nodiscard]] result wait() noexcept
    {
        if constexpr (active_capabilities::has_barrier)
        {
            return osal_barrier_wait(&handle_);
        }
        return error_code::not_supported;
    }

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the barrier was successfully initialised.
    /// @return true if construction created the backend barrier.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    bool                            valid_{false};
    active_traits::barrier_handle_t handle_{};
};

/// @} // osal_barrier

}  // namespace osal
