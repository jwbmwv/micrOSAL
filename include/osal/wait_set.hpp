// SPDX-License-Identifier: Apache-2.0
/// @file wait_set.hpp
/// @brief OSAL wait-set — poll/select/epoll abstraction
/// @details Provides osal::wait_set for simultaneously monitoring multiple
///          OS objects.  On Linux it maps to epoll().  On POSIX-family
///          backends (POSIX, RTEMS, INTEGRITY) it maps to poll().  On PX5 it
///          uses the native PX5 wait-set.  Unsupported backends expose a stub
///          object whose add/remove/wait operations return
///          error_code::not_supported. For a hard compile-time requirement,
///          call osal::wait_set::require_support(). Prefer osal::object_wait_set
///          when waiting on portable OSAL objects rather than native descriptors.
///
///          Limitations:
///          - Maximum number of monitored objects is OSAL_WAIT_SET_MAX_ENTRIES
///            (default 16, override via CMake -DOSAL_WAIT_SET_MAX_ENTRIES=N).
///
///          MISRA note: no virtual dispatch; backend-specific code paths use
///          if constexpr rather than inheritance.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_wait_set
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstdint>
#include <cstddef>
#include <span>

#ifndef OSAL_WAIT_SET_MAX_ENTRIES
/// @brief Maximum number of objects that can be monitored in a single wait_set.
#define OSAL_WAIT_SET_MAX_ENTRIES 16U
#endif

extern "C"
{
    /// @brief Create a backend wait-set.
    /// @param[out] handle Handle storage to initialise.
    /// @return The backend result; consult its error code if creation fails.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t* handle) noexcept;

    /// @brief Destroy a backend wait-set.
    /// @param[in,out] handle Handle to destroy.
    /// @return The backend result; consult its error code if destruction fails.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t* handle) noexcept;

    /// @brief Add a native object to the backend wait-set.
    /// @param[in,out] handle   Initialised wait-set handle.
    /// @param[in]     fd_or_id Native file descriptor or backend object identifier.
    /// @param[in]     events   Readiness event bitmask.
    /// @return The backend result; consult its error code if the object cannot be added.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t* handle, int fd_or_id,
                                   std::uint32_t events) noexcept;

    /// @brief Remove a native object from the backend wait-set.
    /// @param[in,out] handle   Initialised wait-set handle.
    /// @param[in]     fd_or_id Native file descriptor or backend object identifier.
    /// @return The backend result; consult its error code if the object cannot be removed.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t* handle, int fd_or_id) noexcept;

    /// @brief Waits until at least one monitored object is ready.
    /// @param[in,out] handle    Initialised wait-set handle.
    /// @param[out]    fds_ready Caller-allocated array receiving ready identifiers.
    /// @param[in]     max_ready Capacity of @p fds_ready.
    /// @param[out]    n_ready   Receives the number of identifiers written.
    /// @param[in]     timeout   Maximum wait in ticks; WAIT_FOREVER waits indefinitely.
    /// @return The backend result; consult its error code for timeout or other failures.
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t* handle, int* fds_ready,
                                    std::size_t max_ready, std::size_t* n_ready, osal::tick_t timeout) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_wait_set OSAL Wait-Set
/// @brief Multiplex waits across multiple OS objects.
/// @{

/// @brief Wait flags for wait_set entries.
namespace wait_events
{
static constexpr std::uint32_t readable = 0x01U;  ///< Object has data / is signalled.
static constexpr std::uint32_t writable = 0x02U;  ///< Object can accept data.
static constexpr std::uint32_t error    = 0x04U;  ///< Error condition.
}  // namespace wait_events

/// @brief OSAL wait-set.
/// @details Monitors up to OSAL_WAIT_SET_MAX_ENTRIES objects simultaneously.
///          On unsupported backends the object still constructs successfully
///          so generic code can instantiate it unconditionally, but
///          add/remove/wait return error_code::not_supported.
class wait_set
{
public:
    /// @brief True when the active backend exposes a native wait-set primitive.
    static constexpr bool is_supported = supports_requirement<support_requirement::wait_set>;

    /// @brief Enforce native wait-set support at compile time.
    /// @tparam Backend Backend whose wait-set requirement is checked.
    /// @details Compilation fails if @p Backend does not provide wait-set support.
    template<typename Backend = active_backend>
    static consteval void require_support()
    {
        require_backend_support<support_requirement::wait_set, Backend>();
    }

    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the wait-set.
    /// @details On unsupported backends construction creates a valid stub; use
    ///          is_supported or require_support() to check capability.
    wait_set() noexcept
    {
        if constexpr (wait_set_backend<active_backend>)
        {
            valid_ = osal_wait_set_create(&handle_).ok();
        }
        else
        {
            // Stub mode: construction succeeds so generic code can keep a
            // wait_set member even when multiplexing is unavailable.
            valid_ = true;
        }
    }

    /// @brief Destroys the backend wait-set when one was initialized.
    /// @details All concurrent wait/add/remove operations must have ended.
    ~wait_set() noexcept
    {
        if (valid_)
        {
            if constexpr (wait_set_backend<active_backend>)
            {
                (void)osal_wait_set_destroy(&handle_);
            }
            valid_ = false;
        }
    }

    /// @brief Copy construction is disabled; the wait-set handle has one owner.
    wait_set(const wait_set&) = delete;
    /// @brief Copy assignment is disabled; the wait-set handle has one owner.
    wait_set& operator=(const wait_set&) = delete;
    /// @brief Move construction is disabled; the wait-set handle is not transferable.
    wait_set(wait_set&&) = delete;
    /// @brief Move assignment is disabled; the wait-set handle is not transferable.
    wait_set& operator=(wait_set&&) = delete;

    // ---- membership --------------------------------------------------------

    /// @brief Adds an object to the wait-set.
    /// @param[in] fd_or_id File descriptor (POSIX/Linux) or object ID.
    /// @param[in] events   Bitmask from osal::wait_events.
    /// @retval error_code::not_supported The active backend has no native wait-set support.
    /// @par Other errors
    /// Backend errors are returned unchanged.
    /// @details The identifier must refer to an object valid for the backend;
    ///          event support and identifier semantics are backend-specific.
    ///          Do not assume add/remove are safe concurrently with wait().
    /// @retval osal::ok()                The object was added successfully.
    /// @retval error_code::overflow      The wait-set is full.
    result add(int fd_or_id, std::uint32_t events = wait_events::readable) noexcept
    {
        if constexpr (wait_set_backend<active_backend>)
        {
            return osal_wait_set_add(&handle_, fd_or_id, events);
        }
        (void)fd_or_id;
        (void)events;
        return error_code::not_supported;
    }

    /// @brief Removes an object from the wait-set.
    /// @param[in] fd_or_id Object to remove.
    /// @retval error_code::not_supported The active backend has no native wait-set support.
    /// @par Other errors
    /// Backend errors are returned unchanged.
    result remove(int fd_or_id) noexcept
    {
        if constexpr (wait_set_backend<active_backend>)
        {
            return osal_wait_set_remove(&handle_, fd_or_id);
        }
        (void)fd_or_id;
        return error_code::not_supported;
    }

    // ---- wait --------------------------------------------------------------

    /// @brief Blocks until at least one monitored object is ready.
    /// @param[out] ready_ids Caller-supplied buffer for ready IDs.
    /// @param[in]  max_ready Capacity of ready_ids.
    /// @param[out] n_ready   Number of ready IDs written on success.
    /// @param[in]  timeout   Maximum wait time (negative = forever).
    /// @retval osal::ok()                At least one object is ready.
    /// @retval error_code::timeout       The wait expired.
    /// @retval error_code::not_supported The active backend has no native wait-set support.
    /// @par Other errors
    /// Backend errors are returned unchanged.
    /// @details Results are readiness snapshots; the caller owns the monitored
    ///          objects and output storage. This operation is thread-context-only;
    ///          zero timeout polls once and negative timeout waits indefinitely.
    /// @complexity O(n) where n is number of monitored objects.
    /// @blocking   Potentially blocking.
    result wait(int* ready_ids, std::size_t max_ready, std::size_t& n_ready,  // NOLINT(readability-non-const-parameter)
                milliseconds timeout = milliseconds{-1}) noexcept
    {
        if constexpr (wait_set_backend<active_backend>)
        {
            const tick_t ticks = (timeout.count() < 0) ? WAIT_FOREVER : clock_utils::ms_to_ticks(timeout);
            return osal_wait_set_wait(&handle_, ready_ids, max_ready, &n_ready, ticks);
        }
        (void)ready_ids;
        (void)max_ready;
        (void)timeout;
        n_ready = 0U;
        return error_code::not_supported;
    }

    /// @brief Wait using a span as the caller-owned ready-ID buffer.
    /// @param[out] ready_ids Caller-owned output span.
    /// @param[out] n_ready   Receives the number of identifiers written.
    /// @param[in]  timeout   Maximum wait; a negative duration waits indefinitely.
    /// @return Same result and error contract as the pointer-based wait overload.
    result wait(std::span<int> ready_ids, std::size_t& n_ready, milliseconds timeout = milliseconds{-1}) noexcept
    {
        return wait(ready_ids.data(), ready_ids.size(), n_ready, timeout);
    }

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the wait-set was successfully initialised.
    /// @return true when native initialization succeeded or the unsupported stub is active.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    bool                             valid_{false};
    active_traits::wait_set_handle_t handle_{};
};

/// @} // osal_wait_set

}  // namespace osal
