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
#include "diagnostics.hpp"
#include "types.hpp"
#include <cstdint>

namespace osal
{
struct mutex_config;
}

// Forward-declare backend implementation functions (defined in src/<backend>/).
extern "C"
{
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool recursive) noexcept;

    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept;

    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept;

    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept;

    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept;
    osal::result osal_mutex_create_config(osal::active_traits::mutex_handle_t* handle,
                                          const osal::mutex_config*            config) noexcept;
    osal::result osal_mutex_consistent(osal::active_traits::mutex_handle_t* handle) noexcept;

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
enum class mutex_protocol : std::uint8_t
{
    native_default,
    none,
    priority_inheritance,
    priority_ceiling
};

enum class mutex_robustness : std::uint8_t
{
    non_robust,
    robust
};

struct mutex_config
{
    mutex_type       type             = mutex_type::normal;  ///< Mutex locking semantics.
    mutex_protocol   protocol         = mutex_protocol::native_default;
    mutex_robustness robustness       = mutex_robustness::non_robust;
    int              priority_ceiling = 0;  ///< Native scheduler priority; used only by priority_ceiling.
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
    [[nodiscard]] static constexpr bool supports_protocol(mutex_protocol protocol) noexcept
    {
        if (protocol == mutex_protocol::native_default)
        {
            return true;
        }
#if defined(OSAL_BACKEND_LINUX) || defined(OSAL_BACKEND_POSIX)
        return protocol <= mutex_protocol::priority_ceiling;
#elif defined(OSAL_BACKEND_FREERTOS) || defined(OSAL_BACKEND_ZEPHYR)
        return protocol == mutex_protocol::priority_inheritance && active_capabilities::has_priority_inheritance;
#else
        return false;
#endif
    }
    static constexpr bool supports_robustness =
#if (defined(OSAL_BACKEND_LINUX) || defined(OSAL_BACKEND_POSIX)) && defined(__linux__)
        true;
#else
        false;
#endif
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the mutex.
    /// @param[in] type  normal or recursive.
    /// @details Calls the backend to initialise the underlying OS primitive.
    ///          If initialisation fails the mutex is left in an uninitialised
    ///          state; check valid() before use in safety-critical code.
    /// @complexity O(1)
    /// @blocking   Never — initialisation uses static storage only.
    explicit mutex(mutex_type type = mutex_type::normal) noexcept : mutex(mutex_config{type}) {}

    /// @brief Constructs from an immutable config (config may reside in FLASH).
    /// @param[in] cfg  Configuration — typically declared @c const / @c constexpr.
    /// @complexity O(1)
    /// @blocking   Never.
    explicit mutex(const mutex_config& cfg) noexcept : valid_(false)
    {
        initialization_status_ = osal_mutex_create_config(&handle_, &cfg);
        valid_                 = initialization_status_.ok();
        auto& counters         = diagnostics(diagnostic_resource::mutex);
        counters.attempt(initialization_status_);
        if (valid_)
        {
            counters.acquire();
        }
    }

    /// @brief Destructs and releases the mutex.
    /// @warning  Destroying a locked mutex is undefined behaviour (UB) on all backends.
    /// @complexity O(1)
    ~mutex() noexcept { (void)close(); }

    // Disable copy and move — OS handles are non-transferable.
    mutex(const mutex&)            = delete;
    mutex& operator=(const mutex&) = delete;
    mutex(mutex&&)                 = delete;
    mutex& operator=(mutex&&)      = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the mutex was successfully initialised.
    /// @return true if the backing OS primitive is valid.
    [[nodiscard]] bool   valid() const noexcept { return valid_; }
    [[nodiscard]] result initialization_status() const noexcept { return initialization_status_; }

    /// Close only when unlocked and without waiters. A failed close retains ownership.
    [[nodiscard]] result close() noexcept
    {
        if (!valid_)
        {
            return ok();
        }
        const result status = osal_mutex_destroy(&handle_);
        if (status.ok())
        {
            valid_ = false;
            diagnostics(diagnostic_resource::mutex).release();
        }
        return status;
    }

    [[nodiscard]] result checked_lock(milliseconds timeout = milliseconds{-1}) noexcept
    {
        if (!valid_)
        {
            return error_code::not_initialized;
        }
        if constexpr (!active_capabilities::has_timed_mutex)
        {
            if (timeout.count() > 0)
            {
                return error_code::not_supported;
            }
        }
        const result status =
            timeout.count() == 0
                ? osal_mutex_try_lock(&handle_)
                : osal_mutex_lock(&handle_, timeout.count() < 0 ? WAIT_FOREVER : clock_utils::ms_to_ticks(timeout));
        auto& counters = diagnostics(diagnostic_resource::mutex);
        counters.attempt(status);
        if (status == error_code::timeout || status == error_code::would_block)
        {
            counters.contended();
        }
        return status;
    }
    [[nodiscard]] result checked_unlock() noexcept { return osal_mutex_unlock(&handle_); }
    /// Mark repaired protected state consistent after owner_died; caller must own the robust mutex.
    [[nodiscard]] result consistent() noexcept { return osal_mutex_consistent(&handle_); }

    class checked_lock_guard
    {
    public:
        explicit checked_lock_guard(mutex& m, milliseconds timeout = milliseconds{-1}) noexcept
            : mutex_(m), status_(m.checked_lock(timeout)), owns_(status_.ok() || status_ == error_code::owner_died)
        {
        }
        ~checked_lock_guard() noexcept
        {
            if (owns_)
            {
                (void)mutex_.checked_unlock();
            }
        }
        checked_lock_guard(const checked_lock_guard&)            = delete;
        checked_lock_guard& operator=(const checked_lock_guard&) = delete;
        checked_lock_guard(checked_lock_guard&&)                 = delete;
        checked_lock_guard&  operator=(checked_lock_guard&&)     = delete;
        [[nodiscard]] result status() const noexcept { return status_; }
        [[nodiscard]] bool   owns_lock() const noexcept { return owns_; }
        [[nodiscard]] result unlock() noexcept
        {
            if (!owns_)
            {
                return error_code::not_owner;
            }
            const result status = mutex_.checked_unlock();
            if (status.ok())
            {
                owns_ = false;
            }
            return status;
        }

    private:
        mutex& mutex_;
        result status_;
        bool   owns_;
    };

    // ---- locking -----------------------------------------------------------

    /// @brief Acquires the mutex, blocking indefinitely.
    /// @details Blocks the calling thread until the mutex is acquired.
    ///          For recursive mutexes, increments the lock counter if the
    ///          calling thread is already the owner.
    /// @note    Not ISR-safe on any backend.
    /// @complexity O(1)
    /// @blocking   Potentially blocking.
    void lock() noexcept { (void)checked_lock(); }

    /// @brief Attempts to acquire the mutex without blocking.
    /// @return true if the mutex was acquired; false if already locked by another thread.
    /// @note    Not ISR-safe on any backend.
    /// @complexity O(1)
    /// @blocking   Never.
    bool try_lock() noexcept { return checked_lock(milliseconds{0}).ok(); }

    /// @brief Acquires the mutex, blocking up to @p timeout_ms milliseconds.
    /// @param[in] timeout_ms  Maximum time to wait.
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
        /// @param[in] m  The mutex to lock.
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
    bool                          valid_;  ///< Initialisation succeeded.
    result                        initialization_status_{error_code::not_initialized};
    active_traits::mutex_handle_t handle_{};  ///< Opaque backend handle.
};

/// @} // osal_mutex

}  // namespace osal
