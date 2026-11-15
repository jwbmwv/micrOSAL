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
    /// @brief Creates a stopped backend timer.
    /// @param[out] handle        Receives the native timer handle.
    /// @param[in]  name          Optional debug name; follow backend name-lifetime requirements.
    /// @param[in]  cb            Non-null expiry callback; must not block.
    /// @param[in]  arg           Callback context; keep alive until successful destruction.
    /// @param[in]  period_ticks  Positive period in backend ticks.
    /// @param[in]  auto_reload   Whether the timer repeats after expiry.
    /// @return Success or the backend creation/capability error.
    /// @note Thread context only; callbacks use the backend timer thread or ISR context.
    // NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t cb, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept;

    /// @brief Stops and attempts to reclaim a backend timer.
    /// @param[in,out] handle  Timer handle to release.
    /// @pre Other users have quiesced; follow backend thread/callback-context restrictions.
    /// @return Success or a quiescence/capability/backend error; failure retains ownership.
    /// @note Keep callback context and handle storage alive on failure.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept;

    /// @brief Starts a backend timer.
    /// @param[in,out] handle  Initialised timer handle.
    /// @return Success or the backend start error.
    /// @note Thread context only; starting can make later reclamation unsupported.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept;

    /// @brief Stops a backend timer without destroying its handle.
    /// @param[in,out] handle  Initialised timer handle.
    /// @return Success or the backend stop error; success alone does not guarantee callback drainage.
    /// @note Thread context only.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept;

    /// @brief Restarts a backend timer's period.
    /// @param[in,out] handle  Initialised timer handle.
    /// @return Success or the backend reset error.
    /// @note Thread context only.
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept;

    /// @brief Changes a backend timer period while preserving mode and active state.
    /// @param[in,out] handle  Initialised timer handle.
    /// @param[in]     p       Positive replacement period in backend ticks.
    /// @return Success, not_supported if the contract cannot be honored, or another backend error.
    /// @note Thread context only; active timers begin a new interval.
    osal::result
    osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                          osal::tick_t p) noexcept;  // NOLINT(readability-inconsistent-declaration-parameter-name)

    /// @brief Samples whether a backend timer is active.
    /// @param[in] handle  Initialised timer handle.
    /// @return Observed activity; this is not a callback-quiescence guarantee.
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
    /// @tparam Backend  Backend whose timer support is checked.
    /// @details Compilation fails if the selected backend lacks timer support.
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
    /// @details Null callbacks or non-positive periods leave the timer invalid.
    ///          Inspect initialization_status() for validation/capability/backend errors.
    /// @note Callbacks must not block and may run in ISR context. Keep the callback
    ///       argument alive until close() succeeds; check can_reclaim_after_start.
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

    /// @brief Attempts close(), discarding cleanup errors.
    /// @pre Other users have quiesced. Explicitly handle close() failures before
    ///      destroying this object or its callback context.
    ~timer() noexcept { (void)close(); }

    /// @brief Copy construction is disabled.
    timer(const timer&) = delete;
    /// @brief Copy assignment is disabled.
    timer& operator=(const timer&) = delete;
    /// @brief Move construction is disabled.
    timer(timer&&) = delete;
    /// @brief Move assignment is disabled.
    timer& operator=(timer&&) = delete;

    // ---- control -----------------------------------------------------------

    /// @brief Starts the timer.
    /// @retval osal::ok()                   The timer was started successfully.
    /// @retval error_code::not_supported    Timers are unavailable on the active backend.
    /// @retval error_code::not_initialized  The timer is invalid on a timer-capable backend.
    /// @return Otherwise the backend start error.
    /// @note Thread context only; starting may prevent subsequent reclamation.
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
    /// @retval error_code::not_supported    Timers are unavailable on the active backend.
    /// @retval error_code::not_initialized  The timer is invalid on a timer-capable backend.
    /// @return Otherwise the backend stop result.
    /// @note Thread context only. Stopping alone does not prove callback drainage or reclaimability.
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
    /// @retval error_code::not_supported    Timers are unavailable on the active backend.
    /// @retval error_code::not_initialized  The timer is invalid on a timer-capable backend.
    /// @return Otherwise the backend reset result.
    /// @note Thread context only.
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
    /// @retval error_code::invalid_argument  The period is not positive.
    /// @retval error_code::not_initialized   The timer is invalid on a timer-capable backend.
    /// @retval error_code::not_supported     Timers or this period-change contract are unsupported.
    /// @return Otherwise the backend period-change result.
    /// @note Thread context only.
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
    /// @return Whether this object currently owns a valid timer.
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    /// @brief Reports the original validation/creation result.
    /// @return Initialization status, unchanged by later operations or close().
    [[nodiscard]] result initialization_status() const noexcept { return initialization_status_; }

    /// @brief Stops and destroys the timer; a destroy failure retains ownership.
    /// @details Destruction itself stops the timer. Call only from thread context
    ///          after quiescing other users; the destructor remains best-effort.
    ///          On failure keep this object and its callback context alive.
    /// @return Success if already closed or destroyed, otherwise the backend destruction error.
    /// @note Consult can_reclaim_after_start before starting timers whose storage must be reclaimed.
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
    /// @return Observed backend activity, or false for an invalid/unsupported timer.
    /// @note An inactive timer can still have an in-flight callback; use close() for reclamation.
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
