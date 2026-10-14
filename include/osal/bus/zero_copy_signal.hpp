// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <osal/bus/osal_bus.hpp>
#include <osal/diagnostics.hpp>
#include <osal/mutex.hpp>
#include <array>
#include <cassert>
#include <limits>
#include <mutex>
#include <type_traits>
#include <utility>

namespace osal
{
namespace detail
{
[[nodiscard]] inline constexpr std::uint32_t bus_diagnostic_gauge(std::size_t value) noexcept
{
    constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
    return value > limit ? limit : static_cast<std::uint32_t>(value);
}
}  // namespace detail

template<typename T, std::size_t MaxSubscribers, std::size_t PerSubCapacity, std::size_t PoolCapacity>
class zero_copy_signal;

/// Caller-owned payload storage. Must outlive every producer/read loan and topic.
template<typename T, std::size_t Capacity>
class message_pool
{
    static_assert(Capacity > 0U);
    static_assert(Capacity <= std::numeric_limits<std::uint32_t>::max());
    static_assert(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_destructible_v<T>);

public:
    ~message_pool() noexcept { assert(stats_.current == 0U); }

    class producer_loan
    {
    public:
        producer_loan() noexcept = default;
        ~producer_loan() noexcept { release(); }
        producer_loan(const producer_loan&)            = delete;
        producer_loan& operator=(const producer_loan&) = delete;
        producer_loan(producer_loan&& other) noexcept : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_)
        {
        }
        producer_loan& operator=(producer_loan&& other) noexcept
        {
            if (this != &other)
            {
                release();
                pool_  = std::exchange(other.pool_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }
        [[nodiscard]] T* get() noexcept { return pool_ ? &pool_->slots_[index_].value : nullptr; }
        T&               operator*() noexcept { return *get(); }
        T*               operator->() noexcept { return get(); }
        explicit         operator bool() const noexcept { return pool_ != nullptr; }
        void             release() noexcept
        {
            if (auto* pool = std::exchange(pool_, nullptr))
                pool->release_(index_);
        }

    private:
        friend class message_pool;
        template<typename, std::size_t, std::size_t, std::size_t>
        friend class zero_copy_signal;
        producer_loan(message_pool* pool, std::size_t index) noexcept : pool_(pool), index_(index) {}
        message_pool* pool_{};
        std::size_t   index_{};
    };

    class read_loan
    {
    public:
        read_loan() noexcept = default;
        ~read_loan() noexcept { release(); }
        read_loan(const read_loan& other) noexcept : pool_(other.pool_), index_(other.index_)
        {
            if (pool_)
                pool_->retain_(index_);
        }
        read_loan& operator=(const read_loan& other) noexcept
        {
            if (this != &other)
            {
                read_loan copy{other};
                *this = std::move(copy);
            }
            return *this;
        }
        read_loan(read_loan&& other) noexcept : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_) {}
        read_loan& operator=(read_loan&& other) noexcept
        {
            if (this != &other)
            {
                release();
                pool_  = std::exchange(other.pool_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }
        [[nodiscard]] const T* get() const noexcept { return pool_ ? &pool_->slots_[index_].value : nullptr; }
        const T&               operator*() const noexcept { return *get(); }
        const T*               operator->() const noexcept { return get(); }
        explicit               operator bool() const noexcept { return pool_ != nullptr; }
        void                   release() noexcept
        {
            if (auto* pool = std::exchange(pool_, nullptr))
                pool->release_(index_);
        }

    private:
        template<typename, std::size_t, std::size_t, std::size_t>
        friend class zero_copy_signal;
        read_loan(message_pool* pool, std::size_t index) noexcept : pool_(pool), index_(index) {}
        message_pool* pool_{};
        std::size_t   index_{};
    };

    struct statistics
    {
        std::size_t current{};
        std::size_t high_water{};
        std::size_t exhaustion{};
    };

    [[nodiscard]] bool          valid() const noexcept { return mutex_.valid(); }
    [[nodiscard]] producer_loan try_loan() noexcept
    {
        if (!valid())
        {
            operations_.attempt(error_code::not_initialized);
            return {};
        }
        std::lock_guard lock{mutex_};
        for (std::size_t i = 0U; i < Capacity; ++i)
        {
            if (slots_[i].references == 0U)
            {
                slots_[i].references = 1U;
                ++stats_.current;
                if (stats_.current > stats_.high_water)
                    stats_.high_water = stats_.current;
                operations_.attempt(error_code::ok);
                return producer_loan{this, i};
            }
        }
        ++stats_.exhaustion;
        operations_.attempt(error_code::out_of_resources);
        return {};
    }
    [[nodiscard]] statistics snapshot() const noexcept
    {
        if (!valid())
            return {};
        std::lock_guard lock{mutex_};
        return stats_;
    }
    /// Unified per-instance diagnostics; attempts count producer-loan acquisitions.
    [[nodiscard]] diagnostic_snapshot diagnostics() const noexcept
    {
        const auto gauges = snapshot();
        auto       result = operations_.snapshot();
        result.capacity   = static_cast<std::uint32_t>(Capacity);
        result.in_use     = detail::bus_diagnostic_gauge(gauges.current);
        result.high_water = detail::bus_diagnostic_gauge(gauges.high_water);
        return result;
    }

private:
    template<typename, std::size_t, std::size_t, std::size_t>
    friend class zero_copy_signal;
    void retain_(std::size_t index) noexcept
    {
        std::lock_guard lock{mutex_};
        assert(slots_[index].references != std::numeric_limits<std::size_t>::max());
        ++slots_[index].references;
    }
    void release_(std::size_t index) noexcept
    {
        std::lock_guard lock{mutex_};
        if (--slots_[index].references == 0U)
            --stats_.current;
    }
    struct slot
    {
        T           value{};
        std::size_t references{};
    };
    mutable mutex              mutex_;
    std::array<slot, Capacity> slots_{};
    statistics                 stats_{};
    diagnostic_counters        operations_;
};

/// Portable ownership transport: queues copy only pool indices, never T.
template<typename T, std::size_t MaxSubscribers, std::size_t PerSubCapacity, std::size_t PoolCapacity>
class zero_copy_signal
{
    static_assert(MaxSubscribers > 0U && PerSubCapacity > 0U);
    static_assert(MaxSubscribers <= std::numeric_limits<std::uint32_t>::max());
    static_assert(PerSubCapacity <= std::numeric_limits<std::uint32_t>::max() / MaxSubscribers);

public:
    using value_type                = T;
    using pool_type                 = message_pool<T, PoolCapacity>;
    using producer_loan             = typename pool_type::producer_loan;
    using read_loan                 = typename pool_type::read_loan;
    static constexpr bool zero_copy = true;
    enum class publish_status
    {
        delivered,
        partial,
        no_subscribers,
        full,
        invalid_loan
    };
    struct publish_result
    {
        std::size_t    accepted{};
        std::size_t    dropped{};
        publish_status status{publish_status::invalid_loan};
        explicit       operator bool() const noexcept { return accepted != 0U; }
    };
    struct statistics
    {
        std::size_t drops{};
        std::size_t backlog{};
        std::size_t high_water{};
        std::size_t receivers{};
    };

    explicit zero_copy_signal(pool_type& pool) noexcept : pool_(pool) {}
    ~zero_copy_signal() noexcept
    {
        for (auto& slot : slots_)
        {
            assert(slot.receivers == 0U);
            drain_(slot);
        }
    }
    [[nodiscard]] bool valid() const noexcept
    {
        if (!mutex_.valid() || !pool_.valid())
            return false;
        for (const auto& slot : slots_)
            if (!slot.queue.valid())
                return false;
        return true;
    }
    [[nodiscard]] producer_loan try_loan() noexcept { return valid() ? pool_.try_loan() : producer_loan{}; }
    [[nodiscard]] bool          subscribe(subscriber_id& out) noexcept
    {
        out = invalid_subscriber_id;
        if (!valid())
            return false;
        std::lock_guard lock{mutex_};
        for (auto& slot : slots_)
        {
            if (slot.id == invalid_subscriber_id && next_id_ != invalid_subscriber_id)
            {
                slot.id = next_id_++;
                out     = slot.id;
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool unsubscribe(subscriber_id id) noexcept
    {
        if (!valid() || id == invalid_subscriber_id)
            return false;
        std::lock_guard lock{mutex_};
        for (auto& slot : slots_)
        {
            if (slot.id == id)
            {
                if (slot.receivers != 0U)
                    return false;
                slot.id = invalid_subscriber_id;
                drain_(slot);
                return true;
            }
        }
        return false;
    }
    /// On zero acceptance the caller keeps its writable loan; otherwise it is consumed.
    [[nodiscard]] publish_result publish_zero_copy(producer_loan&& loan) noexcept
    {
        if (!valid() || loan.pool_ != &pool_)
        {
            operations_.attempt(error_code::invalid_argument);
            return {};
        }
        std::lock_guard lock{mutex_};
        publish_result  result{};
        for (auto& slot : slots_)
        {
            if (slot.id != invalid_subscriber_id)
            {
                pool_.retain_(loan.index_);
                if (slot.queue.try_send(loan.index_))
                {
                    ++result.accepted;
                    ++slot.backlog;
                }
                else
                {
                    pool_.release_(loan.index_);
                    ++result.dropped;
                    ++stats_.drops;
                    operations_.dropped();
                }
            }
        }
        update_high_water_();
        if (result.accepted != 0U)
        {
            result.status = result.dropped == 0U ? publish_status::delivered : publish_status::partial;
            loan.release();
        }
        else
        {
            result.status = result.dropped == 0U ? publish_status::no_subscribers : publish_status::full;
        }
        operations_.attempt(result.accepted != 0U ? error_code::ok : error_code::would_block);
        return result;
    }
    [[nodiscard]] bool receive_loan(subscriber_id id, read_loan& out, tick_t timeout = NO_WAIT) noexcept
    {
        if (!valid() || id == invalid_subscriber_id)
            return false;
        subscriber_slot* selected = nullptr;
        {
            std::lock_guard lock{mutex_};
            for (auto& slot : slots_)
                if (slot.id == id)
                {
                    ++slot.receivers;
                    selected = &slot;
                    break;
                }
        }
        if (selected == nullptr)
            return false;
        std::size_t index{};
        const bool  received = selected->queue.receive(index, timeout);
        {
            std::lock_guard lock{mutex_};
            --selected->receivers;
            if (received)
                --selected->backlog;
        }
        if (received)
            out = read_loan{&pool_, index};
        return received;
    }
    [[nodiscard]] statistics snapshot() const noexcept
    {
        if (!valid())
            return {};
        std::lock_guard lock{mutex_};
        auto            result = stats_;
        for (const auto& slot : slots_)
        {
            result.backlog += slot.backlog;
            result.receivers += slot.receivers;
        }
        return result;
    }
    /// Unified per-instance diagnostics; attempts count publications.
    [[nodiscard]] diagnostic_snapshot diagnostics() const noexcept
    {
        const auto gauges = snapshot();
        auto       result = operations_.snapshot();
        result.capacity   = static_cast<std::uint32_t>(MaxSubscribers * PerSubCapacity);
        result.in_use     = detail::bus_diagnostic_gauge(gauges.backlog);
        result.backlog    = result.in_use;
        result.high_water = detail::bus_diagnostic_gauge(gauges.high_water);
        return result;
    }

private:
    struct subscriber_slot
    {
        osal_bus<std::size_t, PerSubCapacity, bus_backend_generic> queue;
        subscriber_id                                              id{invalid_subscriber_id};
        std::size_t                                                receivers{};
        std::size_t                                                backlog{};
    };
    void drain_(subscriber_slot& slot) noexcept
    {
        if (!slot.queue.valid())
            return;
        std::size_t index{};
        while (slot.queue.try_receive(index))
            pool_.release_(index);
        slot.backlog = 0U;
    }
    void update_high_water_() noexcept
    {
        std::size_t backlog{};
        for (const auto& slot : slots_)
            backlog += slot.backlog;
        if (backlog > stats_.high_water)
            stats_.high_water = backlog;
    }
    pool_type&                                  pool_;
    mutable mutex                               mutex_;
    std::array<subscriber_slot, MaxSubscribers> slots_{};
    subscriber_id                               next_id_{0U};
    statistics                                  stats_{};
    diagnostic_counters                         operations_;
};
}  // namespace osal
