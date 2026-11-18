// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "condvar.hpp"
#include "detail/atomic_compat.hpp"
#include "error.hpp"
#include "thread.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#ifndef OSAL_STOP_SOURCE_MAX_CALLBACKS
/// @brief Maximum simultaneous callback registrations on one stop_source.
#define OSAL_STOP_SOURCE_MAX_CALLBACKS 16U
#endif

namespace osal
{

class stop_source;
class stop_callback;

/// @brief Borrowed cooperative cancellation state; the source must outlive it.
class stop_token
{
public:
    /// @brief Constructs an empty token that cannot request cancellation.
    constexpr stop_token() noexcept = default;
    /// @brief Reports whether this token refers to a stop source.
    /// @return true if a source was associated with this token.
    [[nodiscard]] bool stop_possible() const noexcept { return source_ != nullptr; }
    /// @brief Reports whether the associated source has requested stopping.
    /// @return true if stop was requested; false for an empty token.
    [[nodiscard]] bool stop_requested() const noexcept;
    /// @brief Reports the associated source's initialization or operational status.
    /// @return Source status, or success for an empty token.
    [[nodiscard]] result status() const noexcept;

private:
    friend class stop_source;
    friend class stop_callback;
    explicit stop_token(stop_source* source) noexcept : source_(source) {}
    stop_source* source_ = nullptr;
};

/// @brief Caller-owned, bounded, thread-context-only cancellation source.
/// @details Does not terminate threads. Must outlive tokens, callbacks and all
///          concurrent operations. Callbacks run synchronously outside the lock.
class stop_source
{
public:
    /// @brief Constructs a cancellation source and its synchronization state.
    /// @details Check initialization_status() or status() before use. Source
    ///          operations are thread-context-only.
    stop_source() noexcept
        : metadata_{mutex_config{mutex_type::normal, mutex::supports_protocol(mutex_protocol::priority_inheritance)
                                                         ? mutex_protocol::priority_inheritance
                                                         : mutex_protocol::native_default}}
    {
        initialization_status_ = metadata_.initialization_status();
        if (initialization_status_.ok())
        {
            initialization_status_ = osal_condvar_create(&completion_);
            completion_created_    = initialization_status_.ok();
        }
    }
    /// @brief Destroys the source's condition variable.
    /// @details The source must outlive every token, callback, and concurrent
    ///          operation referring to it.
    ~stop_source() noexcept
    {
        if (completion_created_)
        {
            (void)osal_condvar_destroy(&completion_);
        }
    }
    /// @brief Copy construction is disabled; source state has a unique owner.
    stop_source(const stop_source&) = delete;
    /// @brief Copy assignment is disabled; source state has a unique owner.
    stop_source& operator=(const stop_source&) = delete;
    /// @brief Move construction is disabled because tokens and callbacks borrow this address.
    stop_source(stop_source&&) = delete;
    /// @brief Move assignment is disabled because tokens and callbacks borrow this address.
    stop_source& operator=(stop_source&&) = delete;
    /// @brief Creates a borrowed token referring to this source.
    /// @return Token whose validity is bounded by the source lifetime.
    [[nodiscard]] stop_token get_token() noexcept { return stop_token{this}; }
    /// @brief Reports whether stop has been requested.
    /// @return Current atomic stop state.
    [[nodiscard]] bool stop_requested() const noexcept { return stopped_.load(std::memory_order_acquire); }
    /// @brief Returns the result of source initialization.
    /// @return Initialization result.
    [[nodiscard]] result initialization_status() const noexcept { return initialization_status_; }
    /// @brief Returns initialization status or the first recorded operational failure.
    /// @return Current source status.
    [[nodiscard]] result status() const noexcept
    {
        return initialization_status_.ok() ? result{failure_.load(std::memory_order_acquire)} : initialization_status_;
    }
    /// @brief Reports whether the source is ready and has no recorded failure.
    /// @return true when status() is successful.
    [[nodiscard]] bool valid() const noexcept { return status().ok(); }
    /// @brief Request cooperative stop and report whether this was the first request.
    /// @param[out] first_request Set true only when this call first requested stop.
    /// @return Success or a synchronization/initialization error.
    /// @details Callbacks execute synchronously outside the source lock, in the
    ///          requesting thread. Repeated requests succeed with
    ///          @p first_request false. Not ISR-safe.
    result request_stop(bool& first_request) noexcept;
    /// @brief Request stop, returning true only for the first successful request.
    /// @return true if this call first requested stop; false for repeated requests
    ///         or operational failure.
    /// @details Consult status() to distinguish a repeated request from an error.
    bool request_stop() noexcept
    {
        bool first_request = false;
        return request_stop(first_request).ok() && first_request;
    }

private:
    friend class stop_callback;
    result record(result status) noexcept
    {
        if (!status.ok())
        {
            auto expected = error_code::ok;
            (void)failure_.compare_exchange_strong(expected, status.code(), std::memory_order_acq_rel);
        }
        return status;
    }
    result lock(milliseconds timeout = milliseconds{-1}) noexcept
    {
        if (!initialization_status_.ok())
        {
            return initialization_status_;
        }
        // Cleanup must remain possible after a recorded fault.
        const result locked = metadata_.checked_lock(timeout);
        return timeout.count() >= 0 && (locked == error_code::timeout || locked == error_code::would_block ||
                                        locked == error_code::not_supported)
                   ? locked
                   : record(locked);
    }
    result                          unlock() noexcept { return record(metadata_.checked_unlock()); }
    mutex                           metadata_;
    active_traits::condvar_handle_t completion_{};
    result                          initialization_status_;
    bool                            completion_created_ = false;
    std::atomic<error_code>         failure_{error_code::ok};
    std::atomic<bool>               stopped_{false};
    stop_callback*                  head_  = nullptr;
    std::size_t                     count_ = 0U;
};

inline bool stop_token::stop_requested() const noexcept
{
    return source_ != nullptr && source_->stop_requested();
}

inline result stop_token::status() const noexcept
{
    return source_ != nullptr ? source_->status() : ok();
}

/// @brief Nonmovable caller-owned callback registration.
/// @details Destruction waits for an in-flight callback without holding the
///          source lock. Callbacks must be short, noexcept, and must not destroy
///          themselves or another in-flight callback, or wait on the requester.
///          Source operations, registration and destruction are not ISR-safe.
class stop_callback
{
public:
    /// @brief Callback function signature invoked on a stop request.
    using callback_type = void (*)(void*) noexcept;

    /// @brief Register a callback to run when its source requests stop.
    /// @param[in] token    Borrowed token identifying the source; may be empty.
    /// @param[in] callback Non-null callback to invoke.
    /// @param[in] argument Opaque value passed to @p callback.
    /// @details The source and argument must remain alive through callback
    ///          execution and registration close. If stopping was already
    ///          requested, the callback is invoked synchronously before
    ///          construction returns. Check status() for registration failure.
    stop_callback(stop_token token, callback_type callback, void* argument) noexcept
        : source_(token.source_), callback_(callback), argument_(argument)
    {
        if (callback_ == nullptr)
        {
            status_ = error_code::invalid_argument;
            source_ = nullptr;
            return;
        }
        if (source_ == nullptr)
        {
            return;
        }
        status_ = source_->status();
        if (!status_.ok())
        {
            source_ = nullptr;
            return;
        }
        status_ = source_->lock();
        if (!status_.ok())
        {
            source_ = nullptr;
            return;
        }
        if (source_->stop_requested())
        {
            status_ = source_->unlock();
            if (status_.ok())
            {
                source_ = nullptr;
                callback_(argument_);
            }
            return;
        }
        if (source_->count_ == OSAL_STOP_SOURCE_MAX_CALLBACKS)
        {
            status_               = error_code::overflow;
            const result unlocked = source_->unlock();
            if (!unlocked.ok())
            {
                status_ = unlocked;
            }
            source_ = nullptr;
            return;
        }
        next_          = source_->head_;
        source_->head_ = this;
        ++source_->count_;
        status_ = source_->unlock();
    }

    /// @brief Unregisters and waits for callback completion.
    /// @details Use close(timeout) and keep the registration, source and argument
    ///          alive on failure when recovery is required before destruction.
    ///          Aborts if the no-argument close operation fails.
    ~stop_callback() noexcept
    {
        if (!close().ok())
        {
            std::abort();
        }
    }

    /// @brief Unregister and wait for any active callback to finish.
    /// @param[in] timeout Maximum wait; negative waits indefinitely.
    /// @return Success, timeout, or a source/condition-variable error.
    /// @details On failure the registration and its source/argument must remain
    ///          alive; retry close() or keep them alive until destruction.
    [[nodiscard]] result close(milliseconds timeout = milliseconds{-1}) noexcept
    {
        if (source_ == nullptr)
        {
            return ok();
        }
        const auto   deadline = monotonic_deadline::after(timeout);
        const result locked   = source_->lock(timeout.count() < 0 ? milliseconds{-1} : deadline.remaining());
        if (!locked.ok())
        {
            return locked;
        }
        auto** link = &source_->head_;
        while (*link != nullptr && *link != this)
        {
            link = &(*link)->next_;
        }
        if (*link == this)
        {
            *link = next_;
            --source_->count_;
        }
        while (executing_.load(std::memory_order_acquire))
        {
            if (timeout.count() >= 0 && deadline.expired())
            {
                const result unlocked = source_->unlock();
                return unlocked.ok() ? result{error_code::timeout} : unlocked;
            }
            tick_t       ticks = timeout.count() < 0 ? WAIT_FOREVER : clock_utils::ms_to_ticks(deadline.remaining());
            const tick_t poll_ticks = clock_utils::ms_to_ticks(milliseconds{10});
            ticks                   = ticks == WAIT_FOREVER || ticks > poll_ticks ? poll_ticks : ticks;
            const result waited = osal_condvar_wait(&source_->completion_, source_->metadata_.native_handle(), ticks);
            if (!waited.ok() && waited != error_code::timeout && waited != error_code::would_block)
            {
                if (waited != error_code::not_supported || timeout.count() < 0)
                {
                    (void)source_->record(waited);
                }
                (void)source_->unlock();
                return waited;
            }
        }
        const result unlocked = source_->unlock();
        if (unlocked.ok())
        {
            source_ = nullptr;
        }
        return unlocked;
    }

    /// @brief Copy construction is disabled; callback registration is uniquely owned.
    stop_callback(const stop_callback&) = delete;
    /// @brief Copy assignment is disabled; callback registration is uniquely owned.
    stop_callback& operator=(const stop_callback&) = delete;
    /// @brief Move construction is disabled because the source links to this object's address.
    stop_callback(stop_callback&&) = delete;
    /// @brief Move assignment is disabled because the source links to this object's address.
    stop_callback& operator=(stop_callback&&) = delete;
    /// @brief Returns the status established during callback registration.
    /// @return Success or the registration/initialization error.
    [[nodiscard]] result status() const noexcept { return status_; }

private:
    friend class stop_source;
    stop_source*      source_;
    callback_type     callback_;
    void*             argument_;
    stop_callback*    next_ = nullptr;
    std::atomic<bool> executing_{false};
    result            status_;
};

inline result stop_source::request_stop(bool& first_request) noexcept
{
    first_request = false;
    if (!status().ok())
    {
        return status();
    }
    result locked = lock();
    if (!locked.ok())
    {
        return locked;
    }
    if (stopped_.exchange(true, std::memory_order_acq_rel))
    {
        return unlock();
    }
    first_request = true;
    while (head_ != nullptr)
    {
        auto* callback = head_;
        head_          = callback->next_;
        --count_;
        callback->executing_.store(true, std::memory_order_release);
        const result unlocked = unlock();
        if (!unlocked.ok())
        {
            return unlocked;
        }
        callback->callback_(callback->argument_);
        locked = lock();
        if (!locked.ok())
        {
            callback->executing_.store(false, std::memory_order_release);
            (void)record(osal_condvar_notify_all(&completion_));
            return locked;
        }
        callback->executing_.store(false, std::memory_order_release);
        // Wake while holding metadata; unregister re-acquires it before returning.
        const result notified = record(osal_condvar_notify_all(&completion_));
        if (!notified.ok())
        {
            (void)unlock();
            return notified;
        }
    }
    return unlock();
}

}  // namespace osal
