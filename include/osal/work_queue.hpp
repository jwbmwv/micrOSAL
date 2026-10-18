// SPDX-License-Identifier: Apache-2.0
/// @file work_queue.hpp
/// @brief OSAL work queue — deferred callback execution
/// @details Provides osal::work_queue for posting work items (function + arg)
///          to a dedicated worker thread.  On backends with native work queues
///          (Zephyr k_work_queue, NuttX work_queue(), VxWorks jobQueueLib) the
///          native primitive is used directly.  On all other backends, a
///          thread + ring-buffer emulation is provided automatically.
///
///          The class is non-copyable, non-movable, and stores no dynamically
///          allocated memory visible to the caller — the worker thread stack
///          is caller-supplied.
///
///          Usage:
///          @code
///          alignas(16) static uint8_t wq_stack[4096];
///          osal::work_queue wq{wq_stack, sizeof(wq_stack), 16, "my_wq"};
///          wq.submit([](void*) { do_something(); });
///          wq.flush();
///          @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_work_queue
#pragma once

#include "backends.hpp"
#include "clock.hpp"
#include "error.hpp"
#include "diagnostics.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>

#ifndef OSAL_WORK_QUEUE_MAX_DEPTH
/// @brief Maximum depth for the internal work item ring buffer (emulated mode).
#define OSAL_WORK_QUEUE_MAX_DEPTH 32U
#endif

/// @brief Signature for a work item callback.
using osal_work_func_t = void (*)(void* arg);

extern "C"
{
    osal::result osal_work_queue_create(osal::active_traits::work_queue_handle_t* handle, void* stack,
                                        std::size_t stack_bytes, std::size_t depth, const char* name) noexcept;

    osal::result osal_work_queue_destroy(osal::active_traits::work_queue_handle_t* handle) noexcept;

    osal::result osal_work_queue_submit(osal::active_traits::work_queue_handle_t* handle, osal_work_func_t func,
                                        void* arg) noexcept;

    osal::result osal_work_queue_submit_from_isr(osal::active_traits::work_queue_handle_t* handle,
                                                 osal_work_func_t func, void* arg) noexcept;

    osal::result osal_work_queue_flush(osal::active_traits::work_queue_handle_t* handle,
                                       osal::tick_t                              timeout_ticks) noexcept;

    osal::result osal_work_queue_cancel_all(osal::active_traits::work_queue_handle_t* handle) noexcept;

    std::size_t osal_work_queue_pending(const osal::active_traits::work_queue_handle_t* handle) noexcept;

}  // extern "C"

namespace osal
{

/// @defgroup osal_work_queue OSAL Work Queue
/// @brief Deferred work execution on a dedicated worker thread.
/// @{

/// @brief Signature for a work item callback.
using work_func_t = osal_work_func_t;

/// @brief A single deferrable unit of work.
struct work_item
{
    work_func_t func = nullptr;  ///< Function to execute.
    void*       arg  = nullptr;  ///< Argument passed to func.
};

// ---------------------------------------------------------------------------
// work_queue_config — constexpr-constructible, place in .rodata / FLASH
// ---------------------------------------------------------------------------

/// @brief Immutable configuration for work queue creation.
/// @details Declare as @c const to place in .rodata (FLASH).  The @c stack
///          pointer targets RAM, but the config descriptor itself lives in FLASH.
///
///          @code
///          alignas(16) static uint8_t stack[4096];
///          const osal::work_queue_config wq_cfg{stack, sizeof(stack), 16, "my_wq"};
///          osal::work_queue wq{wq_cfg};
///          @endcode
struct work_queue_config
{
    void*       stack       = nullptr;  ///< Caller-supplied stack for the worker thread.
    std::size_t stack_bytes = 0;        ///< Size of the stack in bytes.
    std::size_t depth       = 16;       ///< Maximum number of queued work items.
    const char* name        = "wq";     ///< Human-readable name (for debugging).
};

/// @brief OSAL work queue — deferred execution of callbacks on a dedicated thread.
///
/// @details On backends with native work queue support (Zephyr, NuttX, VxWorks),
///          the native primitive is used.  On all other backends, an emulated
///          implementation is provided using a thread + ring buffer + condvar.
///
///          The caller supplies the worker thread stack.  The internal ring
///          buffer is allocated by the backend (typically via `new` on
///          POSIX/Linux, or from a static pool on RTOS backends).
class work_queue
{
public:
    // ---- construction / destruction ----------------------------------------

    /// @brief Constructs and starts the work queue.
    /// @param[in] stack        Caller-supplied stack for the worker thread.
    /// @param[in] stack_bytes  Size of the stack in bytes.
    /// @param[in] depth        Maximum number of queued work items; must be > 0 and is capped at
    ///                     OSAL_WORK_QUEUE_MAX_DEPTH).
    /// @param[in] name  Human-readable name (for debugging).
    work_queue(void* stack, std::size_t stack_bytes, std::size_t depth = 16, const char* name = "wq") noexcept
        : valid_(false)
    {
        const std::size_t capped = (depth > OSAL_WORK_QUEUE_MAX_DEPTH) ? OSAL_WORK_QUEUE_MAX_DEPTH : depth;
        initialize(stack, stack_bytes, capped, name);
    }

    /// @brief Constructs from an immutable config (config may reside in FLASH).
    /// @param[in] cfg  Configuration — typically declared @c const.
    /// @complexity O(1)
    explicit work_queue(const work_queue_config& cfg) noexcept : valid_(false)
    {
        const std::size_t capped = (cfg.depth > OSAL_WORK_QUEUE_MAX_DEPTH) ? OSAL_WORK_QUEUE_MAX_DEPTH : cfg.depth;
        initialize(cfg.stack, cfg.stack_bytes, capped, cfg.name);
    }

    /// @brief Destroys the work queue after draining any already-queued items.
    /// @details The worker thread is signalled to stop once the queue becomes empty,
    ///          then joined before resources are released.
    ~work_queue() noexcept { (void)shutdown(); }

    work_queue(const work_queue&)            = delete;
    work_queue& operator=(const work_queue&) = delete;
    work_queue(work_queue&&)                 = delete;
    work_queue& operator=(work_queue&&)      = delete;

    // ---- submit ------------------------------------------------------------

    /// @brief Submits a work item for deferred execution.
    /// @param[in] func  Function to call on the worker thread.
    /// @param[in] arg   Argument passed to @p func.
    /// @retval osal::ok()           The work item was submitted.
    /// @retval error_code::overflow The work queue is full.
    [[nodiscard]] result submit(work_func_t func, void* arg = nullptr) noexcept
    {
        const result status =
            valid_ ? osal_work_queue_submit(&handle_, func, arg) : result{error_code::not_initialized};
        operations_.attempt(status);
        diagnostics(diagnostic_resource::work_queue).attempt(status);
        if (status == error_code::overflow || status == error_code::would_block)
        {
            operations_.dropped();
            operations_.contended();
            diagnostics(diagnostic_resource::work_queue).dropped();
            diagnostics(diagnostic_resource::work_queue).contended();
        }
        (void)pending();
        return status;
    }

    /// @brief Submits a work item from ISR context.
    /// @retval error_code::not_supported The active backend has no ISR-safe work queue.
    [[nodiscard]] result submit_from_isr(work_func_t func, void* arg = nullptr) noexcept
    {
        return osal_work_queue_submit_from_isr(&handle_, func, arg);
    }

    // ---- control -----------------------------------------------------------

    /// @brief Blocks until all currently-queued items have been executed.
    /// @param[in] timeout  Maximum time to wait.
    /// @retval osal::ok()           All queued work completed.
    /// @retval error_code::timeout  The wait expired.
    [[nodiscard]] result flush(milliseconds timeout = milliseconds{5000}) noexcept
    {
        const tick_t ticks  = (timeout.count() < 0) ? WAIT_FOREVER : clock_utils::ms_to_ticks(timeout);
        const result status = valid_ ? osal_work_queue_flush(&handle_, ticks) : result{error_code::not_initialized};
        operations_.attempt(status);
        diagnostics(diagnostic_resource::work_queue).attempt(status);
        if (status == error_code::timeout || status == error_code::overflow || status == error_code::would_block)
        {
            operations_.contended();
            diagnostics(diagnostic_resource::work_queue).contended();
        }
        (void)pending();
        return status;
    }

    /// @brief Cancels all pending (not yet started) items.
    [[nodiscard]] result cancel_all() noexcept
    {
        const result status = valid_ ? osal_work_queue_cancel_all(&handle_) : result{error_code::not_initialized};
        operations_.attempt(status);
        diagnostics(diagnostic_resource::work_queue).attempt(status);
        (void)pending();
        return status;
    }

    /// @brief Drains and destroys the worker; failure retains ownership for retry.
    /// @details Idempotent. Quiesce producers before calling; never call from ISR.
    ///          Calling from the worker must return the backend's error, not force
    ///          termination. The destructor performs best-effort shutdown.
    ///          On failure keep this object and all callback arguments alive.
    [[nodiscard]] result shutdown() noexcept
    {
        if (!valid_)
        {
            return ok();
        }
        const result status = osal_work_queue_destroy(&handle_);
        diagnostics(diagnostic_resource::work_queue).attempt(status);
        if (status.ok())
        {
            valid_ = false;
            operations_.set_backlog(0);
            diagnostics(diagnostic_resource::work_queue).release();
        }
        return status;
    }

    [[nodiscard]] result close() noexcept { return shutdown(); }

    // ---- query -------------------------------------------------------------

    /// @brief Returns the number of items currently queued (not yet executed).
    [[nodiscard]] std::size_t pending() const noexcept
    {
        const auto count = valid_ ? osal_work_queue_pending(&handle_) : 0U;
        operations_.set_backlog(static_cast<std::uint32_t>(count));
        return count;
    }

    /// @brief Returns true if the work queue was successfully initialised.
    [[nodiscard]] bool   valid() const noexcept { return valid_; }
    [[nodiscard]] result initialization_status() const noexcept { return initialization_status_; }

    /// @brief Approximate last-observed backlog and modulo-2^32 operation counters.
    /// @details ISR submissions are excluded. Backlog/high water here measure
    ///          work items; global in_use/high water measure owned queue handles.
    [[nodiscard]] diagnostic_snapshot statistics() const noexcept { return operations_.snapshot(); }

private:
    void initialize(void* stack, std::size_t stack_bytes, std::size_t depth, const char* name) noexcept
    {
        initialization_status_ = osal_work_queue_create(&handle_, stack, stack_bytes, depth, name);
        valid_                 = initialization_status_.ok();
        operations_.set_capacity(static_cast<std::uint32_t>(depth));
        auto& counters = diagnostics(diagnostic_resource::work_queue);
        counters.attempt(initialization_status_);
        if (valid_)
        {
            counters.acquire();
        }
    }

    result                             initialization_status_{error_code::not_initialized};
    mutable diagnostic_counters        operations_{};
    bool                               valid_;
    active_traits::work_queue_handle_t handle_{};
};

/// @} // osal_work_queue

}  // namespace osal
