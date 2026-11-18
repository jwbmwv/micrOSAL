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
    /// @brief Creates a backend mutex.
    /// @param[out] handle     Receives the native mutex handle.
    /// @param[in]  recursive  Whether repeated locking by the owner is allowed.
    /// @return Success or the backend creation error.
    /// @note Thread context only; release a successfully created handle with osal_mutex_destroy().
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool recursive) noexcept;

    /// @brief Destroys a backend mutex after users have quiesced.
    /// @param[in,out] handle  Mutex handle to release.
    /// @pre The mutex is unlocked and has no waiters; call from thread context.
    /// @return Success or the backend destruction error.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept;

    /// @brief Acquires a backend mutex from thread context.
    /// @param[in,out] handle         Initialised mutex handle.
    /// @param[in]     timeout_ticks  Wait limit in ticks; NO_WAIT polls and WAIT_FOREVER waits indefinitely.
    /// @return Acquisition status, including timeout/ownership errors as provided by the backend.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept;

    /// @brief Attempts to acquire a backend mutex without waiting.
    /// @param[in,out] handle  Initialised mutex handle.
    /// @return Acquisition status, preserving the backend's contention or ownership error.
    /// @note Thread context only.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept;

    /// @brief Releases one level of backend mutex ownership.
    /// @param[in,out] handle  Mutex owned by the calling thread.
    /// @return Success or the backend unlock error.
    /// @note Thread context only.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept;
    /// @brief Creates a mutex with explicit protocol and robustness configuration.
    /// @param[out] handle  Receives the native mutex handle.
    /// @param[in]  config  Configuration consumed during creation.
    /// @return Success or a configuration/capability/backend creation error.
    /// @note Thread context only.
    osal::result osal_mutex_create_config(osal::active_traits::mutex_handle_t* handle,
                                          const osal::mutex_config*            config) noexcept;
    /// @brief Marks repaired robust-mutex state consistent.
    /// @param[in,out] handle  Robust mutex owned after an owner_died result.
    /// @pre The caller repaired the protected state and still owns the mutex.
    /// @return Success or a capability/ownership/backend error.
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
    /// @brief Checks whether the active backend can honor a mutex protocol.
    /// @param[in] protocol  Requested mutex protocol.
    /// @return True when the protocol is supported by this build.
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
    /// @details Check valid() or initialization_status() for creation failure.
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
    /// @details Cleanup is best-effort; explicitly handle close() failures before destroying storage.
    /// @complexity O(1)
    ~mutex() noexcept { (void)close(); }

    // Disable copy and move — OS handles are non-transferable.
    /// @brief Copy construction is disabled because mutex ownership is not transferable.
    mutex(const mutex&) = delete;
    /// @brief Copy assignment is disabled.
    mutex& operator=(const mutex&) = delete;
    /// @brief Move construction is disabled.
    mutex(mutex&&) = delete;
    /// @brief Move assignment is disabled.
    mutex& operator=(mutex&&) = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the mutex was successfully initialised.
    /// @return true if the backing OS primitive is valid.
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    /// @brief Reports the original creation result.
    /// @return Success or the initialization error; closing does not replace this result.
    [[nodiscard]] result initialization_status() const noexcept { return initialization_status_; }

    /// @brief Closes an unlocked mutex with no waiters; failure retains ownership.
    /// @pre Concurrent users have quiesced; call from thread context.
    /// @return Success if already closed or destroyed, otherwise the backend destruction error.
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

    /// @brief Acquires the mutex with explicit error reporting.
    /// @param[in] timeout  Negative waits indefinitely; zero polls; positive values bound the wait.
    /// @retval error_code::not_initialized  The mutex is not valid.
    /// @retval error_code::not_supported    A positive timeout requires timed-mutex support.
    /// @return Otherwise the backend acquisition result, including owner_died for a recovered robust mutex.
    /// @note Thread context only. An owner_died result grants ownership but requires state repair.
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
    /// @brief Releases one ownership level and preserves backend errors.
    /// @pre The calling thread owns the mutex.
    /// @return Success or the backend unlock error.
    [[nodiscard]] result checked_unlock() noexcept { return osal_mutex_unlock(&handle_); }
    /// @brief Marks repaired protected state consistent after owner_died.
    /// @pre The calling thread owns the robust mutex and has repaired its protected state.
    /// @return Success or the backend capability/ownership error.
    [[nodiscard]] result consistent() noexcept { return osal_mutex_consistent(&handle_); }

    /// @brief Scoped checked lock, including ownership granted with owner_died.
    class checked_lock_guard
    {
    public:
        /// @brief Attempts acquisition and retains the result for inspection.
        /// @param[in,out] m        Mutex to acquire; must outlive the guard.
        /// @param[in]     timeout  Wait policy as for checked_lock().
        /// @note After owner_died, repair state and call consistent() before releasing ownership.
        explicit checked_lock_guard(mutex& m, milliseconds timeout = milliseconds{-1}) noexcept
            : mutex_(m), status_(m.checked_lock(timeout)), owns_(status_.ok() || status_ == error_code::owner_died)
        {
        }
        /// @brief Releases owned locking; backend cleanup errors are discarded.
        ~checked_lock_guard() noexcept
        {
            if (owns_)
            {
                (void)mutex_.checked_unlock();
            }
        }
        /// @brief Copy construction is disabled.
        checked_lock_guard(const checked_lock_guard&) = delete;
        /// @brief Copy assignment is disabled.
        checked_lock_guard& operator=(const checked_lock_guard&) = delete;
        /// @brief Move construction is disabled.
        checked_lock_guard(checked_lock_guard&&) = delete;
        /// @brief Move assignment is disabled.
        checked_lock_guard& operator=(checked_lock_guard&&) = delete;
        /// @brief Reports the acquisition result.
        /// @return Original checked_lock() result, unchanged by unlock().
        [[nodiscard]] result status() const noexcept { return status_; }
        /// @brief Checks whether this guard still owns the mutex.
        /// @return True after successful acquisition or owner_died until a successful unlock().
        [[nodiscard]] bool owns_lock() const noexcept { return owns_; }
        /// @brief Releases owned locking, retaining ownership on backend failure.
        /// @retval error_code::not_owner  This guard does not own the mutex.
        /// @return Otherwise the backend unlock result.
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
    ///          Acquisition errors are discarded; use checked_lock() when errors or robust recovery matter.
    /// @note    Not ISR-safe on any backend.
    /// @complexity O(1)
    /// @blocking   Potentially blocking.
    void lock() noexcept { (void)checked_lock(); }

    /// @brief Attempts to acquire the mutex without blocking.
    /// @return True if acquired; false on contention, invalid state, or another acquisition error.
    /// @warning A robust owner_died result grants ownership but returns false here.
    ///          Use checked_lock() to distinguish and repair that state.
    /// @note    Not ISR-safe on any backend.
    /// @complexity O(1)
    /// @blocking   Never.
    bool try_lock() noexcept { return checked_lock(milliseconds{0}).ok(); }

    /// @brief Acquires the mutex, blocking up to @p timeout_ms milliseconds.
    /// @param[in] timeout_ms  Maximum time to wait.
    /// @return True if acquired; false on contention, timeout, or another backend error.
    /// @note   Requires capabilities<active_backend>::has_timed_mutex.
    ///         Falls back to a non-blocking attempt if timed locks are unsupported.
    ///         Non-positive durations convert to a non-blocking attempt.
    /// @warning A robust owner_died result grants ownership but returns false here.
    ///          Use checked_lock() to distinguish and repair that state.
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
    /// @details Backend errors are discarded; use checked_unlock() for error reporting.
    /// @complexity O(1)
    /// @blocking   Never.
    void unlock() noexcept { (void)osal_mutex_unlock(&handle_); }

    /// @brief Returns a pointer to the internal backend handle.
    /// @note  Intended for OSAL internal use (e.g. condvar).
    /// @return Borrowed handle storage, valid only while this mutex object is alive.
    [[nodiscard]] active_traits::mutex_handle_t* native_handle() noexcept { return &handle_; }

    // ---- RAII lock_guard ---------------------------------------------------

    /// @brief RAII scoped lock for osal::mutex.
    /// @details Acquires the mutex on construction; releases on destruction.
    class lock_guard
    {
    public:
        /// @brief Constructs the guard and locks @p m.
        /// @param[in,out] m  Mutex to lock; must outlive the guard.
        /// @note Lock errors are discarded; use checked_lock_guard to inspect acquisition.
        explicit lock_guard(mutex& m) noexcept : m_(m) { m_.lock(); }
        /// @brief Destructs the guard and unlocks the mutex.
        ~lock_guard() noexcept { m_.unlock(); }

        /// @brief Copy construction is disabled.
        lock_guard(const lock_guard&) = delete;
        /// @brief Copy assignment is disabled.
        lock_guard& operator=(const lock_guard&) = delete;
        /// @brief Move construction is disabled.
        lock_guard(lock_guard&&) = delete;
        /// @brief Move assignment is disabled.
        lock_guard& operator=(lock_guard&&) = delete;

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
