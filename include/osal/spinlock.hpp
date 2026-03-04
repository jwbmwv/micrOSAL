// SPDX-License-Identifier: Apache-2.0
/// @file spinlock.hpp
/// @brief OSAL spinlock — low-level spin-wait primitive
/// @details Provides osal::spinlock, a non-blocking busy-wait lock intended for
///          very short critical sections on SMP systems (e.g. protecting a
///          shared variable whose access takes < 10 cycles).
///
///          Availability:
///          - Native spinlock: capabilities<active_backend>::has_spinlock == true
///            (currently Zephyr, via k_spinlock, and FreeRTOS SMP builds).
///          - On all other backends lock() returns error_code::not_supported;
///            use a mutex instead.
///
///          @warning Holding a spinlock for long durations starves other CPUs.
///                   Use only to protect the minimum possible number of
///                   instructions.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_spinlock
#pragma once

#include "backends.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstdint>

extern "C"
{
    osal::result osal_spinlock_create(osal::active_traits::spinlock_handle_t* handle) noexcept;
    osal::result osal_spinlock_destroy(osal::active_traits::spinlock_handle_t* handle) noexcept;
    osal::result osal_spinlock_lock(osal::active_traits::spinlock_handle_t* handle) noexcept;
    bool         osal_spinlock_try_lock(osal::active_traits::spinlock_handle_t* handle) noexcept;
    void         osal_spinlock_unlock(osal::active_traits::spinlock_handle_t* handle) noexcept;
}  // extern "C"

namespace osal
{

/// @defgroup osal_spinlock OSAL Spinlock
/// @brief Low-level spin-wait lock for very short SMP critical sections.
/// @{

/// @brief OSAL spinlock.
/// @details On backends supporting native spinlocks (has_spinlock == true) this
///          maps directly to the backend primitive.  On all other backends,
///          lock() / try_lock() return error_code::not_supported so that
///          callers can use @c if constexpr to provide an alternative path.
///
/// @note  No dynamic allocation.  The OS handle is stored inline.
/// @note  Non-copyable and non-movable.
class spinlock
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Default-constructs and initialises the spinlock.
    /// @complexity O(1)
    /// @blocking   Never.
    spinlock() noexcept : valid_(false), handle_{}
    {
        if constexpr (active_capabilities::has_spinlock)
        {
            valid_ = osal_spinlock_create(&handle_).ok();
        }
    }

    /// @brief Destructor.
    ~spinlock() noexcept
    {
        if constexpr (active_capabilities::has_spinlock)
        {
            if (valid_)
            {
                (void)osal_spinlock_destroy(&handle_);
                valid_ = false;
            }
        }
    }

    spinlock(const spinlock&)            = delete;
    spinlock& operator=(const spinlock&) = delete;
    spinlock(spinlock&&)                 = delete;
    spinlock& operator=(spinlock&&)      = delete;

    // ---- operations --------------------------------------------------------

    /// @brief Acquires the spinlock, busy-waiting until it is free.
    /// @return result::ok() on success; error_code::not_supported if the backend
    ///         does not provide a native spinlock.
    /// @warning Do NOT call from interrupt context on all backends.
    /// @complexity O(1) amortised (busy-waits until acquired).
    /// @blocking   Spins until acquired.
    result lock() noexcept
    {
        if constexpr (active_capabilities::has_spinlock)
        {
            return osal_spinlock_lock(&handle_);
        }
        else
        {
            return error_code::not_supported;
        }
    }

    /// @brief Attempts to acquire the spinlock without spinning.
    /// @return true if acquired; false if already held or backend unsupported.
    /// @complexity O(1)
    /// @blocking   Never.
    [[nodiscard]] bool try_lock() noexcept
    {
        if constexpr (active_capabilities::has_spinlock)
        {
            return osal_spinlock_try_lock(&handle_);
        }
        else
        {
            return false;
        }
    }

    /// @brief Releases the spinlock.
    /// @complexity O(1)
    /// @blocking   Never.
    void unlock() noexcept
    {
        if constexpr (active_capabilities::has_spinlock)
        {
            osal_spinlock_unlock(&handle_);
        }
    }

    // ---- RAII guard --------------------------------------------------------

    /// @brief RAII lock guard for spinlock.
    /// @details Acquires on construction; releases on destruction.
    class lock_guard
    {
    public:
        /// @brief Acquires the spinlock.
        explicit lock_guard(spinlock& sl) noexcept : sl_(sl) { (void)sl_.lock(); }
        /// @brief Releases the spinlock.
        ~lock_guard() noexcept { sl_.unlock(); }

        lock_guard(const lock_guard&)            = delete;
        lock_guard& operator=(const lock_guard&) = delete;
        lock_guard(lock_guard&&)                 = delete;
        lock_guard& operator=(lock_guard&&)      = delete;

    private:
        spinlock& sl_;
    };

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the spinlock was successfully initialised.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    bool                             valid_;
    active_traits::spinlock_handle_t handle_;
};

/// @} // osal_spinlock

}  // namespace osal
