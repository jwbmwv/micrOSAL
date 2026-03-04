// SPDX-License-Identifier: Apache-2.0
/// @file event_flags.hpp
/// @brief OSAL event flags group — native or emulated
/// @details Provides osal::event_flags for inter-task synchronisation using a
///          32-bit bitmask.  Each bit represents one independent event.
///
///          Native mode (FreeRTOS, ThreadX, PX5 — has_native_event_flags == true):
///          - Uses the RTOS event group / event flags API directly.
///
///          Emulated mode (POSIX, Linux, Zephyr, bare-metal):
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
    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept;

    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept;

    /// @brief Sets bits via OR.
    osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept;

    /// @brief Clears bits.
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept;

    /// @brief Returns current flag bits without waiting.
    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept;

    /// @brief Waits until any of the requested bits are set.
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual_bits,
                                           bool clear_on_exit, osal::tick_t timeout_ticks) noexcept;

    /// @brief Waits until ALL of the requested bits are set.
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual_bits,
                                           bool clear_on_exit, osal::tick_t timeout_ticks) noexcept;

    /// @brief ISR-safe set (only on capable backends).
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
class event_flags
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and initialises the event flag group (all bits clear).
    /// @complexity O(1)
    event_flags() noexcept : valid_(false), handle_{} { valid_ = osal_event_flags_create(&handle_).ok(); }

    /// @brief Destructs the event flag group.
    ~event_flags() noexcept
    {
        if (valid_)
        {
            (void)osal_event_flags_destroy(&handle_);
            valid_ = false;
        }
    }

    event_flags(const event_flags&)            = delete;
    event_flags& operator=(const event_flags&) = delete;
    event_flags(event_flags&&)                 = delete;
    event_flags& operator=(event_flags&&)      = delete;

    // ---- control -----------------------------------------------------------

    /// @brief Sets one or more bits (OR operation).
    /// @param bits  Bitmask of bits to set.
    /// @return result::ok() on success.
    result set(event_bits_t bits) noexcept { return osal_event_flags_set(&handle_, bits); }

    /// @brief Sets bits from ISR context.
    /// @param bits  Bitmask to set.
    /// @warning Only safe when capabilities<active_backend>::has_isr_event_flags.
    result set_isr(event_bits_t bits) noexcept { return osal_event_flags_set_isr(&handle_, bits); }

    /// @brief Clears one or more bits.
    /// @param bits  Bitmask of bits to clear.
    result clear(event_bits_t bits) noexcept { return osal_event_flags_clear(&handle_, bits); }

    /// @brief Returns the current bitmask (non-blocking snapshot).
    [[nodiscard]] event_bits_t get() const noexcept { return osal_event_flags_get(&handle_); }

    // ---- wait --------------------------------------------------------------

    /// @brief Waits until ANY of the specified bits are set.
    /// @param      bits          Bits to wait for.
    /// @param[out] actual_bits   Value of the group when unblocked (may be nullptr).
    /// @param      clear_on_exit If true, clears the waited bits after returning.
    /// @param      timeout       Maximum wait time.
    /// @return result::ok() if any bit was set; error_code::timeout on expiry.
    /// @complexity O(1)
    /// @blocking   Potentially blocking.
    result wait_any(event_bits_t bits, event_bits_t* actual_bits = nullptr, bool clear_on_exit = false,
                    milliseconds timeout = milliseconds{-1}) noexcept
    {
        const tick_t  ticks = (timeout.count() < 0) ? WAIT_FOREVER : clock_utils::ms_to_ticks(timeout);
        event_bits_t  dummy{0U};
        event_bits_t* dst = (actual_bits != nullptr) ? actual_bits : &dummy;
        return osal_event_flags_wait_any(&handle_, bits, dst, clear_on_exit, ticks);
    }

    /// @brief Waits until ALL of the specified bits are set.
    /// @param      bits          Bits to wait for.
    /// @param[out] actual_bits   Value of the group when unblocked (may be nullptr).
    /// @param      clear_on_exit If true, atomically clears the waited bits.
    /// @param      timeout       Maximum wait time.
    /// @return result::ok() if all bits were set; error_code::timeout on expiry.
    result wait_all(event_bits_t bits, event_bits_t* actual_bits = nullptr, bool clear_on_exit = false,
                    milliseconds timeout = milliseconds{-1}) noexcept
    {
        const tick_t  ticks = (timeout.count() < 0) ? WAIT_FOREVER : clock_utils::ms_to_ticks(timeout);
        event_bits_t  dummy{0U};
        event_bits_t* dst = (actual_bits != nullptr) ? actual_bits : &dummy;
        return osal_event_flags_wait_all(&handle_, bits, dst, clear_on_exit, ticks);
    }

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the group was successfully initialised.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    bool                                valid_;
    active_traits::event_flags_handle_t handle_;
};

/// @} // osal_event_flags

}  // namespace osal
