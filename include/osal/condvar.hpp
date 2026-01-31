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
    osal::result osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept;

    osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t* handle) noexcept;

    /// @brief Atomically unlock @p mutex and block until signalled, then re-lock.
    osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t* handle,
                                   osal::active_traits::mutex_handle_t* mutex, osal::tick_t timeout_ticks) noexcept;

    /// @brief Wake one waiting thread.
    osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t* handle) noexcept;

    /// @brief Wake all waiting threads.
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
class condvar
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs a condition variable.
    condvar() noexcept : valid_(false), handle_{} { valid_ = osal_condvar_create(&handle_).ok(); }

    /// @brief Destroys the condition variable.
    ~condvar() noexcept
    {
        if (valid_)
        {
            (void)osal_condvar_destroy(&handle_);
            valid_ = false;
        }
    }

    condvar(const condvar&)            = delete;
    condvar& operator=(const condvar&) = delete;
    condvar(condvar&&)                 = delete;
    condvar& operator=(condvar&&)      = delete;

    // ---- wait --------------------------------------------------------------

    /// @brief Atomically unlock @p m and block until notified, then re-lock.
    /// @param m  The mutex that the caller holds.  Must be locked.
    void wait(mutex& m) noexcept { (void)osal_condvar_wait(&handle_, m.native_handle(), WAIT_FOREVER); }

    /// @brief Waits until @p pred returns true, re-checking after each wakeup.
    /// @details Provides built-in protection against spurious wakeups.
    ///          Equivalent to: @code while (!pred()) cv.wait(m); @endcode
    /// @param m     The mutex the caller holds.
    /// @param pred  Callable with signature @c bool().  Evaluated under @p m.
    template<typename Predicate>
    void wait(mutex& m, Predicate pred) noexcept(noexcept(pred()))
    {
        while (!pred())
        {
            wait(m);
        }
    }

    /// @brief Timed wait — block until notified or @p timeout expires.
    /// @param m        The mutex that the caller holds.
    /// @param timeout  Maximum time to wait.
    /// @return true if notified; false on timeout.
    bool wait_for(mutex& m, milliseconds timeout) noexcept
    {
        const tick_t ticks = clock_utils::ms_to_ticks(timeout);
        return osal_condvar_wait(&handle_, m.native_handle(), ticks).ok();
    }

    /// @brief Timed wait with predicate — returns true if @p pred is satisfied.
    /// @details Loops until @p pred is true or the total deadline is reached.
    ///          Protects against spurious wakeups.
    /// @param m        The mutex the caller holds.
    /// @param timeout  Maximum total wait time.
    /// @param pred     Callable with signature @c bool().  Evaluated under @p m.
    /// @return true if @p pred was satisfied; false if the timeout expired first.
    template<typename Predicate>
    bool wait_for(mutex& m, milliseconds timeout, Predicate pred) noexcept(noexcept(pred()))
    {
        const auto deadline = monotonic_clock::now() + timeout;
        while (!pred())
        {
            const auto now = monotonic_clock::now();
            if (now >= deadline)
            {
                return pred();
            }
            const auto remaining = std::chrono::duration_cast<milliseconds>(deadline - now);
            if (!wait_for(m, remaining))
            {
                return pred();
            }
        }
        return true;
    }

    /// @brief Waits until an absolute deadline.
    /// @param m         The mutex the caller holds.
    /// @param deadline  Absolute monotonic time point.
    /// @return true if notified before the deadline; false on timeout.
    /// @details Loops internally so that tick-count saturation (for unreasonably
    ///          large deadlines) does not cause premature false-timeout returns.
    bool wait_until(mutex& m, monotonic_clock::time_point deadline) noexcept
    {
        for (;;)
        {
            const auto now = monotonic_clock::now();
            if (deadline <= now)
            {
                return false;
            }
            const auto remaining = std::chrono::duration_cast<milliseconds>(deadline - now);
            if (wait_for(m, remaining))
            {
                return true;  // notified
            }
            // Timed out: either the real deadline passed (checked at top of loop)
            // or ms_to_ticks saturated a >49-day remaining into a ~49-day chunk.
            // Either way, re-evaluate the deadline.
        }
    }

    /// @brief Waits until an absolute deadline with predicate.
    /// @param m         The mutex the caller holds.
    /// @param deadline  Absolute monotonic time point.
    /// @param pred      Callable with signature @c bool().  Evaluated under @p m.
    /// @return true if @p pred was satisfied before the deadline; false otherwise.
    template<typename Predicate>
    bool wait_until(mutex& m, monotonic_clock::time_point deadline, Predicate pred) noexcept(noexcept(pred()))
    {
        // Delegate to the timed predicate wait_for, which already loops.
        const auto         now = monotonic_clock::now();
        const milliseconds remaining =
            (deadline > now) ? std::chrono::duration_cast<milliseconds>(deadline - now) : milliseconds{0};
        return wait_for(m, remaining, pred);
    }

    // ---- notify ------------------------------------------------------------

    /// @brief Wake one thread waiting on this condition variable.
    void notify_one() noexcept { (void)osal_condvar_notify_one(&handle_); }

    /// @brief Wake all threads waiting on this condition variable.
    void notify_all() noexcept { (void)osal_condvar_notify_all(&handle_); }

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the condition variable was successfully created.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    bool                            valid_;
    active_traits::condvar_handle_t handle_;
};

/// @} // osal_condvar

}  // namespace osal
