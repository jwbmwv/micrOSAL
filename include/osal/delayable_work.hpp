// SPDX-License-Identifier: Apache-2.0
/// @file delayable_work.hpp
/// @brief OSAL delayable work item built on timer + work_queue
/// @details Provides osal::delayable_work, a portable helper for scheduling a
///          work_queue callback to run after a delay in thread context.
///
///          The implementation is intentionally conservative:
///          - only one delayed submission may be armed at a time
///          - once the timer has handed the callback to the work queue,
///            cancel/reschedule return would_block until the callback runs
///          - destroying a pending delayable_work is undefined behaviour;
///            flush() or cancel() it first
///
///          This keeps the primitive allocation-free and avoids backend-
///          specific delayed-work contracts.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_delayable_work
#pragma once

#include "error.hpp"
#include "mutex.hpp"
#include "thread.hpp"
#include "timer.hpp"
#include "types.hpp"
#include "work_queue.hpp"

#include "detail/atomic_compat.hpp"
#include <cstddef>
#include <cstdint>

namespace osal
{

/// @defgroup osal_delayable_work OSAL Delayable Work
/// @brief Delayed submission of work_queue callbacks.
/// @{

/// @brief Delayable submission of a single callback onto a work queue.
/// @details This helper owns a one-shot timer that enqueues the bound callback
///          onto a target work queue once the delay expires. The callback
///          always executes in work-queue thread context, even on backends
///          where timer callbacks may run from ISR context.
///
///          The type is intentionally non-copyable and supports at most one
///          armed delay at a time.
///
/// @warning **Polling cost:** The `flush()` method uses busy-polling with
///          `thread::sleep_for()` to wait for the item to become idle.  This
///          is safe on all backends (including ISR-timer backends that cannot
///          signal a condition variable) but consumes CPU while waiting.
///          Prefer short timeouts or infrequent calls to `flush()`.
class delayable_work
{
public:
    /// @brief True when the active backend can safely support delayable work.
    static constexpr bool is_supported = supports_requirement<support_requirement::delayable_work>;

    /// @brief Enforce delayable-work support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_support()
    {
        require_backend_support<support_requirement::delayable_work, Backend>();
    }

    /// @brief Construct a delayable work item.
    /// @param queue Target work queue used to execute the callback.
    /// @param func Callback invoked when the delay expires.
    /// @param arg Opaque pointer passed back to @p func.
    /// @param name Optional debug name for the backing timer.
    delayable_work(work_queue& queue, work_func_t func, void* arg = nullptr, const char* name = "dw") noexcept
        : queue_(&queue), func_(func), arg_(arg),
          timer_{timer_thunk, this, milliseconds{1}, timer_mode::one_shot, name},
          state_(static_cast<std::uint8_t>(state::idle)), dispatch_error_(static_cast<std::int32_t>(error_code::ok)),
          dispatch_failed_(false), valid_(queue.valid() && (func != nullptr) && mtx_.valid() && timer_.valid() &&
                                          delayable_work_backend<active_backend>)
    {
    }

    ~delayable_work() noexcept = default;

    delayable_work(const delayable_work&)            = delete;
    delayable_work& operator=(const delayable_work&) = delete;
    delayable_work(delayable_work&&)                 = delete;
    delayable_work& operator=(delayable_work&&)      = delete;

    /// @brief Arm the work item to run after @p delay.
    /// @param delay Delay before the callback is submitted to the queue.
    ///              Zero or negative delays enqueue immediately.
    /// @return `error_code::ok` on success, `error_code::already_exists` if a
    ///         delay or queued callback is already pending, or a backend error
    ///         if the timer/work-queue operation fails.
    result schedule(milliseconds delay) noexcept
    {
        if (!valid_)
        {
            return error_code::not_initialized;
        }

        mutex::lock_guard lock{mtx_};
        if (load_state() != state::idle)
        {
            return error_code::already_exists;
        }

        clear_dispatch_error_locked();
        return (delay.count() <= 0) ? enqueue_locked() : arm_locked(delay);
    }

    /// @brief Replace any currently armed delay with a new one.
    /// @param delay New delay before queue submission.
    /// @return `error_code::ok` on success, `error_code::would_block` if the
    ///         callback has already reached the queue or is executing, or a
    ///         backend error if the timer/work-queue operation fails.
    result reschedule(milliseconds delay) noexcept
    {
        if (!valid_)
        {
            return error_code::not_initialized;
        }

        mutex::lock_guard lock{mtx_};
        state             current = load_state();
        if ((current == state::queued) || (current == state::running))
        {
            return error_code::would_block;
        }

        if (current == state::armed)
        {
            state expected = state::armed;
            if (!compare_exchange_state(expected, state::idle))
            {
                if ((expected == state::queued) || (expected == state::running))
                {
                    return error_code::would_block;
                }
            }
            else
            {
                (void)timer_.stop();
            }
        }

        clear_dispatch_error_locked();
        return (delay.count() <= 0) ? enqueue_locked() : arm_locked(delay);
    }

    /// @brief Cancel an armed delay before it reaches the work queue.
    /// @return `error_code::ok` if the item is idle or the timer was stopped,
    ///         `error_code::would_block` if the callback has already been
    ///         queued or is running, or a backend error from the timer stop.
    result cancel() noexcept
    {
        if (!valid_)
        {
            return error_code::not_initialized;
        }

        mutex::lock_guard lock{mtx_};
        if (load_state() == state::idle)
        {
            clear_dispatch_error_locked();
            return ok();
        }

        state expected = state::armed;
        if (!compare_exchange_state(expected, state::idle))
        {
            return (expected == state::idle) ? ok() : result{error_code::would_block};
        }

        clear_dispatch_error_locked();
        return timer_.stop();
    }

    /// @brief Wait until the item becomes idle.
    /// @param timeout Maximum time to wait. A negative value waits forever.
    /// @return `error_code::ok` once the item is idle, `error_code::timeout`
    ///         if the deadline expires first, or any deferred submission error
    ///         captured when the timer tried to queue the callback.
    /// @details This operation polls at the platform tick interval so it
    ///          remains safe on backends where timer callbacks run in ISR
    ///          context and cannot signal a condition variable directly.
    result flush(milliseconds timeout = milliseconds{5000}) noexcept
    {
        if (!valid_)
        {
            return error_code::not_initialized;
        }

        const auto deadline =
            (timeout.count() < 0) ? monotonic_clock::time_point::max() : (monotonic_clock::now() + timeout);
        while (load_state() != state::idle)
        {
            if ((timeout.count() >= 0) && (monotonic_clock::now() >= deadline))
            {
                return error_code::timeout;
            }
            thread::sleep_for(poll_interval());
        }

        return consume_dispatch_error();
    }

    /// @brief Report whether the helper initialized successfully.
    /// @return `true` when the mutex, timer, and callback binding are usable.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    /// @brief Report whether the work item is currently armed in the timer.
    /// @return `true` when waiting for timer expiry and not yet queued.
    [[nodiscard]] bool scheduled() const noexcept { return load_state() == state::armed; }

    /// @brief Report whether any work is still outstanding.
    /// @return `true` when the item is armed, queued, or running.
    [[nodiscard]] bool pending() const noexcept
    {
        const state current = load_state();
        return (current == state::armed) || (current == state::queued) || (current == state::running);
    }

    /// @brief Report whether the callback is executing on the work queue.
    /// @return `true` when the work callback is currently running.
    [[nodiscard]] bool running() const noexcept { return load_state() == state::running; }

private:
    enum class state : std::uint8_t
    {
        idle    = 0U,
        armed   = 1U,
        queued  = 2U,
        running = 3U,
    };

    static void timer_thunk(void* arg) noexcept
    {
        auto* self = static_cast<delayable_work*>(arg);
        if (self != nullptr)
        {
            self->on_timer();
        }
    }

    static void work_thunk(void* arg) noexcept
    {
        auto* self = static_cast<delayable_work*>(arg);
        if (self != nullptr)
        {
            self->run_work();
        }
    }

    void on_timer() noexcept
    {
        state expected = state::armed;
        if (!compare_exchange_state(expected, state::queued))
        {
            return;
        }

        const result r = active_capabilities::timer_callbacks_may_run_in_isr ? queue_->submit_from_isr(work_thunk, this)
                                                                             : queue_->submit(work_thunk, this);
        if (!r.ok())
        {
            store_state(state::idle);
            set_dispatch_error(r.code());
        }
    }

    void run_work() noexcept
    {
        state expected = state::queued;
        if (!compare_exchange_state(expected, state::running))
        {
            return;
        }

        func_(arg_);

        expected = state::running;
        (void)compare_exchange_state(expected, state::idle);
    }

    result arm_locked(milliseconds delay) noexcept
    {
        const result set_period = timer_.set_period(delay);
        if (!set_period.ok())
        {
            return set_period;
        }

        store_state(state::armed);
        const result start = timer_.start();
        if (!start.ok())
        {
            store_state(state::idle);
            return start;
        }
        return ok();
    }

    result enqueue_locked() noexcept
    {
        store_state(state::queued);
        const result r = queue_->submit(work_thunk, this);
        if (r.ok())
        {
            return ok();
        }

        store_state(state::idle);
        set_dispatch_error(r.code());
        return r;
    }

    void clear_dispatch_error_locked() noexcept
    {
        dispatch_error_.store(static_cast<std::int32_t>(error_code::ok), std::memory_order_release);
        dispatch_failed_.store(false, std::memory_order_release);
    }

    void set_dispatch_error(error_code code) noexcept
    {
        dispatch_error_.store(static_cast<std::int32_t>(code), std::memory_order_release);
        dispatch_failed_.store(true, std::memory_order_release);
    }

    [[nodiscard]] result consume_dispatch_error() noexcept
    {
        if (!dispatch_failed_.exchange(false, std::memory_order_acq_rel))
        {
            return ok();
        }

        const auto code = static_cast<error_code>(
            dispatch_error_.exchange(static_cast<std::int32_t>(error_code::ok), std::memory_order_acq_rel));
        return result{code};
    }

    [[nodiscard]] state load_state() const noexcept
    {
        return static_cast<state>(state_.load(std::memory_order_acquire));
    }

    void store_state(state value) noexcept
    {
        state_.store(static_cast<std::uint8_t>(value), std::memory_order_release);
    }

    bool compare_exchange_state(state& expected, state desired) noexcept
    {
        auto expected_raw = static_cast<std::uint8_t>(expected);
        bool exchanged    = false;
        exchanged         = state_.compare_exchange_strong(expected_raw, static_cast<std::uint8_t>(desired),
                                                           std::memory_order_acq_rel, std::memory_order_acquire);
        expected          = static_cast<state>(expected_raw);
        return exchanged;
    }

    [[nodiscard]] static milliseconds poll_interval() noexcept
    {
        const auto tick = clock_utils::ticks_to_ms(static_cast<tick_t>(1));
        return (tick.count() > 0) ? tick : milliseconds{1};
    }

    work_queue*               queue_;
    work_func_t               func_;
    void*                     arg_;
    mutable mutex             mtx_;
    timer                     timer_;
    std::atomic<std::uint8_t> state_;
    std::atomic<std::int32_t> dispatch_error_;
    std::atomic<bool>         dispatch_failed_;
    bool                      valid_{false};
};

/// @} // osal_delayable_work

}  // namespace osal
