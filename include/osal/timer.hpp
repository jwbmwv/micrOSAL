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
///          - Periodic cadence and callback serialization are backend-specific.
///            The generic POSIX backend (POSIX, RTEMS, and INTEGRITY) serializes
///            callbacks with fixed-delay scheduling: the next full period begins
///            after the preceding callback returns. It neither overlaps callbacks
///            nor replays missed expirations.
///          - All operations are O(1) and noexcept.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_timer
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "diagnostics.hpp"
#include "types.hpp"
#include <cstdint>

/// @brief Timer callback function pointer type.
/// @param[in] arg  User-defined argument passed at timer creation.
using osal_timer_callback_t = void (*)(void* arg);

extern "C"
{
    // NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t cb, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept;

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept;

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept;

    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept;

    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept;

    osal::result
    osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                          osal::tick_t p) noexcept;  // NOLINT(readability-inconsistent-declaration-parameter-name)

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
    /// @brief True when the active backend provides timer support.
    static constexpr bool is_supported = supports_requirement<support_requirement::timer>;

    /// @brief Whether backend quiescence can permit reclamation after a start attempt.
    /// @details False means close() returns not_supported after any start attempt,
    ///          retaining the native handle/slot; even stopping does not make it
    ///          reclaimable. Keep this object and its callback context alive.
    ///          True is not a promise that every close succeeds: callers must still
    ///          satisfy backend quiescence and thread/callback-context restrictions.
#if defined(OSAL_BACKEND_THREADX) || defined(OSAL_BACKEND_PX5) || defined(OSAL_BACKEND_MICRIUM) ||          \
    defined(OSAL_BACKEND_CHIBIOS) || defined(OSAL_BACKEND_EMBOS) || defined(OSAL_BACKEND_VXWORKS) ||        \
    defined(OSAL_BACKEND_NUTTX) || defined(OSAL_BACKEND_CMSIS_RTOS) || defined(OSAL_BACKEND_CMSIS_RTOS2) || \
    (defined(OSAL_BACKEND_ZEPHYR) && defined(CONFIG_SMP) && CONFIG_SMP)
    static constexpr bool can_reclaim_after_start = false;
#else
    static constexpr bool can_reclaim_after_start = true;
#endif

    /// @brief Enforce timer support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_support()
    {
        require_backend_support<support_requirement::timer, Backend>();
    }

    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs the timer (does not start it).
    /// @param[in] callback  Function to call on expiry.
    /// @param[in] arg       Opaque argument forwarded to callback.
    /// @param[in] period    Timer period.
    /// @param[in] mode      one_shot or periodic.
    /// @param[in] name      Debug name (may be nullptr).
    /// @complexity O(1)
    timer(osal_timer_callback_t callback, void* arg, milliseconds period, timer_mode mode = timer_mode::one_shot,
          const char* name = nullptr) noexcept
        : valid_(false)
    {
        if (callback == nullptr || period.count() <= 0)
        {
            initialization_status_ = error_code::invalid_argument;
        }
        else if constexpr (timer_backend<active_backend>)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(period);
            initialization_status_ =
                osal_timer_create(&handle_, name, callback, arg, ticks, mode == timer_mode::periodic);
        }
        record_initialization();
    }

    /// @brief Constructs from an immutable config (config may reside in FLASH).
    /// @param[in] cfg  Configuration — typically declared @c const / @c constexpr.
    /// @complexity O(1)
    explicit timer(const timer_config& cfg) noexcept : timer(cfg.callback, cfg.arg, cfg.period, cfg.mode, cfg.name) {}

    /// @brief Destructs the timer (stops if active).
    ~timer() noexcept { (void)close(); }

    timer(const timer&)            = delete;
    timer& operator=(const timer&) = delete;
    timer(timer&&)                 = delete;
    timer& operator=(timer&&)      = delete;

    // ---- control -----------------------------------------------------------

    /// @brief Starts the timer.
    /// @retval osal::ok()                The timer was started successfully.
    /// @retval error_code::not_supported Timers are unavailable on the active backend.
    [[nodiscard]] result start() noexcept
    {
        if constexpr (timer_backend<active_backend>)
        {
            if (!valid_)
            {
                return error_code::not_initialized;
            }
            return osal_timer_start(&handle_);
        }
        return error_code::not_supported;
    }

    /// @brief Stops the timer.
    [[nodiscard]] result stop() noexcept
    {
        if constexpr (timer_backend<active_backend>)
        {
            if (!valid_)
            {
                return error_code::not_initialized;
            }
            return osal_timer_stop(&handle_);
        }
        return error_code::not_supported;
    }

    /// @brief Resets the timer (restarts its period from now).
    [[nodiscard]] result reset() noexcept
    {
        if constexpr (timer_backend<active_backend>)
        {
            if (!valid_)
            {
                return error_code::not_initialized;
            }
            return osal_timer_reset(&handle_);
        }
        return error_code::not_supported;
    }

    /// @brief Changes the timer period while preserving its mode and active state.
    /// @details An inactive timer remains inactive. An active timer begins a new
    ///          interval at the new period. One-shot/periodic mode is preserved.
    ///          Backends unable to guarantee this contract return not_supported.
    /// @param[in] period  New period.
    [[nodiscard]] result set_period(milliseconds period) noexcept
    {
        if (period.count() <= 0)
        {
            return error_code::invalid_argument;
        }
        if constexpr (timer_backend<active_backend>)
        {
            if (!valid_)
            {
                return error_code::not_initialized;
            }
            const tick_t ticks = clock_utils::ms_to_ticks(period);
            return osal_timer_set_period(&handle_, ticks);
        }
        return error_code::not_supported;
    }

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the timer was successfully created.
    [[nodiscard]] bool   valid() const noexcept { return valid_; }
    [[nodiscard]] result initialization_status() const noexcept { return initialization_status_; }

    /// @brief Stops and destroys the timer; a destroy failure retains ownership.
    /// @details Destruction itself stops the timer. Call only from thread context
    ///          after quiescing other users; the destructor remains best-effort.
    ///          On failure keep this object and its callback context alive.
    [[nodiscard]] result close() noexcept
    {
        if (!valid_)
        {
            return ok();
        }
        const result status = osal_timer_destroy(&handle_);
        diagnostics(diagnostic_resource::timer).attempt(status);
        if (status.ok())
        {
            valid_ = false;
            diagnostics(diagnostic_resource::timer).release();
        }
        return status;
    }
    /// @brief Returns true if the timer is currently running.
    [[nodiscard]] bool is_active() const noexcept
    {
        if constexpr (timer_backend<active_backend>)
        {
            return valid_ && osal_timer_is_active(&handle_);
        }
        return false;
    }

private:
    void record_initialization() noexcept
    {
        valid_         = initialization_status_.ok();
        auto& counters = diagnostics(diagnostic_resource::timer);
        counters.attempt(initialization_status_);
        if (valid_)
        {
            counters.acquire();
        }
    }

    result                        initialization_status_{error_code::not_supported};
    bool                          valid_;
    active_traits::timer_handle_t handle_{};
};

/// @} // osal_timer

}  // namespace osal
