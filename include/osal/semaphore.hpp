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
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept;

    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept;

    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept;

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept;

    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept;

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
    /// @complexity O(1)
    /// @blocking   Never.
    semaphore(semaphore_type type, unsigned initial_count, unsigned max_count = 1U) noexcept : valid_(false)
    {
        const unsigned mc = (type == semaphore_type::binary) ? 1U : max_count;
        initialize(type, initial_count, mc);
    }

    /// @brief Constructs from an immutable config (config may reside in FLASH).
    /// @param[in] cfg  Configuration — typically declared @c const / @c constexpr.
    /// @complexity O(1)
    /// @blocking   Never.
    explicit semaphore(const semaphore_config& cfg) noexcept : valid_(false)
    {
        const unsigned mc = (cfg.type == semaphore_type::binary) ? 1U : cfg.max_count;
        initialize(cfg.type, cfg.initial_count, mc);
    }

    /// @brief Destructs the semaphore.
    ~semaphore() noexcept { (void)close(); }

    semaphore(const semaphore&)            = delete;
    semaphore& operator=(const semaphore&) = delete;
    semaphore(semaphore&&)                 = delete;
    semaphore& operator=(semaphore&&)      = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the semaphore was successfully initialised.
    [[nodiscard]] bool   valid() const noexcept { return valid_; }
    [[nodiscard]] result initialization_status() const noexcept { return initialization_status_; }

    /// @brief Idempotent checked destruction; failure retains ownership for retry.
    /// @details The caller must quiesce concurrent operations before closing.
    ///          On failure keep this object alive; destructor cleanup is best-effort.
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
    /// @return true if the give succeeded; false if already at max_count.
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
    /// @param[in] timeout  Maximum wait time.
    /// @return true if acquired within the timeout.
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
    /// @return true if acquired before the deadline.
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
