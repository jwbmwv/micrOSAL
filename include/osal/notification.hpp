// SPDX-License-Identifier: Apache-2.0
/// @file notification.hpp
/// @brief OSAL indexed notification word with FreeRTOS-like actions
/// @details Provides osal::notification<Slots>, a small, allocation-free
///          synchronisation primitive that models the useful parts of richer
///          task-notification semantics without tying the API to a specific
///          thread backend.
///
///          Each slot contains:
///          - a 32-bit notification value
///          - a pending state bit
///
///          Supported actions:
///          - set_bits     : OR the new value into the slot
///          - increment    : increment the slot value
///          - overwrite    : replace the slot value unconditionally
///          - no_overwrite : fail with would_block if the slot is already pending
///
///          The implementation is fully portable and emulated with an OSAL
///          mutex + condition variable.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_notification
#pragma once

#include "concepts.hpp"
#include "condvar.hpp"
#include "detail/cpp_compat.hpp"
#include "error.hpp"
#include "mutex.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>

namespace osal
{

/// @defgroup osal_notification OSAL Notification
/// @brief Indexed 32-bit notification slots with pending state.
/// @{

/// @brief Notification update action.
enum class notification_action : std::uint8_t
{
    set_bits     = 0U,  ///< OR the provided value into the slot.
    increment    = 1U,  ///< Increment the slot value by 1.
    overwrite    = 2U,  ///< Replace the slot value unconditionally.
    no_overwrite = 3U,  ///< Fail if the slot already has a pending notification.
};

template<std::size_t Slots = 1U>
    requires valid_notification_slot_count<Slots>
class notification
{
public:
    /// @brief Number of notification slots stored in this instance.
    static constexpr std::size_t slot_count = Slots;

    /// @brief Construct an empty notification object.
    notification() noexcept  = default;
    ~notification() noexcept = default;

    notification(const notification&)            = delete;
    notification& operator=(const notification&) = delete;
    notification(notification&&)                 = delete;
    notification& operator=(notification&&)      = delete;

    /// @brief Report whether the internal mutex and condition variable exist.
    /// @return `true` when the notification object can be used.
    [[nodiscard]] bool valid() const noexcept { return mtx_.valid() && cv_.valid(); }

    /// @brief Apply a notification action to one slot.
    /// @param[in] value   Value used by the selected @p action.
    /// @param[in] action  Update rule applied to the slot.
    /// @param[in] index   Slot index in the range `[0, Slots)`.
    /// @retval error_code::ok              The notification was applied.
    /// @retval error_code::not_initialized The notification object is not initialized.
    /// @retval error_code::would_block     A no-overwrite notification found a pending slot.
    /// @retval error_code::invalid_argument The slot index or notification action is invalid.
    result notify(std::uint32_t value = 0U, notification_action action = notification_action::overwrite,
                  std::size_t index = 0U) const noexcept
    {
        if (!valid())
        {
            return error_code::not_initialized;
        }
        if (!index_valid(index))
        {
            return error_code::invalid_argument;
        }

        mutex::lock_guard lock{mtx_};
        auto&             slot = slots_[index];

        switch (action)
        {
        case notification_action::set_bits:
            slot.value |= value;
            slot.pending = true;
            break;
        case notification_action::increment:
            ++slot.value;
            slot.pending = true;
            break;
        case notification_action::overwrite:
            slot.value   = value;
            slot.pending = true;
            break;
        case notification_action::no_overwrite:
            if (slot.pending)
            {
                return error_code::would_block;
            }
            slot.value   = value;
            slot.pending = true;
            break;
        default:
            return error_code::invalid_argument;
        }

        cv_.notify_all();
        return ok();
    }

    /// @brief Clear a subset of bits in a slot without changing pending state.
    /// @param[in] bits   Bit mask to clear.
    /// @param[in] index  Slot index in the range `[0, Slots)`.
    /// @retval error_code::ok              The bits were cleared.
    /// @retval error_code::not_initialized The notification object is not initialized.
    /// @retval error_code::invalid_argument The slot index is invalid.
    result clear(std::uint32_t bits, std::size_t index = 0U) const noexcept
    {
        if (!valid())
        {
            return error_code::not_initialized;
        }
        if (!index_valid(index))
        {
            return error_code::invalid_argument;
        }

        mutex::lock_guard lock{mtx_};
        slots_[index].value &= ~bits;
        return ok();
    }

    /// @brief Reset a slot to value `0` with no pending notification.
    /// @param[in] index  Slot index in the range `[0, Slots)`.
    /// @retval error_code::ok              The slot was reset.
    /// @retval error_code::not_initialized The notification object is not initialized.
    /// @retval error_code::invalid_argument The slot index is invalid.
    result reset(std::size_t index = 0U) const noexcept
    {
        if (!valid())
        {
            return error_code::not_initialized;
        }
        if (!index_valid(index))
        {
            return error_code::invalid_argument;
        }

        mutex::lock_guard lock{mtx_};
        slots_[index] = slot_state{};
        return ok();
    }

    /// @brief Wait for one slot to become pending.
    /// @param[in] index       Slot index in the range `[0, Slots)`.
    /// @param[in] timeout     Maximum time to wait. A negative value waits forever.
    /// @param[out] value_out  Optional pointer that receives the slot value observed
    ///                  when the wait succeeds.
    /// @param[in] clear_on_entry  Bits cleared before starting the wait.
    /// @param[in] clear_on_exit   Bits cleared after the wait completes.
    /// @retval error_code::ok              The slot became pending.
    /// @retval error_code::not_initialized The notification object is not initialized.
    /// @retval error_code::invalid_argument The slot index is invalid.
    /// @retval error_code::timeout         The wait expired.
    /// @details A successful wait clears the slot's pending bit before it
    ///          returns.
    result wait(std::size_t index, milliseconds timeout = milliseconds{-1}, std::uint32_t* value_out = nullptr,
                std::uint32_t clear_on_entry = 0U, std::uint32_t clear_on_exit = 0xFFFFFFFFU) const noexcept
    {
        if (!valid())
        {
            return error_code::not_initialized;
        }
        if (!index_valid(index))
        {
            return error_code::invalid_argument;
        }

        mtx_.lock();
        auto& slot = slots_[index];

        slot.value &= ~clear_on_entry;

        const auto pred = [&slot]() noexcept { return slot.pending; };
        if (!slot.pending)
        {
            if (timeout.count() < 0)
            {
                cv_.wait(mtx_, pred);
            }
            else if (!cv_.wait_for(mtx_, timeout, pred))
            {
                mtx_.unlock();
                return error_code::timeout;
            }
        }

        if (value_out != nullptr)
        {
            *value_out = slot.value;
        }

        slot.value &= ~clear_on_exit;
        slot.pending = false;
        mtx_.unlock();
        return ok();
    }

    /// @brief Report whether a slot has a pending notification.
    /// @param[in] index  Slot index in the range `[0, Slots)`.
    /// @return `true` when the slot is pending.
    [[nodiscard]] bool pending(std::size_t index = 0U) const noexcept
    {
        if (!valid() || !index_valid(index))
        {
            return false;
        }

        mutex::lock_guard lock{mtx_};
        return slots_[index].pending;
    }

    /// @brief Read the current slot value without consuming pending state.
    /// @param[in] index  Slot index in the range `[0, Slots)`.
    /// @return Current 32-bit slot value, or `0` for an invalid slot.
    [[nodiscard]] std::uint32_t peek(std::size_t index = 0U) const noexcept
    {
        if (!valid() || !index_valid(index))
        {
            return 0U;
        }

        mutex::lock_guard lock{mtx_};
        return slots_[index].value;
    }

private:
    struct slot_state
    {
        std::uint32_t value   = 0U;
        bool          pending = false;
    };

    [[nodiscard]] static constexpr bool index_valid(std::size_t index) noexcept { return index < Slots; }

    mutable mutex      mtx_;
    mutable condvar    cv_;
    mutable slot_state slots_[Slots]{};
};

/// @} // osal_notification

}  // namespace osal
