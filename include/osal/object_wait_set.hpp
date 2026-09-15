// SPDX-License-Identifier: Apache-2.0
/// @file object_wait_set.hpp
/// @brief OSAL object wait multiplexing for portable RTOS-style waits
/// @details Provides osal::object_wait_set, a portable polling-based wait-set
///          for OSAL objects with non-destructive readiness queries.
///
///          This complements osal::wait_set:
///          - osal::wait_set        : OS/native descriptors (epoll, poll, PX5)
///          - osal::object_wait_set : OSAL objects (queue, mailbox, event_flags,
///                                    stream/message buffer, notification,
///                                    work_queue, delayable_work)
///
///          The implementation is intentionally simple and portable.  It does
///          not consume ready data; it only reports which registered objects are
///          currently ready according to their snapshot state.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_object_wait_set
#pragma once

#include "delayable_work.hpp"
#include "detail/cpp_compat.hpp"
#include "event_flags.hpp"
#include "mailbox.hpp"
#include "message_buffer.hpp"
#include "notification.hpp"
#include "queue.hpp"
#include "stream_buffer.hpp"
#include "thread.hpp"
#include "types.hpp"
#include "work_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#include <expected>
#endif

#ifndef OSAL_OBJECT_WAIT_SET_MAX_ENTRIES
#define OSAL_OBJECT_WAIT_SET_MAX_ENTRIES 16U
#endif

#ifndef OSAL_OBJECT_WAIT_SET_POLL_INTERVAL_MS
#define OSAL_OBJECT_WAIT_SET_POLL_INTERVAL_MS 1U
#endif

namespace osal
{

/// @defgroup osal_object_wait_set OSAL Object Wait-Set
/// @brief Polling-based multiplex wait for OSAL objects.
/// @{

/// @brief Polling wait set for OSAL objects with non-destructive readiness.
/// @details Each registration associates a caller-defined integer identifier
///          with one OSAL object and a readiness predicate. The wait set does
///          not consume data or events unless the specific registration opts
///          into clear-on-exit behavior for event flags or notifications.
///
/// @warning **Polling cost:** This implementation polls registered objects at
///          a fixed interval (default `OSAL_OBJECT_WAIT_SET_POLL_INTERVAL_MS`
///          = 1 ms).  This is portable but trades CPU cycles for simplicity.
///          On battery-powered or latency-sensitive systems, consider tuning
///          the poll interval or using the native `wait_set` primitive when
///          the backend provides one (`capabilities<B>::has_wait_set`).
class object_wait_set
{
public:
    /// @brief Construct an empty object wait set.
    object_wait_set() noexcept  = default;
    ~object_wait_set() noexcept = default;

    object_wait_set(const object_wait_set&)            = delete;
    object_wait_set& operator=(const object_wait_set&) = delete;
    object_wait_set(object_wait_set&&)                 = delete;
    object_wait_set& operator=(object_wait_set&&)      = delete;

    /// @brief Report whether the wait set can be used.
    /// @return Always `true`; this type has no backend resources of its own.
    [[nodiscard]] static constexpr bool valid() noexcept { return true; }

    /// @brief Register a queue and mark it ready when non-empty.
    /// @param[in] q   Queue to observe.
    /// @param[in] id  Caller-defined identifier returned by wait().
    /// @retval error_code::ok              The queue was registered.
    /// @retval error_code::not_initialized The queue is not initialized.
    /// @retval error_code::overflow        The wait-set has reached its entry limit.
    template<queue_element T, queue_depth_t N>
    result add(queue<T, N>& q, int id) noexcept
    {
        if (!q.valid())
        {
            return error_code::not_initialized;
        }
        return add_entry(id, wait_kind::queue_ready, &q, 0U, 0U, false, &queue_probe<T, N>);
    }

    /// @brief Register a mailbox and mark it ready when non-empty.
    /// @param[in] mb  Mailbox to observe.
    /// @param[in] id  Caller-defined identifier returned by wait().
    /// @retval error_code::ok              The mailbox was registered.
    /// @retval error_code::not_initialized The mailbox is not initialized.
    /// @retval error_code::overflow        The wait-set has reached its entry limit.
    template<queue_element T>
    result add(mailbox<T>& mb, int id) noexcept
    {
        if (!mb.valid())
        {
            return error_code::not_initialized;
        }
        return add_entry(id, wait_kind::mailbox_ready, &mb, 0U, 0U, false, &mailbox_probe<T>);
    }

    /// @brief Register an event-flags object and wait for any bit in @p bits.
    /// @param[in] flags          Event flags to observe.
    /// @param[in] bits           Bit mask that triggers readiness when any bit is set.
    /// @param[in] id             Caller-defined identifier returned by wait().
    /// @param[in] clear_on_exit  When `true`, the matched bits are cleared once the
    ///                      entry reports ready.
    /// @retval error_code::ok               The event-flags object was registered.
    /// @retval error_code::not_initialized  The event-flags object is not initialized.
    /// @retval error_code::invalid_argument @p bits is zero.
    /// @retval error_code::overflow         The wait-set has reached its entry limit.
    result add_any(event_flags& flags, event_bits_t bits, int id, bool clear_on_exit = false) noexcept
    {
        if (!flags.valid())
        {
            return error_code::not_initialized;
        }
        if (bits == 0U)
        {
            return error_code::invalid_argument;
        }
        return add_entry(id, wait_kind::event_any, &flags, bits, 0U, clear_on_exit, &event_probe);
    }

    /// @brief Register an event-flags object and wait for all bits in @p bits.
    /// @param[in] flags          Event flags to observe.
    /// @param[in] bits           Bit mask that must be fully set for readiness.
    /// @param[in] id             Caller-defined identifier returned by wait().
    /// @param[in] clear_on_exit  When `true`, the matched bits are cleared once the
    ///                      entry reports ready.
    /// @retval error_code::ok               The event-flags object was registered.
    /// @retval error_code::not_initialized  The event-flags object is not initialized.
    /// @retval error_code::invalid_argument @p bits is zero.
    /// @retval error_code::overflow         The wait-set has reached its entry limit.
    result add_all(event_flags& flags, event_bits_t bits, int id, bool clear_on_exit = false) noexcept
    {
        if (!flags.valid())
        {
            return error_code::not_initialized;
        }
        if (bits == 0U)
        {
            return error_code::invalid_argument;
        }
        return add_entry(id, wait_kind::event_all, &flags, bits, 0U, clear_on_exit, &event_probe);
    }

    /// @brief Register a stream buffer and require at least @p min_bytes ready.
    /// @param[in] sb         Stream buffer to observe.
    /// @param[in] min_bytes  Minimum readable byte count that triggers readiness.
    /// @param[in] id         Caller-defined identifier returned by wait().
    /// @retval error_code::ok               The stream buffer was registered.
    /// @retval error_code::not_initialized  The stream buffer is not initialized.
    /// @retval error_code::invalid_argument @p min_bytes is zero.
    /// @retval error_code::overflow         The wait-set has reached its entry limit.
    template<std::size_t N, std::size_t TriggerLevel>
    result add(stream_buffer<N, TriggerLevel>& sb, std::size_t min_bytes, int id) noexcept
    {
        if (!sb.valid())
        {
            return error_code::not_initialized;
        }
        if (min_bytes == 0U)
        {
            return error_code::invalid_argument;
        }
        return add_entry(id, wait_kind::stream_ready, &sb, 0U, min_bytes, false, &stream_probe<N, TriggerLevel>);
    }

    /// @brief Register a message buffer and mark it ready when a message exists.
    /// @param[in] mb  Message buffer to observe.
    /// @param[in] id  Caller-defined identifier returned by wait().
    /// @retval error_code::ok              The message buffer was registered.
    /// @retval error_code::not_initialized The message buffer is not initialized.
    /// @retval error_code::overflow        The wait-set has reached its entry limit.
    template<std::size_t N>
    result add(message_buffer<N>& mb, int id) noexcept
    {
        if (!mb.valid())
        {
            return error_code::not_initialized;
        }
        return add_entry(id, wait_kind::message_ready, &mb, 0U, 0U, false, &message_probe<N>);
    }

    /// @brief Register a work queue and mark it ready when items are pending.
    /// @param[in] wq  Work queue to observe.
    /// @param[in] id  Caller-defined identifier returned by wait().
    /// @retval error_code::ok              The work queue was registered.
    /// @retval error_code::not_initialized The work queue is not initialized.
    /// @retval error_code::overflow        The wait-set has reached its entry limit.
    result add(work_queue& wq, int id) noexcept
    {
        if (!wq.valid())
        {
            return error_code::not_initialized;
        }
        return add_entry(id, wait_kind::work_pending, &wq, 0U, 0U, false, &work_queue_probe);
    }

    /// @brief Register a delayable work item and mark it ready while pending.
    /// @param[in] work  Delayable work item to observe.
    /// @param[in] id    Caller-defined identifier returned by wait().
    /// @retval error_code::ok              The delayable work item was registered.
    /// @retval error_code::not_initialized The delayable work item is not initialized.
    /// @retval error_code::overflow        The wait-set has reached its entry limit.
    result add(delayable_work& work, int id) noexcept
    {
        if (!work.valid())
        {
            return error_code::not_initialized;
        }
        return add_entry(id, wait_kind::delayable_pending, &work, 0U, 0U, false, &delayable_work_probe);
    }

    /// @brief Register one notification slot and mark it ready when pending.
    /// @param[in] note           Notification object to observe.
    /// @param[in] index          Slot index inside @p note.
    /// @param[in] id             Caller-defined identifier returned by wait().
    /// @param[in] clear_on_exit  When `true`, the slot is reset after readiness is
    ///                      reported.
    /// @retval error_code::ok              The notification slot was registered.
    /// @retval error_code::not_initialized The notification object is not initialized.
    /// @retval error_code::overflow        The wait-set has reached its entry limit.
    template<std::size_t Slots>
    result add(notification<Slots>& note, std::size_t index, int id, bool clear_on_exit = false) noexcept
    {
        if (!note.valid())
        {
            return error_code::not_initialized;
        }
        return add_entry(id, wait_kind::notification_pending, &note, 0U, index, clear_on_exit,
                         &notification_probe<Slots>);
    }

    /// @brief Remove one previously registered identifier.
    /// @param[in] id  Identifier originally passed to add().
    /// @retval error_code::ok               The identifier was removed.
    /// @retval error_code::invalid_argument The identifier is not currently registered.
    result remove(int id) noexcept
    {
        for (auto& entry : entries_)
        {
            if (entry.active && entry.id == id)
            {
                entry = entry_state{};
                return ok();
            }
        }
        return error_code::invalid_argument;
    }

    /// @brief Wait until one or more registered objects become ready.
    /// @param[out] ready_ids  Output buffer that receives ready identifiers.
    /// @param[in] max_ready   Capacity of @p ready_ids.
    /// @param[out] n_ready    Number of identifiers written to @p ready_ids.
    /// @param[in] timeout     Maximum time to wait. A negative value waits forever and
    ///                zero performs a poll-only check.
    /// @retval error_code::ok               At least one entry is ready.
    /// @retval error_code::timeout          No entry became ready before the deadline.
    /// @retval error_code::invalid_argument @p ready_ids is null with a nonzero @p max_ready.
    result wait(int* ready_ids, std::size_t max_ready, std::size_t& n_ready,
                milliseconds timeout = milliseconds{-1}) noexcept
    {
        if ((ready_ids == nullptr) && (max_ready != 0U))
        {
            n_ready = 0U;
            return error_code::invalid_argument;
        }

        n_ready                                = 0U;
        const bool               no_wait       = timeout.count() == 0;
        const bool               forever       = timeout.count() < 0;
        const monotonic_deadline wait_deadline = monotonic_deadline::after(timeout);

        for (;;)
        {
            std::size_t local_ready = 0U;
            for (auto& entry : entries_)
            {
                if (!entry.active || (entry.probe == nullptr))
                {
                    continue;
                }

                if (entry.probe(entry))
                {
                    if (local_ready < max_ready)
                    {
                        ready_ids[local_ready] = entry.id;
                    }
                    ++local_ready;
                }
            }

            if (local_ready != 0U)
            {
                n_ready = (local_ready < max_ready) ? local_ready : max_ready;
                return ok();
            }

            if (no_wait)
            {
                return error_code::timeout;
            }

            if (!forever && wait_deadline.expired())
            {
                return error_code::timeout;
            }

            thread::sleep_for(milliseconds{OSAL_OBJECT_WAIT_SET_POLL_INTERVAL_MS});
        }
    }

    /// @brief Span-based wait overload for fixed-capacity caller buffers.
    result wait(std::span<int> ready_ids, std::size_t& n_ready, milliseconds timeout = milliseconds{-1}) noexcept
    {
        return wait(ready_ids.data(), ready_ids.size(), n_ready, timeout);
    }

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
    /// @brief Value-returning wait overload using std::expected and bounded_vector (C++23/26).
    /// @tparam MaxReady Positive output capacity; excess ready identifiers are truncated as in wait().
    /// @details Uses capacity-sized result storage directly, without a separate scratch array.
    template<std::size_t MaxReady = OSAL_OBJECT_WAIT_SET_MAX_ENTRIES>
        requires(MaxReady > 0U)
    [[nodiscard]] std::expected<detail::bounded_vector<int, MaxReady>, error_code>
    wait_expected(milliseconds timeout = milliseconds{-1}) noexcept
    {
        std::expected<detail::bounded_vector<int, MaxReady>, error_code> ready{std::in_place};
        (void)ready->resize(MaxReady);
        std::size_t  n_ready = 0U;
        const result status  = wait(ready->data(), MaxReady, n_ready, timeout);
        if (!status.ok())
        {
            return std::unexpected(status.code());
        }
        (void)ready->resize(n_ready);
        return ready;
    }
#endif

private:
    enum class wait_kind : std::uint8_t
    {
        queue_ready = 0U,
        mailbox_ready,
        event_any,
        event_all,
        stream_ready,
        message_ready,
        work_pending,
        delayable_pending,
        notification_pending,
    };

    struct entry_state;
    using probe_fn = bool (*)(entry_state&) noexcept;

    struct entry_state
    {
        bool          active        = false;
        int           id            = -1;
        wait_kind     kind          = wait_kind::queue_ready;
        void*         object        = nullptr;
        std::uint32_t bits          = 0U;
        std::size_t   arg           = 0U;
        bool          clear_on_exit = false;
        probe_fn      probe         = nullptr;
    };

    result add_entry(int id, wait_kind kind, void* object, std::uint32_t bits, std::size_t arg, bool clear_on_exit,
                     probe_fn probe) noexcept
    {
        if ((object == nullptr) || (probe == nullptr))
        {
            return error_code::invalid_argument;
        }

        for (auto& entry : entries_)
        {
            if (!entry.active)
            {
                entry.active        = true;
                entry.id            = id;
                entry.kind          = kind;
                entry.object        = object;
                entry.bits          = bits;
                entry.arg           = arg;
                entry.clear_on_exit = clear_on_exit;
                entry.probe         = probe;
                return ok();
            }
        }

        return error_code::overflow;
    }

    template<queue_element T, queue_depth_t N>
    static bool queue_probe(entry_state& entry) noexcept
    {
        OSAL_ASSUME(entry.object != nullptr);
        return !static_cast<queue<T, N>*>(entry.object)->empty();
    }

    template<queue_element T>
    static bool mailbox_probe(entry_state& entry) noexcept
    {
        OSAL_ASSUME(entry.object != nullptr);
        return !static_cast<mailbox<T>*>(entry.object)->empty();
    }

    static bool event_probe(entry_state& entry) noexcept
    {
        OSAL_ASSUME(entry.object != nullptr);
        auto* const flags = static_cast<event_flags*>(entry.object);
        const auto  bits  = flags->get();
        const bool  ready =
            (entry.kind == wait_kind::event_all) ? ((bits & entry.bits) == entry.bits) : ((bits & entry.bits) != 0U);
        if (ready && entry.clear_on_exit)
        {
            const auto matched = (entry.kind == wait_kind::event_all) ? entry.bits : (bits & entry.bits);
            (void)flags->clear(matched);
        }
        return ready;
    }

    template<std::size_t N, std::size_t TriggerLevel>
    static bool stream_probe(entry_state& entry) noexcept
    {
        OSAL_ASSUME(entry.object != nullptr);
        return static_cast<stream_buffer<N, TriggerLevel>*>(entry.object)->available() >= entry.arg;
    }

    template<std::size_t N>
    static bool message_probe(entry_state& entry) noexcept
    {
        OSAL_ASSUME(entry.object != nullptr);
        return !static_cast<message_buffer<N>*>(entry.object)->empty();
    }

    static bool work_queue_probe(entry_state& entry) noexcept
    {
        OSAL_ASSUME(entry.object != nullptr);
        return static_cast<work_queue*>(entry.object)->pending() > 0U;
    }

    static bool delayable_work_probe(entry_state& entry) noexcept
    {
        OSAL_ASSUME(entry.object != nullptr);
        return static_cast<delayable_work*>(entry.object)->pending();
    }

    template<std::size_t Slots>
    static bool notification_probe(entry_state& entry) noexcept
    {
        OSAL_ASSUME(entry.object != nullptr);
        auto* const note  = static_cast<notification<Slots>*>(entry.object);
        const bool  ready = note->pending(entry.arg);
        if (ready && entry.clear_on_exit)
        {
            (void)note->reset(entry.arg);
        }
        return ready;
    }

    entry_state entries_[OSAL_OBJECT_WAIT_SET_MAX_ENTRIES]{};
};

/// @} // osal_object_wait_set

}  // namespace osal
