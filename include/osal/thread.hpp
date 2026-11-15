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
#include "diagnostics.hpp"
#include "types.hpp"
#include <cstdint>
#include <cstddef>

// Backend function declarations.
extern "C"
{
    /// @brief Creates a backend thread using the supplied entry point and scheduling attributes.
    /// @param[out] handle       Receives the native thread handle.
    /// @param[in]  entry        Non-null entry function.
    /// @param[in]  arg          Entry context; must outlive its use by the thread.
    /// @param[in]  priority     Initial OSAL priority.
    /// @param[in]  affinity     Requested CPU mask, subject to backend support.
    /// @param[out] stack        Caller-owned stack storage, or null where native stacks are supported.
    /// @param[in]  stack_bytes  Stack storage size, satisfying backend size/alignment requirements.
    /// @param[in]  name         Optional debug name, with backend-specific lifetime requirements.
    /// @return Success or the backend creation/configuration error.
    /// @note Thread context only. Caller-provided stack storage must outlive native thread use.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t affinity, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept;

    /// @brief Joins/releases a backend thread according to its native lifecycle policy.
    /// @param[in,out] handle         Initialised thread handle.
    /// @param[in]     timeout_ticks  Backend wait limit; WAIT_FOREVER requests an indefinite wait.
    /// @return Success or a timeout/capability/backend lifecycle error.
    /// @note Thread context only. On ThreadX this terminates/deletes the task rather than
    ///       proving cooperative completion; coordinate completion separately when required.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout_ticks) noexcept;

    /// @brief Detaches a backend thread where native lifecycle semantics permit it.
    /// @param[in,out] handle  Thread handle whose lifecycle ownership is relinquished.
    /// @return Success or a capability/backend detach error.
    /// @note Caller-owned entry context and stack storage must still outlive thread use.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept;

    /// @brief Changes a backend thread's priority.
    /// @param[in,out] handle    Initialised thread handle.
    /// @param[in]     priority  Requested OSAL priority.
    /// @return Success or a capability/backend priority error.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept;

    /// @brief Changes a backend thread's CPU affinity.
    /// @param[in,out] handle    Initialised thread handle.
    /// @param[in]     affinity  Requested mask of allowed CPUs.
    /// @return Success or a capability/backend affinity error.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t                      affinity) noexcept;

    /// @brief Suspends a backend thread.
    /// @param[in,out] handle  Initialised target thread handle.
    /// @return Success or a capability/backend suspension error.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept;

    /// @brief Resumes a suspended backend thread.
    /// @param[in,out] handle  Initialised target thread handle.
    /// @return Success or a capability/backend resumption error.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept;

    /// @brief Yields the calling thread's current time slice to the backend scheduler.
    void osal_thread_yield() noexcept;
    /// @brief Sleeps the calling thread for a backend-scheduled interval.
    /// @param[in] ms  Requested duration in milliseconds.
    /// @note Thread context only; actual scheduling/tick granularity is backend-specific.
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept;

    /// @brief Sends a backend-defined direct notification from thread context.
    /// @param[in,out] handle  Target thread handle.
    /// @param[in]     value   Backend-defined notification value.
    /// @return Success or a capability/backend notification error.
    osal::result osal_task_notify(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept;
    /// @brief Sends a direct notification through the backend ISR path.
    /// @param[in,out] handle  Target thread handle.
    /// @param[in]     value   Backend-defined notification value.
    /// @return Success or a capability/backend notification error.
    /// @pre The active backend's notification implementation supports the calling ISR context.
    osal::result osal_task_notify_isr(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept;
    /// @brief Waits for a direct notification on the calling thread.
    /// @param[in]  clear_on_entry  Notification bits to clear before waiting.
    /// @param[in]  clear_on_exit   Notification bits to clear after receipt.
    /// @param[out] value_out       Optional destination for the received value.
    /// @param[in]  timeout_ticks   Wait limit in ticks; NO_WAIT polls and WAIT_FOREVER waits indefinitely.
    /// @return Success or a timeout/capability/backend wait error.
    /// @note Thread context only; clearing semantics follow the backend notification implementation.
    osal::result osal_task_notify_wait(std::uint32_t clear_on_entry, std::uint32_t clear_on_exit,
                                       std::uint32_t* value_out, osal::tick_t timeout_ticks) noexcept;
    /// @brief Queries a thread identity token.
    /// @param[in]  handle  Target thread handle, or null for the calling thread.
    /// @param[out] out_id  Receives the identity on success.
    /// @return Success or a capability/backend query error.
    osal::result osal_thread_get_id(const osal::active_traits::thread_handle_t* handle,
                                    osal::thread_id_t*                          out_id) noexcept;
    /// @brief Queries a thread priority.
    /// @param[in]  handle        Target thread handle, or null for the calling thread.
    /// @param[out] out_priority  Receives the OSAL priority on success.
    /// @return Success or a capability/backend query error.
    osal::result osal_thread_get_priority(const osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t*                           out_priority) noexcept;
    /// @brief Queries a thread affinity mask.
    /// @param[in]  handle        Target thread handle, or null for the calling thread.
    /// @param[out] out_affinity  Receives the affinity mask on success.
    /// @return Success or a capability/backend query error.
    osal::result osal_thread_get_affinity(const osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t*                           out_affinity) noexcept;
    /// @brief Queries the calling thread's current CPU/core index.
    /// @param[out] out_cpu  Receives the zero-based CPU index on success.
    /// @return Success or a capability/backend query error.
    osal::result osal_thread_current_cpu(std::uint32_t* out_cpu) noexcept;
    /// @brief Queries a thread's minimum observed free stack.
    /// @param[in]  handle     Target thread handle, or null for the calling thread.
    /// @param[out] out_bytes  Receives the watermark in bytes on success.
    /// @return Success or a capability/backend query error.
    osal::result osal_thread_stack_low_watermark_bytes(const osal::active_traits::thread_handle_t* handle,
                                                       std::size_t*                                out_bytes) noexcept;
    /// @brief Queries accumulated thread execution time.
    /// @param[in]  handle  Target thread handle, or null for the calling thread.
    /// @param[out] out_us  Receives elapsed execution microseconds on success.
    /// @return Success or a capability/backend query error.
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
    /// @brief False for production bare-metal builds without a context-switch port.
    static constexpr bool is_supported = supports_requirement<support_requirement::thread>;

    /// @brief Enforces thread support at compile time.
    /// @tparam Backend  Backend whose thread capability is required.
    /// @details Compilation fails if the selected backend lacks thread support.
    template<typename Backend = active_backend>
    static consteval void require_support()
    {
        require_backend_support<support_requirement::thread, Backend>();
    }

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
    /// @tparam Backend  Backend required to support timed joins.
    template<typename Backend = active_backend>
    static consteval void require_timed_join_support()
    {
        require_backend_support<support_requirement::timed_join, Backend>();
    }

    /// @brief Enforce CPU-affinity support at compile time.
    /// @tparam Backend  Backend required to support CPU-affinity changes.
    template<typename Backend = active_backend>
    static consteval void require_affinity_support()
    {
        require_backend_support<support_requirement::thread_affinity, Backend>();
    }

    /// @brief Enforce runtime-priority support at compile time.
    /// @tparam Backend  Backend required to support runtime-priority changes.
    template<typename Backend = active_backend>
    static consteval void require_dynamic_priority_support()
    {
        require_backend_support<support_requirement::dynamic_thread_priority, Backend>();
    }

    /// @brief Enforce task-notification support at compile time.
    /// @tparam Backend  Backend required to support direct task notifications.
    template<typename Backend = active_backend>
    static consteval void require_task_notification_support()
    {
        require_backend_support<support_requirement::task_notification, Backend>();
    }

    /// @brief Enforce suspend/resume support at compile time.
    /// @tparam Backend  Backend required to support thread suspension/resumption.
    template<typename Backend = active_backend>
    static consteval void require_suspend_resume_support()
    {
        require_backend_support<support_requirement::thread_suspend_resume, Backend>();
    }

    /// @brief Enforce stack-watermark query support at compile time.
    /// @tparam Backend  Backend required to support stack-watermark queries.
    template<typename Backend = active_backend>
    static consteval void require_stack_watermark_support()
    {
        require_backend_support<support_requirement::thread_stack_watermark, Backend>();
    }

    /// @brief Enforce execution-time query support at compile time.
    /// @tparam Backend  Backend required to support execution-time queries.
    template<typename Backend = active_backend>
    static consteval void require_execution_time_support()
    {
        require_backend_support<support_requirement::thread_execution_time, Backend>();
    }

    /// @brief Enforce CPU-load query support at compile time.
    /// @tparam Backend  Backend required to support per-thread CPU-load queries.
    template<typename Backend = active_backend>
    static consteval void require_cpu_load_stats_support()
    {
        require_backend_support<support_requirement::thread_cpu_load_stats, Backend>();
    }

    /// @brief Enforce current-CPU query support at compile time.
    /// @tparam Backend  Backend required to support current-CPU queries.
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
    /// @details Does not implicitly join, detach, or terminate a thread.
    ~thread() noexcept = default;

    /// @brief Copy construction is disabled.
    thread(const thread&) = delete;
    /// @brief Copy assignment is disabled.
    thread& operator=(const thread&) = delete;
    /// @brief Move construction is disabled.
    thread(thread&&) = delete;
    /// @brief Move assignment is disabled.
    thread& operator=(thread&&) = delete;

    // ---- lifecycle ---------------------------------------------------------

    /// @brief Creates and starts the thread.
    /// @param[in] cfg  Thread configuration.
    /// @retval osal::ok()                    The thread was created successfully.
    /// @retval error_code::already_exists    This object already owns a thread.
    /// @retval error_code::invalid_argument  The entry function is null.
    /// @return Otherwise the backend creation error; inspect initialization_status() for the last attempt.
    /// @pre Stack storage and entry context remain alive throughout their native thread use.
    /// @note Thread context only. Backend stack/alignment and initial scheduling requirements apply.
    /// @complexity O(1)
    /// @blocking   Does not wait for thread completion.
    result create(const thread_config& cfg) noexcept
    {
        if (valid_)
        {
            const result status{error_code::already_exists};
            diagnostics(diagnostic_resource::thread).attempt(status);
            return status;
        }
        const result r         = cfg.entry == nullptr ? result{error_code::invalid_argument}
                                                      : osal_thread_create(&handle_, cfg.entry, cfg.arg, cfg.priority,
                                                                           cfg.affinity, cfg.stack, cfg.stack_bytes, cfg.name);
        initialization_status_ = r;
        valid_                 = r.ok();
        diagnostics(diagnostic_resource::thread).attempt(r);
        if (valid_)
        {
            diagnostics(diagnostic_resource::thread).acquire();
        }
        return r;
    }

    /// @brief Joins/releases the thread according to the backend's lifecycle policy.
    /// @details Native joins wait for completion. ThreadX terminates/deletes instead;
    ///          coordinate cooperative completion separately when required.
    /// @retval osal::ok()                   The thread was joined successfully.
    /// @retval error_code::not_supported    The active backend does not support thread joins.
    /// @retval error_code::not_initialized  This object does not own a thread.
    /// @return Otherwise the backend join error; ownership is retained on failure.
    /// @complexity O(1)
    /// @blocking   Backend-dependent; native joins can wait until the thread exits.
    result join() noexcept
    {
        if (!valid_)
        {
            return error_code::not_initialized;
        }
        const result r = osal_thread_join(&handle_, WAIT_FOREVER);
        diagnostics(diagnostic_resource::thread).attempt(r);
        if (r.ok())
        {
            valid_ = false;
            diagnostics(diagnostic_resource::thread).release();
        }
        return r;
    }

    /// @brief Joins with a timeout.
    /// @param[in] timeout  Maximum wait time.
    /// @retval osal::ok()                   The thread was joined successfully.
    /// @retval error_code::timeout          The join timeout expired.
    /// @retval error_code::not_supported    The active backend does not support timed joins.
    /// @retval error_code::not_initialized  This object does not own a thread.
    /// @return Otherwise the backend join error; ownership is retained on failure.
    /// @note Non-positive durations poll. Thread context only; native join policy still applies.
    /// @complexity O(1)
    /// @blocking   Up to timeout.
    result join_for(milliseconds timeout) noexcept
    {
        if (!valid_)
        {
            return error_code::not_initialized;
        }
        if constexpr (timed_join_backend<active_backend>)
        {
            const tick_t ticks = clock_utils::ms_to_ticks(timeout);
            const result r     = osal_thread_join(&handle_, ticks);
            diagnostics(diagnostic_resource::thread).attempt(r);
            if (r.ok())
            {
                valid_ = false;
                diagnostics(diagnostic_resource::thread).release();
            }
            return r;
        }
        else
        {
            return error_code::not_supported;
        }
    }

    /// @brief Joins until an absolute deadline.
    /// @param[in] deadline  Absolute monotonic time point.
    /// @retval osal::ok()                 The thread was joined before the deadline.
    /// @retval error_code::timeout        The deadline expired before the thread completed.
    /// @retval error_code::not_supported  The active backend does not support timed joins.
    /// @details Loops internally so that tick-count saturation (for unusually
    ///          large deadlines) does not cause premature timeout returns.
    /// @return Otherwise a backend join error or not_initialized; ownership is retained on failure.
    /// @note Already-expired deadlines return timeout without attempting a join.
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
    /// @retval osal::ok()                   The thread was detached successfully.
    /// @retval error_code::not_initialized  This object does not own a thread.
    /// @return Otherwise the backend detach/capability error.
    /// @note Entry context and caller-owned stack must still outlive the detached thread.
    /// @complexity O(1)
    result detach() noexcept
    {
        if (!valid_)
        {
            return error_code::not_initialized;
        }
        const result r = osal_thread_detach(&handle_);
        diagnostics(diagnostic_resource::thread).attempt(r);
        if (r.ok())
        {
            valid_ = false;
            diagnostics(diagnostic_resource::thread).release();
        }
        return r;
    }

    // ---- attributes --------------------------------------------------------

    /// @brief Changes the thread priority.
    /// @param[in] priority  New priority value.
    /// @retval osal::ok()                 The priority was updated successfully.
    /// @retval error_code::not_supported  The active backend has no dynamic-priority support.
    /// @return Otherwise the backend priority-change error.
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
    /// @param[in] affinity  Bitmask of allowed CPUs.
    /// @retval osal::ok()                 The affinity was updated successfully.
    /// @retval error_code::not_supported  The active backend has no affinity support.
    /// @return Otherwise the backend affinity-change error.
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
    /// @param[in] value  32-bit value; semantics are backend-defined (usually a bitmask or increment).
    /// @retval osal::ok()                 The notification was sent successfully.
    /// @retval error_code::not_supported  The active backend has no task-notification support.
    /// @return Otherwise the backend notification error.
    /// @note Thread context only.
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
    /// @param[in] value  32-bit value; semantics are backend-defined.
    /// @retval osal::ok()                 The notification was sent successfully.
    /// @retval error_code::not_supported  The active backend has no task-notification support.
    /// @return Otherwise the backend notification error.
    /// @pre The backend notification implementation supports the calling ISR context.
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
    /// @param[in]  timeout    Maximum wait; non-positive values poll.
    /// @param[out] value_out  Receives the notification value (may be nullptr).
    /// @retval osal::ok()                 A notification was received.
    /// @retval error_code::timeout        The wait expired.
    /// @retval error_code::not_supported  The active backend has no task-notification support.
    /// @return Otherwise the backend wait error.
    /// @note Thread context only. Clears all notification bits after receipt.
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
    /// @retval osal::ok()                 The thread was suspended successfully.
    /// @retval error_code::not_supported  The active backend does not support suspension.
    /// @return Otherwise the backend suspension error.
    [[nodiscard]] result suspend() noexcept { return osal_thread_suspend(&handle_); }

    /// @brief Resumes a previously suspended thread.
    /// @retval osal::ok()                 The thread was resumed successfully.
    /// @retval error_code::not_supported  The active backend does not support resumption.
    /// @return Otherwise the backend resumption error.
    [[nodiscard]] result resume() noexcept { return osal_thread_resume(&handle_); }

    // ---- query -------------------------------------------------------------

    /// @brief Returns an opaque identity token for this thread object.
    /// @return Backend identity, or INVALID_THREAD_ID if invalid, unsupported, or querying fails.
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
    /// @retval osal::ok()                 The priority was written to @p out.
    /// @retval error_code::not_supported  This query is unavailable in the draft API.
    /// @return Otherwise the backend query error; unsupported queries set @p out to PRIORITY_NORMAL.
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
    /// @retval osal::ok()                 The affinity mask was written to @p out.
    /// @retval error_code::not_supported  This query is unavailable in the draft API.
    /// @return Otherwise the backend query error; unsupported queries set @p out to AFFINITY_ANY.
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
    /// @retval osal::ok()                 The low-watermark value was written to @p out.
    /// @retval error_code::not_supported  This query is unavailable in the draft API.
    /// @return Otherwise the backend query error; unsupported queries set @p out to zero.
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
    /// @retval osal::ok()                 The execution time was written to @p out.
    /// @retval error_code::not_supported  This query is unavailable in the draft API.
    /// @return Otherwise the backend query error; unsupported queries set @p out to zero.
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
    /// @param[out] out  Set to zero; no CPU-load query is currently implemented.
    /// @retval error_code::not_supported  Always returned by this draft query.
    [[nodiscard]] result cpu_load_permille(load_permille_t& out) const noexcept
    {
        out = load_permille_t{0};
        return error_code::not_supported;
    }

    /// @brief Returns true if the thread was successfully created.
    /// @return Whether this object currently owns a thread not yet joined or detached.
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    /// @brief Last backend creation result; a rejected duplicate create preserves it.
    /// @return Most recent creation attempt's status, initially not_initialized.
    [[nodiscard]] result initialization_status() const noexcept { return initialization_status_; }

    // ---- static helpers (current thread) -----------------------------------

    /// @brief Returns the opaque identity token for the current thread.
    /// @return Backend identity, or INVALID_THREAD_ID when unsupported or querying fails.
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
    /// @param[out] out  Receives the priority on success; set to PRIORITY_NORMAL when unsupported.
    /// @return Backend query result, or not_supported when the query capability is absent.
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
    /// @param[out] out  Receives the CPU mask on success; set to AFFINITY_ANY when unsupported.
    /// @return Backend query result, or not_supported when the query capability is absent.
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
    /// @param[out] out  Receives the CPU index on success; set to zero when unsupported.
    /// @return Backend query result, or not_supported when current-CPU queries are unavailable.
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
    /// @param[out] out  Receives the minimum observed free bytes; set to zero when unsupported.
    /// @return Backend query result, or not_supported when stack-watermark queries are unavailable.
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
    /// @param[out] out  Receives the measured duration; set to zero when unsupported.
    /// @return Backend query result, or not_supported when execution-time queries are unavailable.
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
    /// @param[out] out  Set to zero; no CPU-load query is currently implemented.
    /// @retval error_code::not_supported  Always returned by this draft query.
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
    /// @param[in] duration  Sleep duration.
    /// @complexity O(1)
    /// @blocking   Blocks for the clamped positive duration, subject to backend tick/scheduler behavior.
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
    /// @param[in] tp  Absolute monotonic deadline.
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
    result                         initialization_status_{error_code::not_initialized};
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
/// @return Backend identity, or INVALID_THREAD_ID when unsupported or querying fails.
[[nodiscard]] inline thread_id_t get_id() noexcept
{
    return thread::current_id();
}

/// @brief Queries the priority of the current thread.
/// @param[out] out  Receives the priority on success; set to PRIORITY_NORMAL when unsupported.
/// @return Backend query result, or not_supported when priority queries are unavailable.
[[nodiscard]] inline result get_priority(priority_t& out) noexcept
{
    return thread::current_priority(out);
}

/// @brief Queries the affinity mask of the current thread.
/// @param[out] out  Receives the CPU mask on success; set to AFFINITY_ANY when unsupported.
/// @return Backend query result, or not_supported when affinity queries are unavailable.
[[nodiscard]] inline result get_affinity(affinity_t& out) noexcept
{
    return thread::current_affinity(out);
}

/// @brief Queries the current CPU/core index of the calling thread.
/// @param[out] out  Receives the CPU index on success; set to zero when unsupported.
/// @return Backend query result, or not_supported when current-CPU queries are unavailable.
[[nodiscard]] inline result get_cpu(std::uint32_t& out) noexcept
{
    return thread::current_cpu(out);
}

/// @brief Queries the stack low-watermark of the current thread.
/// @param[out] out  Receives the minimum free bytes; set to zero when unsupported.
/// @return Backend query result, or not_supported when stack-watermark queries are unavailable.
[[nodiscard]] inline result stack_low_watermark_bytes(std::size_t& out) noexcept
{
    return thread::current_stack_low_watermark_bytes(out);
}

/// @brief Queries the execution time of the current thread.
/// @param[out] out  Receives the measured duration; set to zero when unsupported.
/// @return Backend query result, or not_supported when execution-time queries are unavailable.
[[nodiscard]] inline result execution_time(microseconds& out) noexcept
{
    return thread::current_execution_time(out);
}

/// @brief Queries the CPU-load share of the current thread.
/// @param[out] out  Set to zero; no CPU-load query is currently implemented.
/// @retval error_code::not_supported  Always returned by this draft query.
[[nodiscard]] inline result cpu_load_permille(load_permille_t& out) noexcept
{
    return thread::current_cpu_load_permille(out);
}

/// @brief Yields the current thread's time-slice to the scheduler.
inline void yield() noexcept
{
    thread::yield();
}

/// @brief Sleeps the current thread using thread::sleep_for().
/// @param[in] d  Requested duration; non-positive values return immediately and large values saturate.
/// @note Thread context only; backend tick/scheduling granularity applies.
inline void sleep_for(milliseconds d) noexcept
{
    thread::sleep_for(d);
}

/// @brief Blocks the current thread until the absolute time point @p tp.
/// @param[in] tp  Monotonic deadline.  If in the past, returns immediately.
/// @note Thread context only.
inline void sleep_until(monotonic_clock::time_point tp) noexcept
{
    thread::sleep_until(tp);
}

}  // namespace this_thread

}  // namespace osal
