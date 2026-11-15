// SPDX-License-Identifier: Apache-2.0
/// @file event_flags.hpp
/// @brief OSAL event flags group — native or emulated
/// @details Provides osal::event_flags for inter-task synchronisation using a
///          32-bit bitmask.  Each bit represents one independent event.
///
///          Native mode (FreeRTOS, Zephyr, ThreadX, PX5, Micrium, ChibiOS,
///          embOS, CMSIS-RTOS2 — has_native_event_flags == true):
///          - Uses the RTOS event group / event flags API directly.
///
///          Emulated mode (all other backends):
///          - Implemented with a mutex + condition variable (or spin-based on
///            bare-metal), providing identical semantics at higher cost.
///
///          All operations are O(1) and noexcept.
///          Bits are numbered 0 (LSB) to EVENT_BITS_MAX-1.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_event_flags
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstdint>

extern "C"
{
    /// @brief Create an event-flags group with all bits clear.
    /// @param[out] handle Handle storage to initialise.
    /// @return The backend result; consult its error code if creation fails.
    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept;

    /// @brief Destroy an event-flags group.
    /// @param[in,out] handle Handle to destroy.
    /// @return The backend result; consult its error code if destruction fails.
    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept;

    /// @brief Sets bits via OR.
    /// @param[in,out] handle Initialised event-flags handle.
    /// @param[in]     bits   Bits to set.
    /// @return The backend result; consult its error code if the operation fails.
    osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept;

    /// @brief Clears bits.
    /// @param[in,out] handle Initialised event-flags handle.
    /// @param[in]     bits   Bits to clear.
    /// @return The backend result; consult its error code if the operation fails.
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept;

    /// @brief Returns current flag bits without waiting.
    /// @param[in] handle Initialised event-flags handle.
    /// @return Snapshot of the current event bits.
    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept;

    /// @brief Waits until any of the requested bits are set.
    /// @param[in,out] handle        Initialised event-flags handle.
    /// @param[in]     wait_bits     Bits whose presence is tested.
    /// @param[out]    actual        Receives the observed bits.
    /// @param[in]     clear_on_exit Whether matched bits are cleared before return.
    /// @param[in]     timeout       Maximum wait in ticks; WAIT_FOREVER waits indefinitely.
    /// @return The backend result; consult its error code for timeout or other failures.
    // NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept;

    /// @brief Waits until ALL of the requested bits are set.
    /// @param[in,out] handle        Initialised event-flags handle.
    /// @param[in]     wait_bits     Bits that must all be present.
    /// @param[out]    actual        Receives the observed bits.
    /// @param[in]     clear_on_exit Whether matched bits are cleared before return.
    /// @param[in]     timeout       Maximum wait in ticks; WAIT_FOREVER waits indefinitely.
    /// @return The backend result; consult its error code for timeout or other failures.
    // NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept;

    /// @brief Set bits from interrupt context on capable backends.
    /// @param[in,out] handle Initialised event-flags handle.
    /// @param[in]     bits   Bits to set.
    /// @return The backend result; consult its error code if the operation fails.
    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_event_flags OSAL Event Flags
/// @brief 32-bit event flag group for inter-task signalling.
/// @{

/// @brief OSAL event flag group.
/// @details Stores 32 independently settable/clearable bits.
///          Uses native RTOS event group where available; emulates otherwise.
///          Blocking waits are thread-context operations. Use set_isr() only
///          when the active backend advertises ISR event-flag support.
class event_flags  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initializes the event-flag group with all bits clear.
    /// @complexity O(1)
    /// @details Check valid() before relying on successful initialization.
    event_flags() noexcept = default;  // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

    /// @brief Destroys the event-flag group if initialization succeeded.
    /// @details No concurrent operations or waiters may remain when the object
    ///          is destroyed.
    ~event_flags() noexcept
    {
        if (valid_)
        {
            (void)osal_event_flags_destroy(&handle_);
            valid_ = false;
        }
    }

    /// @brief Copy construction is disabled; the backend handle has one owner.
    event_flags(const event_flags&) = delete;
    /// @brief Copy assignment is disabled; the backend handle has one owner.
    event_flags& operator=(const event_flags&) = delete;
    /// @brief Move construction is disabled; the backend handle is not transferable.
    event_flags(event_flags&&) = delete;
    /// @brief Move assignment is disabled; the backend handle is not transferable.
    event_flags& operator=(event_flags&&) = delete;

    // ---- control -----------------------------------------------------------

    /// @brief Sets one or more bits (OR operation).
    /// @param[in] bits Bitmask of bits to set.
    /// @retval osal::ok() The bits were set successfully.
    /// @par Other errors
    /// Backend errors are returned unchanged.
    [[nodiscard]] result set(event_bits_t bits) noexcept { return osal_event_flags_set(&handle_, bits); }

    /// @brief Sets bits from ISR context.
    /// @param[in] bits Bitmask to set.
    /// @retval osal::ok()                Bits were set successfully.
    /// @retval error_code::not_supported The active backend lacks ISR-safe event-flag support.
    /// @warning Only call from an ISR when capabilities<active_backend>::has_isr_event_flags.
    /// @par Other errors
    /// Backend errors are returned unchanged.
    [[nodiscard]] result set_isr(event_bits_t bits) noexcept { return osal_event_flags_set_isr(&handle_, bits); }

    /// @brief Clears one or more bits.
    /// @param[in] bits Bitmask of bits to clear.
    /// @retval osal::ok() The bits were cleared successfully.
    /// @par Other errors
    /// Backend errors are returned unchanged.
    [[nodiscard]] result clear(event_bits_t bits) noexcept { return osal_event_flags_clear(&handle_, bits); }

    /// @brief Returns the current bitmask as a non-blocking snapshot.
    /// @return Current bits; this is a snapshot and may immediately become stale.
    [[nodiscard]] event_bits_t get() const noexcept { return osal_event_flags_get(&handle_); }

    // ---- wait --------------------------------------------------------------

    /// @brief Waits until ANY of the specified bits are set.
    /// @param[in]  bits          Bits to wait for; at least one must be set.
    /// @param[out] actual_bits   Optional output for the group value when unblocked.
    /// @param[in]  clear_on_exit Whether matched bits are cleared before return.
    /// @param[in]  timeout       Maximum wait; a negative duration waits indefinitely.
    /// @retval osal::ok()          At least one requested bit was set.
    /// @retval error_code::timeout The wait expired.
    /// @par Other errors
    /// Backend errors are returned unchanged.
    /// @details Thread context only; a negative duration waits indefinitely
    ///          and a non-positive duration does not wait.
    /// @complexity O(1)
    /// @blocking   Potentially blocking.
    [[nodiscard]] result wait_any(event_bits_t bits, event_bits_t* actual_bits = nullptr, bool clear_on_exit = false,
                                  milliseconds timeout = milliseconds{-1}) noexcept
    {
        const tick_t  ticks = (timeout.count() < 0) ? WAIT_FOREVER : clock_utils::ms_to_ticks(timeout);
        event_bits_t  dummy{0U};
        event_bits_t* dst = (actual_bits != nullptr) ? actual_bits : &dummy;
        return osal_event_flags_wait_any(&handle_, bits, dst, clear_on_exit, ticks);
    }

    /// @brief Waits until ALL of the specified bits are set.
    /// @param[in]  bits          Bits that must all be set.
    /// @param[out] actual_bits   Optional output for the group value when unblocked.
    /// @param[in]  clear_on_exit Whether matched bits are cleared before return.
    /// @param[in]  timeout       Maximum wait; a negative duration waits indefinitely.
    /// @retval osal::ok()          All requested bits were set.
    /// @retval error_code::timeout The wait expired.
    /// @par Other errors
    /// Backend errors are returned unchanged.
    /// @details Thread context only; a negative duration waits indefinitely
    ///          and a non-positive duration does not wait.
    [[nodiscard]] result wait_all(event_bits_t bits, event_bits_t* actual_bits = nullptr, bool clear_on_exit = false,
                                  milliseconds timeout = milliseconds{-1}) noexcept
    {
        const tick_t  ticks = (timeout.count() < 0) ? WAIT_FOREVER : clock_utils::ms_to_ticks(timeout);
        event_bits_t  dummy{0U};
        event_bits_t* dst = (actual_bits != nullptr) ? actual_bits : &dummy;
        return osal_event_flags_wait_all(&handle_, bits, dst, clear_on_exit, ticks);
    }

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the group was successfully initialised.
    /// @return true if construction initialized the group successfully.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    active_traits::event_flags_handle_t handle_{};
    bool                                valid_{osal_event_flags_create(&handle_).ok()};
};

/// @} // osal_event_flags

}  // namespace osal
