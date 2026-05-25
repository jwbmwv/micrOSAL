// SPDX-License-Identifier: Apache-2.0
/// @file thread.hpp
/// @brief OSAL thread creation, lifecycle, and policy
/// @details Provides osal::thread — a non-allocating, noexcept wrapper around
///          the backend task/thread primitive.
///
///          Stack:
///          - Static-stack embedded backends expect caller-provided storage.
///          - POSIX-family backends may instead use a native pthread stack
///            when no explicit stack buffer is supplied.
///          - In FreeRTOS static mode, MicrOSAL stores the TCB at the start of
///            the caller-provided buffer and uses the remainder as stack space.
///          - Stack size must be >= active_traits::default_stack_bytes when the
///            active backend enforces a minimum.
///
///          Function signature:
///          - Thread entry points are plain C-compatible function pointers to
///            avoid std::function (which may allocate).
///          - Signature: void (*entry)(void* arg)
///
///          Lifecycle:
///          - Threads start immediately on create() unless the backend requires
///            an explicit resume().
///          - join() / detach() follow POSIX-style semantics on supporting
///            backends.
///          - Timed join (join_for, join_until) is available when
///            capabilities<active_backend>::has_timed_join == true.
///          - Bare-metal records tasks but does not switch to independent
///            stacks; all tasks execute on the scheduler's call stack.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_thread
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "types.hpp"
#include <cstdint>
#include <cstddef>

// Backend function declarations.
extern "C"
{
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t affinity, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept;

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout_ticks) noexcept;

    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept;

    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept;

    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t                      affinity) noexcept;

    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept;

    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept;

    void osal_thread_yield() noexcept;
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept;

    osal::result osal_task_notify(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept;
    osal::result osal_task_notify_isr(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept;
    osal::result osal_task_notify_wait(std::uint32_t clear_on_entry, std::uint32_t clear_on_exit,
                                       std::uint32_t* value_out, osal::tick_t timeout_ticks) noexcept;
    osal::result osal_thread_get_id(const osal::active_traits::thread_handle_t* handle,
                                    osal::thread_id_t*                          out_id) noexcept;
    osal::result osal_thread_get_priority(const osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t*                           out_priority) noexcept;
    osal::result osal_thread_get_affinity(const osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t*                           out_affinity) noexcept;
    osal::result osal_thread_current_cpu(std::uint32_t* out_cpu) noexcept;
    osal::result osal_thread_stack_low_watermark_bytes(const osal::active_traits::thread_handle_t* handle,
                                                       std::size_t*                                out_bytes) noexcept;
    osal::result osal_thread_execution_time_us(const osal::active_traits::thread_handle_t* handle,
                                               std::int64_t*                               out_us) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_thread OSAL Thread
/// @brief Thread creation, lifecycle, priorities, and affinity.
/// @{

/// @brief Configuration block passed to thread::create().
/// @details All members are trivial types, so the struct is
///          constexpr-constructible.  Declare as @c const to place in
///          .rodata (FLASH) — function and stack pointers target RAM but
///          the descriptor itself lives in FLASH.
///
///          @code
///          alignas(16) static uint8_t stack[4096];
///          const osal::thread_config cfg{
///              my_task, nullptr, osal::PRIORITY_NORMAL,
///              osal::AFFINITY_ANY, stack, sizeof(stack), "sensor"};
///          osal::thread t;
///          t.create(cfg);
///          @endcode
struct thread_config
{
    void (*entry)(void*)     = nullptr;          ///< Thread entry function. Must not be nullptr.
    void*        arg         = nullptr;          ///< Argument passed to entry.
    priority_t   priority    = PRIORITY_NORMAL;  ///< Initial priority.
    affinity_t   affinity    = AFFINITY_ANY;     ///< CPU affinity mask.
    void*        stack       = nullptr;          ///< Caller-provided stack buffer.
    stack_size_t stack_bytes = 0U;               ///< Size of stack buffer in bytes.
    const char*  name        = nullptr;          ///< Debug name (may be nullptr).
};

/// @brief OSAL thread.
/// @details Non-copyable, non-movable OS task wrapper.  The object stores the
///          native handle by value; no heap involvement.
///
/// @note  Static-stack embedded backends require a caller-managed stack via
///        thread_config::stack.  POSIX-family backends may accept a null stack
///        and use native pthread-managed storage instead.
class thread
{
public:
    /// @brief True when the active backend supports timed thread join.
    static constexpr bool supports_timed_join = supports_requirement<support_requirement::timed_join>;
    /// @brief True when the active backend supports CPU affinity changes.
    static constexpr bool supports_affinity = supports_requirement<support_requirement::thread_affinity>;
    /// @brief True when the active backend supports runtime priority changes.
    static constexpr bool supports_dynamic_priority =
        supports_requirement<support_requirement::dynamic_thread_priority>;
    /// @brief True when the active backend supports direct task notifications.
    static constexpr bool supports_task_notification = supports_requirement<support_requirement::task_notification>;
    /// @brief True when the active backend supports suspend/resume.
    static constexpr bool supports_suspend_resume = supports_requirement<support_requirement::thread_suspend_resume>;
    /// @brief True when the active backend reports per-thread stack watermark information.
    static constexpr bool supports_stack_watermark = supports_requirement<support_requirement::thread_stack_watermark>;
    /// @brief True when the active backend reports per-thread execution time.
    static constexpr bool supports_execution_time = supports_requirement<support_requirement::thread_execution_time>;
    /// @brief True when the active backend reports per-thread CPU-load statistics.
    static constexpr bool supports_cpu_load_stats = supports_requirement<support_requirement::thread_cpu_load_stats>;
    /// @brief True when the active backend reports the current running CPU/core.
    static constexpr bool supports_current_cpu = supports_requirement<support_requirement::current_cpu_query>;

    /// @brief Enforce timed-join support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_timed_join_support()
    {
        require_backend_support<support_requirement::timed_join, Backend>();
    }

    /// @brief Enforce CPU-affinity support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_affinity_support()
    {
        require_backend_support<support_requirement::thread_affinity, Backend>();
    }

    /// @brief Enforce runtime-priority support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_dynamic_priority_support()
    {
        require_backend_support<support_requirement::dynamic_thread_priority, Backend>();
    }

    /// @brief Enforce task-notification support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_task_notification_support()
    {
        require_backend_support<support_requirement::task_notification, Backend>();
    }

    /// @brief Enforce suspend/resume support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_suspend_resume_support()
    {
        require_backend_support<support_requirement::thread_suspend_resume, Backend>();
    }

    /// @brief Enforce stack-watermark query support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_stack_watermark_support()
    {
        require_backend_support<support_requirement::thread_stack_watermark, Backend>();
    }

    /// @brief Enforce execution-time query support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_execution_time_support()
    {
        require_backend_support<support_requirement::thread_execution_time, Backend>();
    }

    /// @brief Enforce CPU-load query support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_cpu_load_stats_support()
    {
        require_backend_support<support_requirement::thread_cpu_load_stats, Backend>();
    }

    /// @brief Enforce current-CPU query support at compile time.
    template<typename Backend = active_backend>
    static consteval void require_current_cpu_support()
    {
        require_backend_support<support_requirement::current_cpu_query, Backend>();
    }

    // ---- construction / destruction ----------------------------------------

    /// @brief Default constructor.  Thread is not running.
    thread() noexcept = default;

    /// @brief Destructor.
    /// @warning Destroying a running thread that has not been joined or detached
    ///          is undefined behaviour.
    ~thread() noexcept = default;

    thread(const thread&)            = delete;
    thread& operator=(const thread&) = delete;
    thread(thread&&)                 = delete;
    thread& operator=(thread&&)      = delete;

    // ---- lifecycle ---------------------------------------------------------

    /// @brief Creates and starts the thread.
    /// @param cfg  Thread configuration.
    /// @return result::ok() on success.
    /// @complexity O(1)
    /// @blocking   Never (the new thread runs independently).
    result create(const thread_config& cfg) noexcept
    {
        const result r = osal_thread_create(&handle_, cfg.entry, cfg.arg, cfg.priority, cfg.affinity, cfg.stack,
                                            cfg.stack_bytes, cfg.name);
        valid_         = r.ok();
        return r;
    }

    /// @brief Joins the thread, blocking until it completes.
    /// @return result::ok() on success; error_code::not_supported if the backend
    ///         does not support join.
    /// @complexity O(1)
    /// @blocking   Blocks until the thread exits.
    result join() noexcept
    {
        const result r = osal_thread_join(&handle_, WAIT_FOREVER);
        if (r.ok())
        {
            valid_ = false;
        }
        return r;
    }

    /// @brief Joins with a timeout.
    /// @param timeout Maximum wait time.
    /// @return result::ok() if joined; error_code::timeout on expiry.
    ///         Returns error_code::not_supported on backends without timed join.
    /// @complexity O(1)
    /// @blocking   Up to timeout.
    result join_for(milliseconds timeout) noexcept
    {
        if constexpr (timed_join_backend<active_backend>)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            const result r     = osal_thread_join(&handle_, ticks);
            if (r.ok())
            {
                valid_ = false;
            }
            return r;
        }
        else
        {
            return error_code::not_supported;
        }
    }

    /// @brief Joins until an absolute deadline.
    /// @param deadline Absolute monotonic time point.
    /// @return result::ok() if joined before deadline; error_code::timeout on
    ///         expiry; error_code::not_supported if the backend lacks timed join.
    /// @details Loops internally so that tick-count saturation (for unusually
    ///          large deadlines) does not cause premature timeout returns.
    result join_until(monotonic_clock::time_point deadline) noexcept
    {
        const monotonic_deadline join_deadline = monotonic_deadline::at(deadline);
        for (;;)
        {
            if (join_deadline.expired())
            {
                return error_code::timeout;
            }
            const result r = join_for(join_deadline.remaining());
            // Joined (ok), unsupported backend, or any non-timeout error: propagate.
            if (r != error_code::timeout)
            {
                return r;
            }
            // Timed out: either deadline truly passed (checked at top of loop)
            // or ms_to_ticks saturated a large remaining duration — re-evaluate.
        }
    }

    /// @brief Detaches the thread; resources are reclaimed on exit.
    /// @return result::ok() on success.
    /// @complexity O(1)
    result detach() noexcept
    {
        const result r = osal_thread_detach(&handle_);
        if (r.ok())
        {
            valid_ = false;
        }
        return r;
    }

    // ---- attributes --------------------------------------------------------

    /// @brief Changes the thread priority.
    /// @param priority New priority value.
    /// @return result::ok() on success; error_code::not_supported if the backend
    ///         does not support dynamic priority changes.
    result set_priority(priority_t priority) noexcept
    {
        if constexpr (dynamic_thread_priority_backend<active_backend>)
        {
            return osal_thread_set_priority(&handle_, priority);
        }
        else
        {
            (void)priority;
            return error_code::not_supported;
        }
    }

    /// @brief Sets CPU affinity.
    /// @param affinity Bitmask of allowed CPUs.
    /// @return result::ok() on success; error_code::not_supported if the backend
    ///         does not support affinity.
    result set_affinity(affinity_t affinity) noexcept
    {
        if constexpr (thread_affinity_backend<active_backend>)
        {
            return osal_thread_set_affinity(&handle_, affinity);
        }
        else
        {
            (void)affinity;
            return error_code::not_supported;
        }
    }

    /// @brief Sends a direct-to-task notification to this thread.
    /// @param value  32-bit value; semantics are backend-defined (usually a bitmask or increment).
    /// @return result::ok() on success; error_code::not_supported if unavailable.
    result notify(std::uint32_t value = 0U) noexcept
    {
        if constexpr (task_notification_backend<active_backend>)
        {
            return osal_task_notify(&handle_, value);
        }
        else
        {
            (void)value;
            return error_code::not_supported;
        }
    }

    /// @brief Sends a direct-to-task notification from ISR context.
    /// @param value  32-bit value; semantics are backend-defined.
    /// @return result::ok() on success; error_code::not_supported if unavailable.
    result notify_isr(std::uint32_t value = 0U) noexcept
    {
        if constexpr (task_notification_backend<active_backend>)
        {
            return osal_task_notify_isr(&handle_, value);
        }
        else
        {
            (void)value;
            return error_code::not_supported;
        }
    }

    /// @brief Waits for a direct notification on the calling thread.
    /// @param timeout         Maximum wait time.
    /// @param[out] value_out  Receives the notification value (may be nullptr).
    /// @return result::ok() on notify; error_code::timeout on expiry;
    ///         error_code::not_supported if the backend lacks task notifications.
    static result wait_for_notification(milliseconds timeout, std::uint32_t* value_out = nullptr) noexcept
    {
        if constexpr (task_notification_backend<active_backend>)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            return osal_task_notify_wait(0U, 0xFFFFFFFFU, value_out, ticks);
        }
        else
        {
            (void)timeout;
            (void)value_out;
            return error_code::not_supported;
        }
    }

    /// @brief Suspends the target thread until resumed.
    /// @return result::ok() on success; error_code::not_supported if unsupported
    ///         by the active backend.
    [[nodiscard]] result suspend() noexcept { return osal_thread_suspend(&handle_); }

    /// @brief Resumes a previously suspended thread.
    /// @return result::ok() on success; error_code::not_supported if unsupported
    ///         by the active backend.
    [[nodiscard]] result resume() noexcept { return osal_thread_resume(&handle_); }

    // ---- query -------------------------------------------------------------

    /// @brief Returns an opaque identity token for this thread object.
    [[nodiscard]] thread_id_t id() const noexcept
    {
        if (!valid_)
        {
            return INVALID_THREAD_ID;
        }
        if constexpr (thread_identity_query_capability<active_backend>::value)
        {
            thread_id_t out = INVALID_THREAD_ID;
            if (osal_thread_get_id(&handle_, &out).ok())
            {
                return out;
            }
        }
        return INVALID_THREAD_ID;
    }

    /// @brief Queries the current priority of this thread.
    /// @param[out] out  Receives the priority on success.
    /// @return result::ok() on success; error_code::not_supported in the draft API.
    [[nodiscard]] result get_priority(priority_t& out) const noexcept
    {
        if constexpr (thread_priority_query_capability<active_backend>::value)
        {
            return osal_thread_get_priority(&handle_, &out);
        }
        out = PRIORITY_NORMAL;
        return error_code::not_supported;
    }

    /// @brief Queries the current affinity mask of this thread.
    /// @param[out] out  Receives the affinity mask on success.
    /// @return result::ok() on success; error_code::not_supported in the draft API.
    [[nodiscard]] result get_affinity(affinity_t& out) const noexcept
    {
        if constexpr (thread_affinity_query_capability<active_backend>::value)
        {
            return osal_thread_get_affinity(&handle_, &out);
        }
        out = AFFINITY_ANY;
        return error_code::not_supported;
    }

    /// @brief Queries the stack low-watermark for this thread.
    /// @param[out] out  Receives the minimum free stack observed in bytes.
    /// @return result::ok() on success; error_code::not_supported in the draft API.
    [[nodiscard]] result stack_low_watermark_bytes(std::size_t& out) const noexcept
    {
        if constexpr (supports_stack_watermark)
        {
            return osal_thread_stack_low_watermark_bytes(&handle_, &out);
        }
        out = 0U;
        return error_code::not_supported;
    }

    /// @brief Queries the accumulated execution time for this thread.
    /// @param[out] out  Receives the execution time on success.
    /// @return result::ok() on success; error_code::not_supported in the draft API.
    [[nodiscard]] result execution_time(microseconds& out) const noexcept
    {
        if constexpr (supports_execution_time)
        {
            std::int64_t us = 0;
            const result r  = osal_thread_execution_time_us(&handle_, &us);
            out             = microseconds{us};
            return r;
        }
        out = microseconds{0};
        return error_code::not_supported;
    }

    /// @brief Queries the CPU-load share for this thread.
    /// @param[out] out  Receives the load in permille on success.
    /// @return result::ok() on success; error_code::not_supported in the draft API.
    [[nodiscard]] result cpu_load_permille(load_permille_t& out) const noexcept
    {
        out = load_permille_t{0};
        return error_code::not_supported;
    }

    /// @brief Returns true if the thread was successfully created.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    // ---- static helpers (current thread) -----------------------------------

    /// @brief Returns the opaque identity token for the current thread.
    [[nodiscard]] static thread_id_t current_id() noexcept
    {
        if constexpr (thread_identity_query_capability<active_backend>::value)
        {
            thread_id_t out = INVALID_THREAD_ID;
            if (osal_thread_get_id(nullptr, &out).ok())
            {
                return out;
            }
        }
        return INVALID_THREAD_ID;
    }

    /// @brief Queries the current thread priority.
    [[nodiscard]] static result current_priority(priority_t& out) noexcept
    {
        if constexpr (thread_priority_query_capability<active_backend>::value)
        {
            return osal_thread_get_priority(nullptr, &out);
        }
        out = PRIORITY_NORMAL;
        return error_code::not_supported;
    }

    /// @brief Queries the current thread affinity mask.
    [[nodiscard]] static result current_affinity(affinity_t& out) noexcept
    {
        if constexpr (thread_affinity_query_capability<active_backend>::value)
        {
            return osal_thread_get_affinity(nullptr, &out);
        }
        out = AFFINITY_ANY;
        return error_code::not_supported;
    }

    /// @brief Queries the current CPU/core index for the calling thread.
    [[nodiscard]] static result current_cpu(std::uint32_t& out) noexcept
    {
        if constexpr (supports_current_cpu)
        {
            return osal_thread_current_cpu(&out);
        }
        out = 0U;
        return error_code::not_supported;
    }

    /// @brief Queries the stack low-watermark of the current thread.
    [[nodiscard]] static result current_stack_low_watermark_bytes(std::size_t& out) noexcept
    {
        if constexpr (supports_stack_watermark)
        {
            return osal_thread_stack_low_watermark_bytes(nullptr, &out);
        }
        out = 0U;
        return error_code::not_supported;
    }

    /// @brief Queries the accumulated execution time of the current thread.
    [[nodiscard]] static result current_execution_time(microseconds& out) noexcept
    {
        if constexpr (supports_execution_time)
        {
            std::int64_t us = 0;
            const result r  = osal_thread_execution_time_us(nullptr, &us);
            out             = microseconds{us};
            return r;
        }
        out = microseconds{0};
        return error_code::not_supported;
    }

    /// @brief Queries the CPU-load share of the current thread.
    [[nodiscard]] static result current_cpu_load_permille(load_permille_t& out) noexcept
    {
        out = load_permille_t{0};
        return error_code::not_supported;
    }

    /// @brief Yields the current thread's time-slice.
    /// @complexity O(1)
    static void yield() noexcept { osal_thread_yield(); }

    /// @brief Sleeps for the specified duration.
    /// @details Non-positive durations return immediately.  Durations that exceed
    ///          the @c uint32_t range of @c osal_thread_sleep_ms are saturated
    ///          rather than wrapping.
    /// @param duration Sleep duration.
    /// @complexity O(1)
    /// @blocking   Blocks for at least the requested duration.
    static void sleep_for(milliseconds duration) noexcept
    {
        if (duration.count() <= 0)
        {
            return;
        }
        // osal_thread_sleep_ms takes uint32_t — saturate to prevent silent wrap.
        constexpr auto kMax = static_cast<milliseconds::rep>(std::numeric_limits<std::uint32_t>::max());
        const auto     ms   = (duration.count() > kMax) ? kMax : duration.count();
        osal_thread_sleep_ms(static_cast<std::uint32_t>(ms));
    }

    /// @brief Sleeps until an absolute monotonic time point.
    /// @details If @p tp is already in the past, returns immediately.  Loops in
    ///          chunks so that (a) uint32_t overflow in @c osal_thread_sleep_ms
    ///          is impossible and (b) early wakeups are corrected before return.
    /// @param tp  Absolute monotonic deadline.
    /// @complexity O(duration / chunk)
    /// @blocking   Until @p tp is reached.
    static void sleep_until(monotonic_clock::time_point tp) noexcept
    {
        // Half of uint32_t max-ms gives comfortable headroom and avoids
        // the saturation path in sleep_for on every iteration.
        constexpr milliseconds kChunk{static_cast<milliseconds::rep>(std::numeric_limits<std::uint32_t>::max()) / 2};
        for (;;)
        {
            const auto now = monotonic_clock::now();
            if (tp <= now)
            {
                break;
            }
            const auto remaining = std::chrono::duration_cast<milliseconds>(tp - now);
            sleep_for((remaining > kChunk) ? kChunk : remaining);
        }
    }

private:
    bool                           valid_{false};
    active_traits::thread_handle_t handle_{};
};

/// @} // osal_thread

// ---------------------------------------------------------------------------
// osal::this_thread — idiomatic C++ current-thread operations
// ---------------------------------------------------------------------------

/// @brief Mirrors the std::this_thread interface for portable code.
/// @details Thin wrappers around the corresponding osal::thread static helpers.
///          Prefer these over the static class methods in application code to
///          keep the appearance consistent with the standard C++ thread API.
/// @ingroup osal_thread
namespace this_thread
{

/// @brief Returns the opaque identity token of the current thread.
[[nodiscard]] inline thread_id_t get_id() noexcept
{
    return thread::current_id();
}

/// @brief Queries the priority of the current thread.
[[nodiscard]] inline result get_priority(priority_t& out) noexcept
{
    return thread::current_priority(out);
}

/// @brief Queries the affinity mask of the current thread.
[[nodiscard]] inline result get_affinity(affinity_t& out) noexcept
{
    return thread::current_affinity(out);
}

/// @brief Queries the current CPU/core index of the calling thread.
[[nodiscard]] inline result get_cpu(std::uint32_t& out) noexcept
{
    return thread::current_cpu(out);
}

/// @brief Queries the stack low-watermark of the current thread.
[[nodiscard]] inline result stack_low_watermark_bytes(std::size_t& out) noexcept
{
    return thread::current_stack_low_watermark_bytes(out);
}

/// @brief Queries the execution time of the current thread.
[[nodiscard]] inline result execution_time(microseconds& out) noexcept
{
    return thread::current_execution_time(out);
}

/// @brief Queries the CPU-load share of the current thread.
[[nodiscard]] inline result cpu_load_permille(load_permille_t& out) noexcept
{
    return thread::current_cpu_load_permille(out);
}

/// @brief Yields the current thread's time-slice to the scheduler.
inline void yield() noexcept
{
    thread::yield();
}

/// @brief Blocks the current thread for at least @p d.
/// @param d  Duration to sleep.
inline void sleep_for(milliseconds d) noexcept
{
    thread::sleep_for(d);
}

/// @brief Blocks the current thread until the absolute time point @p tp.
/// @param tp  Monotonic deadline.  If in the past, returns immediately.
inline void sleep_until(monotonic_clock::time_point tp) noexcept
{
    thread::sleep_until(tp);
}

}  // namespace this_thread

}  // namespace osal
