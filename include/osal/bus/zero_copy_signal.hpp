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
/// @brief Converts a size gauge to uint32_t without wrapping on large values.
/// @param[in] value  Gauge value to convert.
/// @return @p value clamped to the largest representable @c uint32_t value.
[[nodiscard]] inline constexpr std::uint32_t bus_diagnostic_gauge(std::size_t value) noexcept
{
    constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
    return value > limit ? limit : static_cast<std::uint32_t>(value);
}
}  // namespace detail

template<typename T, std::size_t MaxSubscribers, std::size_t PerSubCapacity, std::size_t PoolCapacity>
class zero_copy_signal;

/// @brief Fixed-capacity caller-owned storage for shared signal payloads.
/// @details The pool does not allocate dynamically. Its lifetime must extend
///          past every signal using it and every outstanding producer or read
///          loan; destruction asserts that all pool references have been released.
/// @note Pool operations use an OSAL mutex and are not ISR-safe.
/// @tparam T        Stored payload type; must be nothrow default-constructible and destructible.
/// @tparam Capacity Number of payload slots; must be positive and fit in @c uint32_t.
template<typename T, std::size_t Capacity>
class message_pool
{
    static_assert(Capacity > 0U);
    static_assert(Capacity <= std::numeric_limits<std::uint32_t>::max());
    static_assert(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_destructible_v<T>);

public:
    /// @brief Destroys the pool after all producer and read loans are released.
    /// @pre No pool references remain outstanding.
    ~message_pool() noexcept { assert(stats_.current == 0U); }

    /// @brief Unique writable reference to one payload slot.
    /// @details The pool must outlive the loan. The loan is move-only and releases
    ///          its reference automatically on destruction.
    class producer_loan
    {
    public:
        /// @brief Creates an empty producer loan.
        producer_loan() noexcept = default;
        /// @brief Releases the owned pool reference, if any.
        ~producer_loan() noexcept { release(); }
        /// @brief Copying a producer loan is disallowed to preserve unique write ownership.
        producer_loan(const producer_loan&) = delete;
        /// @brief Copy assignment is disallowed to preserve unique write ownership.
        producer_loan& operator=(const producer_loan&) = delete;
        /// @brief Transfers producer-loan ownership from @p other.
        /// @param[in,out] other  Loan emptied by the transfer.
        producer_loan(producer_loan&& other) noexcept : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_)
        {
        }
        /// @brief Releases this loan and takes ownership from @p other.
        /// @param[in,out] other  Loan emptied by the transfer.
        /// @return Reference to this loan.
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
        /// @brief Returns the writable payload pointer.
        /// @return Pointer to the payload, or null when the loan is empty.
        [[nodiscard]] T* get() noexcept { return pool_ ? &pool_->slots_[index_].value : nullptr; }
        /// @brief Accesses the writable payload.
        /// @pre The loan is nonempty; test its boolean conversion before dereferencing.
        /// @return Reference to the writable payload, valid while the loan remains nonempty
        ///         and its pool remains alive.
        T& operator*() noexcept { return *get(); }
        /// @brief Returns the writable payload pointer.
        /// @pre The loan is nonempty; test its boolean conversion before dereferencing.
        /// @return Pointer to the writable payload, valid while the loan remains nonempty
        ///         and its pool remains alive.
        T* operator->() noexcept { return get(); }
        /// @brief Tests whether the loan owns a payload slot.
        /// @return @c true when the loan is nonempty.
        explicit operator bool() const noexcept { return pool_ != nullptr; }
        /// @brief Releases this producer reference and makes its slot reusable when unreferenced.
        /// @post The loan is empty; repeated calls have no effect.
        void release() noexcept
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

    /// @brief Shared read-only reference to one payload slot.
    /// @details The pool must outlive the loan. Copies retain an additional pool
    ///          reference; each loan releases its reference on destruction.
    class read_loan
    {
    public:
        /// @brief Creates an empty read loan.
        read_loan() noexcept = default;
        /// @brief Releases this read reference.
        ~read_loan() noexcept { release(); }
        /// @brief Copies a read loan and retains its shared payload reference.
        /// @param[in] other  Loan whose payload reference is shared.
        read_loan(const read_loan& other) noexcept : pool_(other.pool_), index_(other.index_)
        {
            if (pool_)
                pool_->retain_(index_);
        }
        /// @brief Releases this reference and shares the payload held by @p other.
        /// @param[in] other  Loan whose payload reference is shared.
        /// @return Reference to this loan.
        read_loan& operator=(const read_loan& other) noexcept
        {
            if (this != &other)
            {
                read_loan copy{other};
                *this = std::move(copy);
            }
            return *this;
        }
        /// @brief Transfers read-loan ownership from @p other.
        /// @param[in,out] other  Loan emptied by the transfer.
        read_loan(read_loan&& other) noexcept : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_) {}
        /// @brief Releases this loan and takes ownership from @p other.
        /// @param[in,out] other  Loan emptied by the transfer.
        /// @return Reference to this loan.
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
        /// @brief Returns the read-only payload pointer.
        /// @return Pointer to the payload, or null when the loan is empty.
        [[nodiscard]] const T* get() const noexcept { return pool_ ? &pool_->slots_[index_].value : nullptr; }
        /// @brief Accesses the read-only payload.
        /// @pre The loan is nonempty; test its boolean conversion before dereferencing.
        /// @return Reference to the read-only payload, valid while the loan remains nonempty
        ///         and its pool remains alive.
        const T& operator*() const noexcept { return *get(); }
        /// @brief Returns the read-only payload pointer.
        /// @pre The loan is nonempty; test its boolean conversion before dereferencing.
        /// @return Pointer to the read-only payload, valid while the loan remains nonempty
        ///         and its pool remains alive.
        const T* operator->() const noexcept { return get(); }
        /// @brief Tests whether the loan owns a payload reference.
        /// @return @c true when the loan is nonempty.
        explicit operator bool() const noexcept { return pool_ != nullptr; }
        /// @brief Releases this read reference.
        /// @post The loan is empty; repeated calls have no effect.
        void release() noexcept
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

    /// @brief Current and high-water payload-pool usage statistics.
    struct statistics
    {
        std::size_t current{};     ///< Number of slots with one or more outstanding references.
        std::size_t high_water{};  ///< Maximum observed @c current value.
        std::size_t exhaustion{};  ///< Failed loan attempts because every slot was referenced.
    };

    /// @brief Reports whether the pool mutex initialized.
    /// @return @c true when pool operations can be used.
    [[nodiscard]] bool valid() const noexcept { return mutex_.valid(); }

    /// @brief Acquires a writable reference to an unused payload slot.
    /// @return A producer loan on success, or an empty loan if invalid or exhausted.
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
    /// @brief Returns pool occupancy and exhaustion statistics.
    /// @return A mutex-protected snapshot, or zeroed statistics when the pool is invalid.
    [[nodiscard]] statistics snapshot() const noexcept
    {
        if (!valid())
            return {};
        std::lock_guard lock{mutex_};
        return stats_;
    }
    /// @brief Returns unified per-instance diagnostics.
    /// @details Attempt counters count producer-loan acquisition attempts.
    /// @return A diagnostic snapshot; occupancy gauges reflect the pool statistics.
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

/// @brief Fixed-capacity pub/sub transport that queues pool indices instead of copying payloads.
/// @details Each subscriber has a bounded index queue. A successful publication
///          creates one shared pool reference per accepting subscriber and consumes
///          the producer loan; read loans keep the payload alive after dequeue.
///          Operations use OSAL mutexes and are not ISR-safe.
/// @tparam T               Message type stored in the external pool.
/// @tparam MaxSubscribers  Maximum concurrent subscribers; must be positive.
/// @tparam PerSubCapacity  Queue depth per subscriber; must be positive.
/// @tparam PoolCapacity    Number of payload slots in the referenced pool.
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
    /// @brief Outcome category for a zero-copy publication.
    enum class publish_status
    {
        delivered,       ///< All active subscriber queues accepted the payload.
        partial,         ///< At least one queue accepted it and at least one dropped it.
        no_subscribers,  ///< No active subscriber was registered.
        full,            ///< Subscribers existed but none accepted the payload.
        invalid_loan     ///< The supplied loan was empty, foreign, or the signal was invalid.
    };
    /// @brief Counts accepted and dropped subscriber deliveries for one publish.
    struct publish_result
    {
        std::size_t    accepted{};  ///< Number of subscriber queues that accepted the payload.
        std::size_t    dropped{};   ///< Number of subscriber queues that rejected the payload.
        publish_status status{publish_status::invalid_loan};  ///< Detailed publication outcome.

        /// @brief Tests whether any subscriber accepted the payload.
        /// @return @c true when @c accepted is nonzero.
        explicit operator bool() const noexcept { return accepted != 0U; }
    };
    /// @brief Aggregate subscriber-queue statistics.
    /// @brief Aggregate subscriber backlog and receiver statistics.
    struct statistics
    {
        std::size_t drops{};       ///< Total messages dropped because subscriber queues were full.
        std::size_t backlog{};     ///< Total queued deliveries across subscribers.
        std::size_t high_water{};  ///< Maximum observed total backlog.
        std::size_t receivers{};   ///< Number of receive calls currently in progress.
    };

    /// @brief Constructs a signal that borrows a payload pool.
    /// @param[in] pool  Pool used for payload storage; it must outlive this signal and its loans.
    explicit zero_copy_signal(pool_type& pool) noexcept : pool_(pool) {}
    /// @brief Drains queued payload references and checks that no receiver is active.
    ~zero_copy_signal() noexcept
    {
        for (auto& slot : slots_)
        {
            assert(slot.receivers == 0U);
            drain_(slot);
        }
    }
    /// @brief Reports whether the signal, pool, and all subscriber queues are initialized.
    /// @return @c true when signal operations can be used.
    [[nodiscard]] bool valid() const noexcept
    {
        if (!mutex_.valid() || !pool_.valid())
            return false;
        for (const auto& slot : slots_)
            if (!slot.queue.valid())
                return false;
        return true;
    }
    /// @brief Acquires a writable payload slot from the associated pool.
    /// @return A producer loan, or an empty loan if the signal/pool is invalid or exhausted.
    [[nodiscard]] producer_loan try_loan() noexcept { return valid() ? pool_.try_loan() : producer_loan{}; }

    /// @brief Registers a subscriber with an empty delivery queue.
    /// @param[out] out  Receives the subscriber ID, or the invalid sentinel on failure.
    /// @return @c true if registered; @c false if invalid, full, or identifier space is exhausted.
    [[nodiscard]] bool subscribe(subscriber_id& out) noexcept
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
    /// @brief Removes a subscriber when no receive call for it is active.
    /// @param[in] id  Subscriber ID returned by subscribe() on this signal.
    /// @return @c true if removed; @c false if invalid, unknown, or currently receiving.
    /// @note Queued payload references for this subscriber are released on removal.
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
    /// @brief Publishes the payload reference to all active subscriber queues.
    /// @param[in,out] loan  Writable loan from this signal's pool. Consumed only when
    ///                      at least one queue accepts the message.
    /// @return Accepted/dropped counts and a status describing delivery.
    /// @note When no queue accepts the payload, ownership remains with @p loan.
    ///       When one or more queues accept it, @p loan is released after creating
    ///       subscriber references; the payload remains alive until all read references end.
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
    /// @brief Dequeues a payload reference for a subscriber.
    /// @param[in]  id       Subscriber ID returned by subscribe() on this signal.
    /// @param[out] out      Receives a read loan on success; unchanged on failure.
    /// @param[in]  timeout  Maximum wait in ticks; NO_WAIT polls and WAIT_FOREVER waits indefinitely.
    /// @return @c true if a payload was received; @c false for an invalid/unknown subscriber,
    ///         timeout, or empty queue.
    /// @pre The subscriber remains registered for the duration of this call.
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
    /// @brief Returns aggregate queue and receiver statistics.
    /// @return A mutex-protected snapshot, or zeroed statistics when invalid.
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
    /// @brief Returns unified per-instance diagnostics.
    /// @details Attempts count publication calls; backlog gauges aggregate subscriber queues.
    /// @return Diagnostic counters and current capacity/backlog gauges.
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
