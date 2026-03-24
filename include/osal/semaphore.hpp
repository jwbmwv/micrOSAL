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
/// @note On POSIX and Linux backends, the underlying `sem_t` does not enforce
///       a caller-specified maximum count.  The @c max_count field is accepted
///       but silently ignored; the effective upper bound is `SEM_VALUE_MAX`
///       (typically 2^31 - 1).  Portable code that requires strict max-count
///       enforcement should verify the active backend's behaviour.
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
    /// @param type           binary or counting.
    /// @param initial_count  Initial permit count.
    /// @param max_count      Maximum permit count (ignored for binary — forced to 1).
    /// @complexity O(1)
    /// @blocking   Never.
    semaphore(semaphore_type type, unsigned initial_count, unsigned max_count = 1U) noexcept : valid_(false), handle_{}
    {
        const unsigned mc = (type == semaphore_type::binary) ? 1U : max_count;
        valid_            = osal_semaphore_create(&handle_, initial_count, mc).ok();
    }

    /// @brief Constructs from an immutable config (config may reside in FLASH).
    /// @param cfg  Configuration — typically declared @c const / @c constexpr.
    /// @complexity O(1)
    /// @blocking   Never.
    explicit semaphore(const semaphore_config& cfg) noexcept : valid_(false), handle_{}
    {
        const unsigned mc = (cfg.type == semaphore_type::binary) ? 1U : cfg.max_count;
        valid_            = osal_semaphore_create(&handle_, cfg.initial_count, mc).ok();
    }

    /// @brief Destructs the semaphore.
    ~semaphore() noexcept
    {
        if (valid_)
        {
            (void)osal_semaphore_destroy(&handle_);
            valid_ = false;
        }
    }

    semaphore(const semaphore&)            = delete;
    semaphore& operator=(const semaphore&) = delete;
    semaphore(semaphore&&)                 = delete;
    semaphore& operator=(semaphore&&)      = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the semaphore was successfully initialised.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    // ---- give (signal) -----------------------------------------------------

    /// @brief Gives (signals) the semaphore from thread context.
    /// @details Increments the count by 1.  Wakes one waiting thread if any.
    ///          The underlying OS result is intentionally discarded; on a
    ///          correctly initialised semaphore this call cannot fail.
    ///          Use `try_give()` when the caller needs to detect overflow.
    /// @note   Not ISR-safe — use isr_give() from interrupt context.
    /// @complexity O(1)
    /// @blocking   Never.
    void give() noexcept { (void)osal_semaphore_give(&handle_); }

    /// @brief Attempts to give without blocking; returns false on overflow.
    /// @return true if the give succeeded; false if already at max_count.
    /// @complexity O(1)
    /// @blocking   Never.
    bool try_give() noexcept { return osal_semaphore_give(&handle_).ok(); }

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
    void take() noexcept { (void)osal_semaphore_take(&handle_, WAIT_FOREVER); }

    /// @brief Attempts to take without blocking.
    /// @return true if a permit was available; false otherwise.
    /// @complexity O(1)
    /// @blocking   Never.
    bool try_take() noexcept { return osal_semaphore_try_take(&handle_).ok(); }

    /// @brief Takes, blocking for at most @p timeout milliseconds.
    /// @param timeout Maximum wait time.
    /// @return true if acquired within the timeout.
    /// @complexity O(1)
    /// @blocking   Up to timeout.
    bool take_for(milliseconds timeout) noexcept
    {
        if constexpr (timed_semaphore_backend<active_backend>)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            return osal_semaphore_take(&handle_, ticks).ok();
        }
        else
        {
            return try_take();
        }
    }

    /// @brief Takes, blocking until an absolute deadline.
    /// @param deadline Absolute monotonic time point.
    /// @return true if acquired before the deadline.
    /// @complexity O(1)
    /// @blocking   Until deadline.
    bool take_until(monotonic_clock::time_point deadline) noexcept
    {
        const auto now = monotonic_clock::now();
        if (deadline <= now)
        {
            return try_take();
        }
        const auto diff = deadline - now;
        const auto ms   = std::chrono::duration_cast<milliseconds>(diff);
        return take_for(ms);
    }

private:
    bool                              valid_;  ///< Init succeeded.
    active_traits::semaphore_handle_t handle_;
};

/// @} // osal_semaphore

}  // namespace osal
