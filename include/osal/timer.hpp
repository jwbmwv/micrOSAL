// SPDX-License-Identifier: Apache-2.0
/// @file timer.hpp
/// @brief OSAL software timer — one-shot and periodic
/// @details Provides osal::timer for both one-shot and periodic (auto-reload)
///          software timers.  Callbacks are plain function pointers to avoid
///          std::function / heap allocation.
///
///          Guarantees:
///          - Callbacks are invoked in the backend's timer task / ISR context.
///          - Callbacks must be short and non-blocking (embedded rule).
///          - Timer resolution is backend-specific (typically RTOS tick period).
///          - All operations are O(1) and noexcept.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_timer
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstdint>

/// @brief Timer callback function pointer type.
/// @param arg User-defined argument passed at timer creation.
using osal_timer_callback_t = void (*)(void* arg);

extern "C"
{
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept;

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept;

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept;

    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept;

    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept;

    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept;

    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_timer OSAL Timer
/// @brief One-shot and periodic software timers.
/// @{

/// @brief Timer operating mode.
enum class timer_mode : std::uint8_t
{
    one_shot = 0U,  ///< Fires once and stops.
    periodic = 1U,  ///< Auto-reloads; fires repeatedly.
};

// ---------------------------------------------------------------------------
// timer_config — constexpr-constructible, place in .rodata / FLASH
// ---------------------------------------------------------------------------

/// @brief Immutable configuration for timer creation.
/// @details Declare as @c const or @c constexpr to place in .rodata (FLASH).
///
///          @code
///          constexpr osal::timer_config heartbeat_cfg{
///              heartbeat_cb, nullptr, osal::milliseconds{1000},
///              osal::timer_mode::periodic, "heartbeat"};
///          osal::timer heartbeat{heartbeat_cfg};
///          @endcode
struct timer_config
{
    osal_timer_callback_t callback = nullptr;           ///< Function called on expiry.
    void*                 arg      = nullptr;           ///< Opaque argument forwarded to callback.
    milliseconds          period{0};                    ///< Timer period.
    timer_mode            mode = timer_mode::one_shot;  ///< One-shot or periodic.
    const char*           name = nullptr;               ///< Debug name (may be nullptr).
};

/// @brief OSAL software timer.
/// @details Wraps the backend timer primitive.  Callback is a raw function pointer.
///          No virtual functions, no heap allocation.
class timer
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs the timer (does not start it).
    /// @param callback  Function to call on expiry.
    /// @param arg       Opaque argument forwarded to callback.
    /// @param period    Timer period.
    /// @param mode      one_shot or periodic.
    /// @param name      Debug name (may be nullptr).
    /// @complexity O(1)
    timer(osal_timer_callback_t callback, void* arg, milliseconds period, timer_mode mode = timer_mode::one_shot,
          const char* name = nullptr) noexcept
        : valid_(false), handle_{}
    {
        if constexpr (active_capabilities::has_timer)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(period);
            valid_ = osal_timer_create(&handle_, name, callback, arg, ticks, mode == timer_mode::periodic).ok();
        }
    }

    /// @brief Constructs from an immutable config (config may reside in FLASH).
    /// @param cfg  Configuration — typically declared @c const / @c constexpr.
    /// @complexity O(1)
    explicit timer(const timer_config& cfg) noexcept : valid_(false), handle_{}
    {
        if constexpr (active_capabilities::has_timer)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(cfg.period);
            valid_ =
                osal_timer_create(&handle_, cfg.name, cfg.callback, cfg.arg, ticks, cfg.mode == timer_mode::periodic)
                    .ok();
        }
    }

    /// @brief Destructs the timer (stops if active).
    ~timer() noexcept
    {
        if (valid_)
        {
            (void)osal_timer_stop(&handle_);
            (void)osal_timer_destroy(&handle_);
            valid_ = false;
        }
    }

    timer(const timer&)            = delete;
    timer& operator=(const timer&) = delete;
    timer(timer&&)                 = delete;
    timer& operator=(timer&&)      = delete;

    // ---- control -----------------------------------------------------------

    /// @brief Starts the timer.
    /// @return result::ok() on success; error_code::not_supported if timers
    ///         are unavailable on this backend.
    result start() noexcept
    {
        if constexpr (active_capabilities::has_timer)
        {
            return osal_timer_start(&handle_);
        }
        return error_code::not_supported;
    }

    /// @brief Stops the timer.
    result stop() noexcept
    {
        if constexpr (active_capabilities::has_timer)
        {
            return osal_timer_stop(&handle_);
        }
        return error_code::not_supported;
    }

    /// @brief Resets the timer (restarts its period from now).
    result reset() noexcept
    {
        if constexpr (active_capabilities::has_timer)
        {
            return osal_timer_reset(&handle_);
        }
        return error_code::not_supported;
    }

    /// @brief Changes the timer period without restarting it.
    /// @param period New period.
    result set_period(milliseconds period) noexcept
    {
        if constexpr (active_capabilities::has_timer)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(period);
            return osal_timer_set_period(&handle_, ticks);
        }
        return error_code::not_supported;
    }

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the timer was successfully created.
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    /// @brief Returns true if the timer is currently running.
    [[nodiscard]] bool is_active() const noexcept
    {
        if constexpr (active_capabilities::has_timer)
        {
            return osal_timer_is_active(&handle_);
        }
        return false;
    }

private:
    bool                          valid_;
    active_traits::timer_handle_t handle_;
};

/// @} // osal_timer

}  // namespace osal
