// SPDX-License-Identifier: Apache-2.0
/// @file mutex.hpp
/// @brief OSAL mutex — normal and recursive variants
/// @details Provides osal::mutex with normal and recursive modes.
///
///          Design rules:
///          - No virtual functions, no RTTI, no dynamic allocation.
///          - Operations are O(1) and noexcept.
///          - Recursive mode uses the native primitive when
///            capabilities<Backend>::has_recursive_mutex == true,
///            otherwise emulates via owner tracking + counter.
///          - Lock ordering and deadlock detection are NOT enforced at runtime;
///            add OSAL_DEBUG_LOCKS for assisted diagnostics (not MISRA-safe).
///          - Each mutex holds its OS handle by value; no heap involvement.
///
///          MISRA C++ 2023 notes:
///          - Rule 15.1: No exception specifications wider than noexcept.
///          - Rule 12.2: All members explicitly initialised.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_mutex
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstdint>

// Forward-declare backend implementation functions (defined in src/<backend>/).
extern "C"
{
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool recursive) noexcept;

    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept;

    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept;

    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept;

    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_mutex OSAL Mutex
/// @brief Normal and recursive mutexes.
/// @{

// ---------------------------------------------------------------------------
// mutex_type
// ---------------------------------------------------------------------------

/// @brief Selects normal or recursive locking semantics.
/// @details - normal:    re-entrant lock by the same thread causes deadlock or assert.
///          - recursive: re-entrant lock by the same thread increments a counter.
enum class mutex_type : std::uint8_t
{
    normal    = 0U,  ///< Standard non-recursive mutex.
    recursive = 1U,  ///< Recursive (re-entrant) mutex.
};

// ---------------------------------------------------------------------------
// mutex_config — constexpr-constructible, place in .rodata / FLASH
// ---------------------------------------------------------------------------

/// @brief Immutable configuration for mutex creation.
/// @details Declare as @c const or @c constexpr to place in .rodata (FLASH).
///          Only the mutable handle lives in RAM.
///
///          @code
///          constexpr osal::mutex_config cfg{osal::mutex_type::recursive};
///          osal::mutex m{cfg};  // only handle_ + valid_ in RAM
///          @endcode
struct mutex_config
{
    mutex_type type = mutex_type::normal;  ///< Mutex locking semantics.
};

// ---------------------------------------------------------------------------
// mutex
// ---------------------------------------------------------------------------

/// @brief OSAL mutex.
/// @details Wraps the backend-native mutex.  All operations are O(1) and noexcept.
///          Copying and moving are explicitly disabled; mutexes are not transferable.
///
/// @note  MISRA C++ 2023: classes owning resources must not be copyable (Rule A12-8-1).
class mutex
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the mutex.
    /// @param type  normal or recursive.
    /// @details Calls the backend to initialise the underlying OS primitive.
    ///          If initialisation fails the mutex is left in an uninitialised
    ///          state; check valid() before use in safety-critical code.
    /// @complexity O(1)
    /// @blocking   Never — initialisation uses static storage only.
    explicit mutex(mutex_type type = mutex_type::normal) noexcept : valid_(false), handle_{}
    {
        valid_ = osal_mutex_create(&handle_, type == mutex_type::recursive).ok();
    }

    /// @brief Constructs from an immutable config (config may reside in FLASH).
    /// @param cfg  Configuration — typically declared @c const / @c constexpr.
    /// @complexity O(1)
    /// @blocking   Never.
    explicit mutex(const mutex_config& cfg) noexcept : valid_(false), handle_{}
    {
        valid_ = osal_mutex_create(&handle_, cfg.type == mutex_type::recursive).ok();
    }

    /// @brief Destructs and releases the mutex.
    /// @warning  Destroying a locked mutex is undefined behaviour (UB) on all backends.
    /// @complexity O(1)
    ~mutex() noexcept
    {
        if (valid_)
        {
            (void)osal_mutex_destroy(&handle_);
            valid_ = false;
        }
    }

    // Disable copy and move — OS handles are non-transferable.
    mutex(const mutex&)            = delete;
    mutex& operator=(const mutex&) = delete;
    mutex(mutex&&)                 = delete;
    mutex& operator=(mutex&&)      = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the mutex was successfully initialised.
    /// @return true if the backing OS primitive is valid.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    // ---- locking -----------------------------------------------------------

    /// @brief Acquires the mutex, blocking indefinitely.
    /// @details Blocks the calling thread until the mutex is acquired.
    ///          For recursive mutexes, increments the lock counter if the
    ///          calling thread is already the owner.
    /// @note    Not ISR-safe on any backend.
    /// @complexity O(1)
    /// @blocking   Potentially blocking.
    void lock() noexcept { (void)osal_mutex_lock(&handle_, WAIT_FOREVER); }

    /// @brief Attempts to acquire the mutex without blocking.
    /// @return true if the mutex was acquired; false if already locked by another thread.
    /// @note    Not ISR-safe on any backend.
    /// @complexity O(1)
    /// @blocking   Never.
    bool try_lock() noexcept { return osal_mutex_try_lock(&handle_).ok(); }

    /// @brief Acquires the mutex, blocking up to @p timeout_ms milliseconds.
    /// @param  timeout_ms Maximum time to wait.
    /// @return true if acquired within the timeout; false on timeout.
    /// @note   Requires capabilities<active_backend>::has_timed_mutex.
    ///         Returns false immediately if the backend does not support timed locks.
    /// @complexity O(1)
    /// @blocking   Potentially blocking up to timeout_ms.
    bool try_lock_for(milliseconds timeout_ms) noexcept
    {
        if constexpr (active_capabilities::has_timed_mutex)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout_ms);
            return osal_mutex_lock(&handle_, ticks).ok();
        }
        else
        {
            return try_lock();
        }
    }

    /// @brief Releases the mutex.
    /// @details For recursive mutexes, decrements the lock counter; the mutex
    ///          is only released when the counter reaches zero.
    /// @warning Calling unlock() from a thread that does not own the mutex is
    ///          undefined behaviour.
    /// @note    Not ISR-safe on any backend.
    /// @complexity O(1)
    /// @blocking   Never.
    void unlock() noexcept { (void)osal_mutex_unlock(&handle_); }

    /// @brief Returns a pointer to the internal backend handle.
    /// @note  Intended for OSAL internal use (e.g. condvar).
    [[nodiscard]] active_traits::mutex_handle_t* native_handle() noexcept { return &handle_; }

    // ---- RAII lock_guard ---------------------------------------------------

    /// @brief RAII scoped lock for osal::mutex.
    /// @details Acquires the mutex on construction; releases on destruction.
    class lock_guard
    {
    public:
        /// @brief Constructs the guard and locks @p m.
        /// @param m  The mutex to lock.
        explicit lock_guard(mutex& m) noexcept : m_(m) { m_.lock(); }
        /// @brief Destructs the guard and unlocks the mutex.
        ~lock_guard() noexcept { m_.unlock(); }

        lock_guard(const lock_guard&)            = delete;
        lock_guard& operator=(const lock_guard&) = delete;
        lock_guard(lock_guard&&)                 = delete;
        lock_guard& operator=(lock_guard&&)      = delete;

    private:
        mutex& m_;
    };

private:
    // ---- data members (mutable runtime state only — config is consumed at init)
    bool                          valid_;   ///< Initialisation succeeded.
    active_traits::mutex_handle_t handle_;  ///< Opaque backend handle.
};

/// @} // osal_mutex

}  // namespace osal
