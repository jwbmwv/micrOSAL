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

#include "condvar.hpp"
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
class notification
{
    static_assert(Slots > 0U, "osal::notification: Slots must be > 0.");

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
    /// @param value Value used by the selected @p action.
    /// @param action Update rule applied to the slot.
    /// @param index Slot index in the range `[0, Slots)`.
    /// @return `error_code::ok` on success, `error_code::would_block` for
    ///         `notification_action::no_overwrite` when the slot is already
    ///         pending, or `error_code::invalid_argument` for an invalid slot.
    result notify(std::uint32_t value = 0U, notification_action action = notification_action::overwrite,
                  std::size_t index = 0U) noexcept
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
    /// @param bits Bit mask to clear.
    /// @param index Slot index in the range `[0, Slots)`.
    /// @return `error_code::ok` on success or an argument/init error.
    result clear(std::uint32_t bits, std::size_t index = 0U) noexcept
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
    /// @param index Slot index in the range `[0, Slots)`.
    /// @return `error_code::ok` on success or an argument/init error.
    result reset(std::size_t index = 0U) noexcept
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
    /// @param index Slot index in the range `[0, Slots)`.
    /// @param timeout Maximum time to wait. A negative value waits forever.
    /// @param value_out Optional pointer that receives the slot value observed
    ///                  when the wait succeeds.
    /// @param clear_on_entry Bits cleared before starting the wait.
    /// @param clear_on_exit Bits cleared after the wait completes.
    /// @return `error_code::ok` on success, `error_code::timeout` on expiry, or
    ///         an argument/init error.
    /// @details A successful wait clears the slot's pending bit before it
    ///          returns.
    result wait(std::size_t index, milliseconds timeout = milliseconds{-1}, std::uint32_t* value_out = nullptr,
                std::uint32_t clear_on_entry = 0U, std::uint32_t clear_on_exit = 0xFFFFFFFFU) noexcept
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

        const auto pred  = [&slot]() noexcept { return slot.pending; };
        bool       ready = true;
        if (!slot.pending)
        {
            if (timeout.count() < 0)
            {
                cv_.wait(mtx_, pred);
            }
            else
            {
                ready = cv_.wait_for(mtx_, timeout, pred);
            }
        }

        if (!ready)
        {
            mtx_.unlock();
            return error_code::timeout;
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
    /// @param index Slot index in the range `[0, Slots)`.
    /// @return `true` when the slot is pending.
    [[nodiscard]] bool pending(std::size_t index = 0U) const noexcept
    {
        if (!valid() || !index_valid(index))
        {
            return false;
        }

        auto* self = const_cast<notification*>(this);
        self->mtx_.lock();
        const bool is_pending = self->slots_[index].pending;
        self->mtx_.unlock();
        return is_pending;
    }

    /// @brief Read the current slot value without consuming pending state.
    /// @param index Slot index in the range `[0, Slots)`.
    /// @return Current 32-bit slot value, or `0` for an invalid slot.
    [[nodiscard]] std::uint32_t peek(std::size_t index = 0U) const noexcept
    {
        if (!valid() || !index_valid(index))
        {
            return 0U;
        }

        auto* self = const_cast<notification*>(this);
        self->mtx_.lock();
        const std::uint32_t value = self->slots_[index].value;
        self->mtx_.unlock();
        return value;
    }

private:
    struct slot_state
    {
        std::uint32_t value   = 0U;
        bool          pending = false;
    };

    [[nodiscard]] static constexpr bool index_valid(std::size_t index) noexcept { return index < Slots; }

    mutable mutex mtx_;
    condvar       cv_;
    slot_state    slots_[Slots]{};
};

/// @} // osal_notification

}  // namespace osal
