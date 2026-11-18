// SPDX-License-Identifier: Apache-2.0
/// @file semaphore.hpp
/// @brief OSAL semaphore — binary and counting variants
/// @details Provides osal::semaphore for both binary (max_count == 1) and
///          counting (max_count > 1) use-cases.  A single class handles both
///          modes; the type tag is advisory and controls assertions only.
///
///          ISR safety:
///          - give() and take() have ISR-safe counterparts isr_give() / isr_take()
///            on backends that support osal::capabilities::has_isr_semaphore.
///          - On backends without ISR support, isr_give/isr_take call the regular
///            variants and are NOT actually ISR-safe.
///
///          MISRA C++ 2023 notes:
///          - All operations are noexcept.
///          - No dynamic allocation; OS handle stored by value.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_semaphore
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "diagnostics.hpp"
#include "types.hpp"
#include <cstdint>

// Backend implementation functions — defined in src/<backend>/*.cpp.
extern "C"
{
    /// @brief Creates a backend semaphore with bounded permit count.
    /// @param[out] handle         Receives the native semaphore handle.
    /// @param[in]  initial_count  Initial permits, no greater than max_count.
    /// @param[in]  max_count      Nonzero maximum permit count.
    /// @return Success or the backend creation error.
    /// @note Thread context only.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept;

    /// @brief Destroys a backend semaphore after concurrent users have quiesced.
    /// @param[in,out] handle  Semaphore handle to release.
    /// @return Success or the backend destruction error.
    /// @note Thread context only; keep handle storage alive if destruction fails.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept;

    /// @brief Gives a permit from thread context without waiting.
    /// @param[in,out] handle  Initialised semaphore handle.
    /// @return Success or the backend overflow/error result.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept;

    /// @brief Gives a permit from ISR context without waiting.
    /// @param[in,out] handle  Initialised semaphore handle.
    /// @pre The active backend advertises ISR semaphore support.
    /// @return Success or the backend overflow/error result.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept;

    /// @brief Takes a permit from thread context.
    /// @param[in,out] handle         Initialised semaphore handle.
    /// @param[in]     timeout_ticks  Wait limit in ticks; NO_WAIT polls and WAIT_FOREVER waits indefinitely.
    /// @return Success or the backend wait/error result.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept;

    /// @brief Attempts to take a permit without waiting.
    /// @param[in,out] handle  Initialised semaphore handle.
    /// @return Success or the backend empty/error result.
    /// @note Thread context only.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_semaphore OSAL Semaphore
/// @brief Binary and counting semaphores.
/// @{

/// @brief Classifies the intended semaphore usage.
enum class semaphore_type : std::uint8_t
{
    binary   = 0U,  ///< Binary semaphore: max_count is implicitly 1.
    counting = 1U,  ///< Counting semaphore: max_count > 1.
};

// ---------------------------------------------------------------------------
// semaphore_config — constexpr-constructible, place in .rodata / FLASH
// ---------------------------------------------------------------------------

/// @brief Immutable configuration for semaphore creation.
/// @details Declare as @c const or @c constexpr to place in .rodata (FLASH).
///
///          @code
///          constexpr osal::semaphore_config cfg{
///              osal::semaphore_type::counting, 0, 10};
///          osal::semaphore s{cfg};  // only handle_ + valid_ in RAM
///          @endcode
///
/// @note The maximum count is enforced; checked_give() reports overflow.
struct semaphore_config
{
    semaphore_type type          = semaphore_type::binary;  ///< Binary or counting.
    unsigned       initial_count = 0;                       ///< Initial permit count.
    unsigned       max_count     = 1;                       ///< Maximum permit count (forced to 1 for binary).
};

/// @brief OSAL semaphore.
/// @details O(1), noexcept, non-allocating.  The OS handle is stored inline.
///          Copying and moving are disabled.
class semaphore
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the semaphore.
    /// @param[in] type           binary or counting.
    /// @param[in] initial_count  Initial permit count.
    /// @param[in] max_count      Maximum permit count (ignored for binary — forced to 1).
    /// @details Invalid type/counts leave the object invalid; inspect initialization_status().
    /// @complexity O(1)
    /// @blocking   Never.
    semaphore(semaphore_type type, unsigned initial_count, unsigned max_count = 1U) noexcept : valid_(false)
    {
        const unsigned mc = (type == semaphore_type::binary) ? 1U : max_count;
        initialize(type, initial_count, mc);
    }

    /// @brief Constructs from an immutable config (config may reside in FLASH).
    /// @param[in] cfg  Configuration — typically declared @c const / @c constexpr.
    /// @details Check valid() or initialization_status() before using the semaphore.
    /// @complexity O(1)
    /// @blocking   Never.
    explicit semaphore(const semaphore_config& cfg) noexcept : valid_(false)
    {
        const unsigned mc = (cfg.type == semaphore_type::binary) ? 1U : cfg.max_count;
        initialize(cfg.type, cfg.initial_count, mc);
    }

    /// @brief Attempts cleanup, discarding close() errors.
    /// @pre Users and waiters have quiesced; handle close() failure before destroying storage.
    ~semaphore() noexcept { (void)close(); }

    /// @brief Copy construction is disabled.
    semaphore(const semaphore&) = delete;
    /// @brief Copy assignment is disabled.
    semaphore& operator=(const semaphore&) = delete;
    /// @brief Move construction is disabled.
    semaphore(semaphore&&) = delete;
    /// @brief Move assignment is disabled.
    semaphore& operator=(semaphore&&) = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the semaphore was successfully initialised.
    /// @return Whether this object currently owns a valid semaphore.
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    /// @brief Reports the original initialization result.
    /// @return Success or the validation/backend creation error, unchanged by close().
    [[nodiscard]] result initialization_status() const noexcept { return initialization_status_; }

    /// @brief Idempotent checked destruction; failure retains ownership for retry.
    /// @details The caller must quiesce concurrent operations before closing.
    ///          On failure keep this object alive; destructor cleanup is best-effort.
    /// @return Success if already closed or destroyed, otherwise the backend destruction error.
    /// @note Thread context only.
    [[nodiscard]] result close() noexcept
    {
        if (!valid_)
        {
            return ok();
        }
        const result status = osal_semaphore_destroy(&handle_);
        diagnostics(diagnostic_resource::semaphore).attempt(status);
        if (status.ok())
        {
            valid_ = false;
            diagnostics(diagnostic_resource::semaphore).release();
        }
        return status;
    }

    /// @brief Gives a permit and preserves overflow and backend errors.
    /// @retval error_code::not_initialized  The semaphore is invalid.
    /// @return Otherwise the backend give result, including overflow when already full.
    /// @note Thread context only; does not wait for capacity.
    [[nodiscard]] result checked_give() noexcept
    {
        const result status = valid_ ? osal_semaphore_give(&handle_) : result{error_code::not_initialized};
        operations_.attempt(status);
        diagnostics(diagnostic_resource::semaphore).attempt(status);
        return status;
    }

    /// @brief Takes a permit; negative timeout means an indefinite wait.
    /// @details Zero never blocks. Positive timeouts require timed-semaphore
    ///          support; otherwise not_supported is returned without waiting.
    /// @param[in] timeout  Negative waits indefinitely; zero polls; positive values bound the wait.
    /// @retval error_code::not_initialized  The semaphore is invalid.
    /// @retval error_code::not_supported    A positive timeout requires timed-semaphore support.
    /// @return Otherwise the backend acquisition result, including contention/timeout errors.
    /// @note Thread context only.
    [[nodiscard]] result checked_take(milliseconds timeout = milliseconds{-1}) noexcept
    {
        result status{error_code::not_initialized};
        if (valid_)
        {
            if (timeout.count() < 0)
            {
                status = osal_semaphore_take(&handle_, WAIT_FOREVER);
            }
            else if (timeout.count() == 0)
            {
                status = osal_semaphore_try_take(&handle_);
            }
            else if constexpr (timed_semaphore_backend<active_backend>)
            {
                status = osal_semaphore_take(&handle_, clock_utils::ms_to_ticks(timeout));
            }
            else
            {
                status = error_code::not_supported;
            }
        }
        operations_.attempt(status);
        diagnostics(diagnostic_resource::semaphore).attempt(status);
        if (status == error_code::timeout || status == error_code::would_block)
        {
            operations_.contended();
            diagnostics(diagnostic_resource::semaphore).contended();
        }
        return status;
    }

    /// @brief Approximate operation counters; uint32 counters wrap modulo 2^32.
    /// @return Independently sampled counters for checked thread-context operations.
    [[nodiscard]] diagnostic_snapshot statistics() const noexcept { return operations_.snapshot(); }

    // ---- give (signal) -----------------------------------------------------

    /// @brief Gives (signals) the semaphore from thread context.
    /// @details Increments the count by 1.  Wakes one waiting thread if any.
    ///          The underlying OS result is intentionally discarded.
    ///          Use `try_give()` when the caller needs to detect overflow.
    /// @note   Not ISR-safe — use isr_give() from interrupt context.
    /// @complexity O(1)
    /// @blocking   Never.
    void give() noexcept { (void)checked_give(); }

    /// @brief Attempts to give without blocking; returns false on overflow.
    /// @return True if a permit was given; false on overflow, invalid state, or another backend error.
    /// @note Thread context only.
    /// @complexity O(1)
    /// @blocking   Never.
    bool try_give() noexcept { return checked_give().ok(); }

    /// @brief Gives the semaphore from ISR context.
    /// @details On backends with has_isr_semaphore == true, uses the ISR-safe
    ///          native API.  On others, falls back to give() (NOT ISR-safe).
    /// @warning Check capabilities<active_backend>::has_isr_semaphore before
    ///          calling from an ISR.
    /// @complexity O(1)
    /// @blocking   Never.
    void isr_give() noexcept { (void)osal_semaphore_give_isr(&handle_); }

    // ---- take (wait) -------------------------------------------------------

    /// @brief Takes (waits for) the semaphore, blocking indefinitely.
    /// @note   Not ISR-safe.
    /// @complexity O(1)
    /// @blocking   Potentially blocking.
    void take() noexcept { (void)checked_take(); }

    /// @brief Attempts to take without blocking.
    /// @return true if a permit was available; false otherwise.
    /// @complexity O(1)
    /// @blocking   Never.
    bool try_take() noexcept { return checked_take(milliseconds{0}).ok(); }

    /// @brief Takes, blocking for at most @p timeout milliseconds.
    /// @param[in] timeout  Wait policy as for checked_take().
    /// @return True if acquired; false on contention, timeout, invalid state, or another error.
    /// @note Thread context only; unsupported timed semaphores fall back to a non-blocking attempt.
    /// @complexity O(1)
    /// @blocking   Up to timeout.
    bool take_for(milliseconds timeout) noexcept
    {
        if constexpr (timed_semaphore_backend<active_backend>)
        {
            return checked_take(timeout).ok();
        }
        else
        {
            return try_take();
        }
    }

    /// @brief Takes, blocking until an absolute deadline.
    /// @param[in] deadline  Absolute monotonic time point.
    /// @return True if acquired, false on contention, timeout, invalid state, or another error.
    /// @note An expired deadline polls once. Unsupported timed semaphores also poll without waiting.
    /// @complexity O(1)
    /// @blocking   Until deadline.
    bool take_until(monotonic_clock::time_point deadline) noexcept
    {
        const monotonic_deadline take_deadline = monotonic_deadline::at(deadline);
        if (take_deadline.expired())
        {
            return try_take();
        }
        return take_for(take_deadline.remaining());
    }

private:
    void initialize(semaphore_type type, unsigned initial_count, unsigned max_count) noexcept
    {
        initialization_status_ = (type != semaphore_type::binary && type != semaphore_type::counting) ||
                                         max_count == 0U || initial_count > max_count
                                     ? result{error_code::invalid_argument}
                                     : osal_semaphore_create(&handle_, initial_count, max_count);
        valid_                 = initialization_status_.ok();
        auto& counters         = diagnostics(diagnostic_resource::semaphore);
        counters.attempt(initialization_status_);
        if (valid_)
        {
            counters.acquire();
        }
    }

    result                            initialization_status_{error_code::not_initialized};
    diagnostic_counters               operations_{};
    bool                              valid_;  ///< Init succeeded.
    active_traits::semaphore_handle_t handle_{};
};

/// @} // osal_semaphore

}  // namespace osal
