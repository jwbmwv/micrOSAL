// SPDX-License-Identifier: Apache-2.0
/// @file condvar.hpp
/// @brief OSAL condition variable — thread synchronisation primitive
/// @details Provides osal::condvar for blocking threads until a condition is
///          signalled.  On backends with native condition variables (POSIX,
///          Linux, NuttX, QNX, RTEMS, INTEGRITY, Zephyr, ChibiOS, VxWorks) the
///          native primitive is used.  On all other backends, an emulated
///          implementation using OSAL mutex + semaphore is provided
///          automatically.
///
///          Usage:
///          @code
///          osal::mutex m;
///          osal::condvar cv;
///
///          // Producer thread:
///          {
///              osal::mutex::lock_guard lg{m};
///              data_ready = true;
///          }
///          cv.notify_one();
///
///          // Consumer thread:
///          {
///              osal::mutex::lock_guard lg{m};
///              while (!data_ready) {
///                  cv.wait(m);
///              }
///          }
///          @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_condvar
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "mutex.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>

extern "C"
{
    /// @brief Create a condition variable.
    /// @param[out] handle Handle storage to initialise.
    /// @return The backend result; consult its error code if creation fails.
    osal::result osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept;

    /// @brief Destroy a condition variable.
    /// @param[in,out] handle Handle to destroy.
    /// @return The backend result; consult its error code if destruction fails.
    osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t* handle) noexcept;

    /// @brief Atomically unlock @p mutex and block until signalled, then re-lock.
    /// @param[in,out] handle        Initialised condition-variable handle.
    /// @param[in,out] mutex         Initialised mutex held by the calling thread.
    /// @param[in]     timeout_ticks Maximum wait in ticks; WAIT_FOREVER waits indefinitely.
    /// @return The backend result, including any backend-specific wait failure.
    osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t* handle,
                                   osal::active_traits::mutex_handle_t* mutex, osal::tick_t timeout_ticks) noexcept;

    /// @brief Wake one waiting thread.
    /// @param[in,out] handle Initialised condition-variable handle.
    /// @return The backend result; consult its error code if notification fails.
    osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t* handle) noexcept;

    /// @brief Wake all waiting threads.
    /// @param[in,out] handle Initialised condition-variable handle.
    /// @return The backend result; consult its error code if notification fails.
    osal::result osal_condvar_notify_all(osal::active_traits::condvar_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_condvar OSAL Condition Variable
/// @brief Condition variable for producer/consumer and state-change patterns.
/// @{

/// @brief OSAL condition variable — wait/notify synchronisation primitive.
///
/// @details On backends with native condvar support the native primitive is
///          used.  On all other backends an emulated implementation is
///          provided using OSAL mutex + counting semaphore.
///
///          The caller is responsible for holding an osal::mutex when calling
///          wait() / wait_for().  The mutex is atomically released during the
///          wait and re-acquired before the call returns.
class condvar  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initializes a condition variable.
    /// @details Check valid() before relying on the condition variable.
    condvar() noexcept = default;  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

    /// @brief Destroys the condition variable if initialization succeeded.
    /// @details No threads may be waiting on it when its lifetime ends.
    ~condvar() noexcept
    {
        if (valid_)
        {
            (void)osal_condvar_destroy(&handle_);
            valid_ = false;
        }
    }

    /// @brief Copy construction is disabled; the native handle has one owner.
    condvar(const condvar&) = delete;
    /// @brief Copy assignment is disabled; the native handle has one owner.
    condvar& operator=(const condvar&) = delete;
    /// @brief Move construction is disabled; the native handle is not transferable.
    condvar(condvar&&) = delete;
    /// @brief Move assignment is disabled; the native handle is not transferable.
    condvar& operator=(condvar&&) = delete;

    // ---- wait --------------------------------------------------------------

    /// @brief Atomically unlock @p m and block until notified, then re-lock.
    /// @param[in,out] m The mutex held by the calling thread; it is released
    ///                  while waiting and reacquired before return.
    /// @details Thread context only. The mutex and condition variable must
    ///          remain alive for the duration of the wait.
    void wait(mutex& m) noexcept { (void)osal_condvar_wait(&handle_, m.native_handle(), WAIT_FOREVER); }

    /// @brief Waits until @p pred returns true, re-checking after each wakeup.
    /// @details Provides built-in protection against spurious wakeups.
    ///          Equivalent to: @code while (!pred()) cv.wait(m); @endcode
    /// @param[in,out] m    The mutex held by the calling thread; released while
    ///                     waiting and reacquired before return.
    /// @param[in]     pred Callable with signature @c bool(), evaluated under @p m.
    /// @tparam Predicate   A predicate satisfying @c wait_predicate.
    /// @details The condition variable and mutex must outlive the wait. Thread
    ///          context only; a non-positive duration checks the predicate once.
    ///          The predicate is re-evaluated after each wake.
    template<wait_predicate Predicate>
    void wait(mutex& m, Predicate pred) noexcept(noexcept(pred()))
    {
        while (!pred())
        {
            wait(m);
        }
    }

    /// @brief Timed wait — block until notified or @p timeout expires.
    /// @param[in,out] m       The mutex held by the calling thread; released
    ///                        while waiting and reacquired before return.
    /// @param[in]     timeout Maximum wait; a non-positive duration does not wait.
    /// @return true if the backend wait succeeds; false if it reports timeout
    ///         or another failure.
    /// @details Thread context only; the mutex and condition variable must
    ///          remain alive for the duration of the wait. There is no ISR
    ///          notification overload or ISR-safety guarantee.
    bool wait_for(mutex& m, milliseconds timeout) noexcept
    {
        const tick_t ticks = clock_utils::ms_to_ticks(timeout);
        return osal_condvar_wait(&handle_, m.native_handle(), ticks).ok();
    }

    /// @brief Timed wait with predicate — returns true if @p pred is satisfied.
    /// @details Loops until @p pred is true or the total deadline is reached.
    ///          Protects against spurious wakeups.
    /// @param[in,out] m       The mutex held by the calling thread; released
    ///                        while waiting and reacquired before return.
    /// @param[in]     timeout Maximum total wait; a non-positive duration checks the predicate once.
    /// @param[in]     pred    Callable with signature @c bool(), evaluated under @p m.
    /// @tparam Predicate       A predicate satisfying @c wait_predicate.
    /// @return true if @p pred was satisfied; false if the timeout or a condition-variable wait failed.
    template<wait_predicate Predicate>
    bool wait_for(mutex& m, milliseconds timeout, Predicate pred) noexcept(noexcept(pred()))
    {
        const monotonic_deadline wait_deadline = monotonic_deadline::after(timeout);
        while (!pred())
        {
            if (wait_deadline.expired())
            {
                return pred();
            }
            if (!wait_for(m, wait_deadline.remaining()))
            {
                return pred();
            }
        }
        return true;
    }

    /// @brief Waits until an absolute deadline.
    /// @param[in,out] m        The mutex held by the calling thread; released
    ///                         while waiting and reacquired before return.
    /// @param[in]     deadline Absolute monotonic time point.
    /// @return true if notified before the deadline; false on timeout.
    /// @details Thread context only; the mutex and condition variable must
    ///          remain alive for the duration of the wait. Loops internally so
    ///          that tick-count saturation (for unreasonably
    ///          large deadlines) does not cause premature false-timeout returns.
    bool wait_until(mutex& m, monotonic_clock::time_point deadline) noexcept
    {
        const monotonic_deadline wait_deadline = monotonic_deadline::at(deadline);
        for (;;)
        {
            if (wait_deadline.expired())
            {
                return false;
            }
            if (wait_for(m, wait_deadline.remaining()))
            {
                return true;  // notified
            }
            // Timed out: either the real deadline passed (checked at top of loop)
            // or ms_to_ticks saturated a >49-day remaining into a ~49-day chunk.
            // Either way, re-evaluate the deadline.
        }
    }

    /// @brief Waits until an absolute deadline with predicate.
    /// @param[in,out] m        The mutex held by the calling thread; released
    ///                         while waiting and reacquired before return.
    /// @param[in]     deadline Absolute monotonic time point.
    /// @param[in]     pred     Callable with signature @c bool(), evaluated under @p m.
    /// @tparam Predicate        A predicate satisfying @c wait_predicate.
    /// @return true if @p pred is true when checked; false when the relative wait
    ///         derived from @p deadline expires.
    /// @details The implementation uses the remaining duration at entry and
    ///          delegates to wait_for(); the mutex and condition variable must
    ///          outlive the call. Thread context only.
    template<wait_predicate Predicate>
    bool wait_until(mutex& m, monotonic_clock::time_point deadline, Predicate pred) noexcept(noexcept(pred()))
    {
        // Delegate to the timed predicate wait_for, which already loops.
        return wait_for(m, monotonic_deadline::at(deadline).remaining(), pred);
    }

    // ---- notify ------------------------------------------------------------

    /// @brief Wake one thread waiting on this condition variable.
    /// @details May be called from thread context to notify a waiter. A
    ///          notification is not retained when no thread is waiting.
    void notify_one() noexcept { (void)osal_condvar_notify_one(&handle_); }

    /// @brief Wake all threads waiting on this condition variable.
    /// @details May be called from thread context to notify all current waiters.
    void notify_all() noexcept { (void)osal_condvar_notify_all(&handle_); }

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the condition variable was successfully created.
    /// @return true if backend initialization succeeded.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    active_traits::condvar_handle_t handle_{};
    bool                            valid_{osal_condvar_create(&handle_).ok()};
};

/// @} // osal_condvar

}  // namespace osal
