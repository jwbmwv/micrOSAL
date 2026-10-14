// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "condvar.hpp"
#include "detail/atomic_compat.hpp"
#include "error.hpp"
#include "thread.hpp"
#include <cstddef>
#include <cstdint>

#ifndef OSAL_STOP_SOURCE_MAX_CALLBACKS
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
    constexpr stop_token() noexcept = default;
    [[nodiscard]] bool   stop_possible() const noexcept { return source_ != nullptr; }
    [[nodiscard]] bool   stop_requested() const noexcept;
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
    ~stop_source() noexcept
    {
        if (completion_created_)
        {
            (void)osal_condvar_destroy(&completion_);
        }
    }
    stop_source(const stop_source&)                   = delete;
    stop_source& operator=(const stop_source&)        = delete;
    stop_source(stop_source&&)                        = delete;
    stop_source&             operator=(stop_source&&) = delete;
    [[nodiscard]] stop_token get_token() noexcept { return stop_token{this}; }
    [[nodiscard]] bool       stop_requested() const noexcept { return stopped_.load(std::memory_order_acquire); }
    [[nodiscard]] result     initialization_status() const noexcept { return initialization_status_; }
    [[nodiscard]] result     status() const noexcept
    {
        return initialization_status_.ok() ? result{failure_.load(std::memory_order_acquire)} : initialization_status_;
    }
    [[nodiscard]] bool valid() const noexcept { return status().ok(); }
    /// @brief Checked request; reports primitive errors separately from a repeated request.
    result request_stop(bool& first_request) noexcept;
    /// @brief Compatibility request; consult status() when it returns false.
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
    using callback_type = void (*)(void*) noexcept;

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

    /// @brief Destruction waits without holding the source mutex.
    /// @details A permanent backend fault may prevent safe unregistering. Use
    ///          close(timeout) and keep the registration alive on failure.
    ~stop_callback() noexcept
    {
        while (!close().ok())
        {
            // Fault-only retry: never abandon a node still referenced by the source.
            // Completion and metadata contention use blocking primitives, not this path.
            thread::sleep_for(milliseconds{1000});
        }
    }

    /// @brief Checked unregister; failure retains the registration for retry.
    result close(milliseconds timeout = milliseconds{-1}) noexcept
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

    stop_callback(const stop_callback&)             = delete;
    stop_callback& operator=(const stop_callback&)  = delete;
    stop_callback(stop_callback&&)                  = delete;
    stop_callback&       operator=(stop_callback&&) = delete;
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
