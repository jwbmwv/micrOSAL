// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_backend_zephyr.hpp
/// @brief Zephyr topic backend tag
/// @details On real Zephyr builds this header provides the dedicated native
///          osal::osal_signal implementation for @c bus_backend_zephyr. On
///          hosted builds that instantiate the tag only for compile-time
///          coverage, the topic still delegates to the generic runtime so the
///          cross-backend doctest matrix keeps compiling.
///
///          Premium capability traits remain limited to the functionality that
///          is actually implemented today.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <osal/bus/detail/osal_bus_backend_zephyr.hpp>
#include <osal/bus/detail/osal_signal_backend_generic.hpp>

#if defined(OSAL_BACKEND_ZEPHYR)
#include <osal/detail/cpp_compat.hpp>
#include <osal/mutex.hpp>
#include <osal/types.hpp>

#include <zephyr/kernel.h>

#include <array>
#include <cstdint>
#endif

#include <cstddef>

namespace osal
{

#if defined(OSAL_BACKEND_ZEPHYR)

// ===========================================================================
// osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_zephyr>
// ===========================================================================

/// @brief Zephyr-native LCD pub/sub topic.
/// @details Uses a direct Zephyr-backed queue per subscriber slot so the
///          public MicrOSAL publish / receive contract matches the generic
///          implementation while avoiding the generic C queue ABI layer.
/// @note Operations use a mutex and are not ISR-safe.
template<queue_element T, std::size_t MaxSubscribers, std::size_t PerSubCapacity>
    requires(MaxSubscribers > 0U && PerSubCapacity > 0U)
class osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_zephyr>
{
public:
    using value_type  = T;
    using observer_fn = void (*)(const T&) noexcept;

    /// @brief Reports whether the topic mutex initialized.
    /// @return @c true when the topic is ready for operations.
    [[nodiscard]] bool valid() const noexcept { return mutex_.valid(); }

    /// @brief Registers a new subscriber and returns its ID.
    /// @param[out] out_id  Receives the allocated subscriber ID, or the invalid sentinel on failure.
    /// @return @c true if a free slot was available; @c false when invalid or at capacity.
    /// @blocking Never (mutex-locked, O(MaxSubscribers) scan).
    [[nodiscard]] bool subscribe(subscriber_id& out_id) noexcept
    {
        if (!valid())
        {
            out_id = invalid_subscriber_id;
            return false;
        }
        scoped_lock lk{mutex_};
        for (std::size_t i = 0U; i < MaxSubscribers; ++i)
        {
            if (!slots_[i].in_use_)
            {
                slots_[i].queue_.purge();
                const subscriber_id new_id = ensure_subscriber_id_(i);
                if (new_id == invalid_subscriber_id)
                {
                    out_id = invalid_subscriber_id;
                    return false;
                }
                slots_[i].in_use_ = true;
                ++subscriber_count_;
                out_id = new_id;
                return true;
            }
        }
        out_id = invalid_subscriber_id;
        return false;
    }

    /// @brief Removes a subscriber by ID.
    /// @param[in] id  Subscriber ID obtained from subscribe() on this topic.
    /// @return @c true if removed; @c false if invalid or a receive is still in progress.
    [[nodiscard]] bool unsubscribe(subscriber_id id) noexcept
    {
        if (!valid())
        {
            return false;
        }
        scoped_lock       lk{mutex_};
        const std::size_t slot_index = slot_index_from_id_(id);
        if (slot_index >= MaxSubscribers || !subscriber_id_matches_slot_(id, slot_index))
        {
            return false;
        }

        if (slots_[slot_index].receivers_ != 0U)
        {
            return false;
        }
        slots_[slot_index].in_use_ = false;
        advance_subscriber_generation_(slot_index);
        --subscriber_count_;
        return true;
    }

    /// @brief Fans the message out to all active subscriber queues.
    /// @param[in] msg  Message to broadcast; copied into each accepting subscriber queue.
    /// @return @c true if at least one subscriber received the message.
    /// @blocking Never (uses non-blocking queue sends; full queues drop this message).
    /// @complexity O(MaxSubscribers).
    [[nodiscard]] bool publish(const T& msg) noexcept
    {
        if (!valid())
        {
            return false;
        }
        scoped_lock lk{mutex_};
        bool        any = false;
        for (auto& slot : slots_)
        {
            if (slot.in_use_)
            {
                any |= slot.queue_.try_send(msg);
            }
        }
        return any;
    }

    /// @brief Attempts a non-blocking receive for one subscriber.
    /// @param[in]  id   Subscriber ID obtained from subscribe() on this topic.
    /// @param[out] out  Receives the message when one is available.
    /// @return @c true if a message was received.
    [[nodiscard]] bool try_receive(subscriber_id id, T& out) noexcept
    {
        if (!valid())
        {
            return false;
        }
        scoped_lock       lk{mutex_};
        const std::size_t slot_index = slot_index_from_id_(id);
        if (slot_index >= MaxSubscribers || !subscriber_id_matches_slot_(id, slot_index))
        {
            return false;
        }

        return slots_[slot_index].queue_.try_receive(out);
    }

    /// @brief Receives a message for one subscriber, waiting up to the timeout.
    /// @param[in]  id       Subscriber ID obtained from subscribe() on this topic.
    /// @param[out] out      Receives the message when successful.
    /// @param[in]  timeout  Maximum wait in ticks; WAIT_FOREVER waits indefinitely.
    /// @return @c true if a message was received within the timeout.
    /// @pre The subscriber must remain registered for the duration of this call.
    [[nodiscard]] bool receive(subscriber_id id, T& out, tick_t timeout = WAIT_FOREVER) noexcept
    {
        if (!valid())
        {
            return false;
        }
        const std::size_t slot_index = slot_index_from_id_(id);
        {
            scoped_lock lk{mutex_};
            if (slot_index >= MaxSubscribers || !subscriber_id_matches_slot_(id, slot_index))
            {
                return false;
            }
            ++slots_[slot_index].receivers_;
        }
        const bool received = slots_[slot_index].queue_.receive(out, timeout);
        {
            scoped_lock lk{mutex_};
            --slots_[slot_index].receivers_;
        }
        return received;
    }

    /// @brief Registers a native observer callback.
    /// @param[in] fn  Non-null callback invoked synchronously during observer publication.
    /// @return @c true if registered; @c false for a null callback, invalid topic, or full observer table.
    /// @note The callback must not block or throw.
    [[nodiscard]] bool native_subscribe_observer(observer_fn fn) noexcept
    {
        if ((fn == nullptr) || !valid())
        {
            return false;
        }

        scoped_lock lk{mutex_};
        if (observer_count_ >= MaxSubscribers)
        {
            return false;
        }

        observers_[observer_count_++] = fn;
        return true;
    }

    /// @brief Removes a previously registered native observer callback.
    /// @param[in] fn  Callback pointer to remove; must be non-null.
    /// @return @c true if found and removed; @c false otherwise.
    [[nodiscard]] bool native_unsubscribe_observer(observer_fn fn) noexcept
    {
        if ((fn == nullptr) || !valid())
        {
            return false;
        }

        scoped_lock lk{mutex_};
        for (std::size_t i = 0U; i < observer_count_; ++i)
        {
            if (observers_[i] == fn)
            {
                observers_[i]               = observers_[--observer_count_];
                observers_[observer_count_] = nullptr;
                return true;
            }
        }

        return false;
    }

    /// @brief Returns the number of registered native observer callbacks.
    /// @return Observer count, or zero when the topic is invalid.
    [[nodiscard]] std::size_t native_observer_count() const noexcept
    {
        if (!valid())
        {
            return 0U;
        }
        scoped_lock lk{mutex_};
        return observer_count_;
    }

    /// @brief Invokes a snapshot of all registered native observer callbacks.
    /// @param[in] msg  Message passed by reference to each callback.
    /// @return @c true if at least one observer was registered when the snapshot was taken.
    /// @note Callbacks run synchronously in the caller's context and must not block or throw.
    [[nodiscard]] bool native_publish_observers(const T& msg) noexcept
    {
        if (!valid())
        {
            return false;
        }
        detail::bounded_vector<observer_fn, MaxSubscribers> snapshot;

        {
            scoped_lock       lk{mutex_};
            const std::size_t count = observer_count_;
            for (std::size_t i = 0U; i < count; ++i)
            {
                (void)snapshot.push_back(observers_[i]);
            }
        }

        const bool any = !snapshot.empty();
        for (const auto& fn : snapshot)
        {
            if (fn != nullptr)
            {
                fn(msg);
            }
        }
        return any;
    }

    /// @brief Returns the current number of active subscribers.
    /// @return Active subscriber count, or zero when the topic is invalid.
    [[nodiscard]] std::size_t subscriber_count() const noexcept
    {
        if (!valid())
        {
            return 0U;
        }
        scoped_lock lk{mutex_};
        return subscriber_count_;
    }

    /// @brief Returns the compile-time maximum subscriber count.
    /// @return The value of @c MaxSubscribers.
    static constexpr std::size_t max_subscribers() noexcept { return MaxSubscribers; }

    /// @brief Returns the compile-time capacity of each subscriber queue.
    /// @return The value of @c PerSubCapacity.
    static constexpr std::size_t per_subscriber_capacity() noexcept { return PerSubCapacity; }

private:
    using subscriber_generation_t = subscriber_id;

    static constexpr subscriber_id           subscriber_id_span_ = static_cast<subscriber_id>(MaxSubscribers);
    static constexpr subscriber_generation_t subscriber_generation_modulus_ =
        invalid_subscriber_id / subscriber_id_span_;

    static_assert(subscriber_generation_modulus_ > 0U,
                  "subscriber_id does not provide enough distinct values for this MaxSubscribers");

    struct subscriber_slot
    {
        subscriber_generation_t                         generation_{0U};
        bool                                            in_use_{false};
        std::size_t                                     receivers_{0U};
        osal_bus<T, PerSubCapacity, bus_backend_zephyr> queue_{};
    };

    /// @brief Derives this instance's identifier salt so IDs are scoped to a topic object.
    [[nodiscard]] subscriber_id subscriber_cookie_() const noexcept
    {
        constexpr auto mix = static_cast<subscriber_id>(0x9E3779B97F4A7C15ULL);
        return static_cast<subscriber_id>(reinterpret_cast<std::uintptr_t>(this)) ^ mix;
    }

    [[nodiscard]] static constexpr subscriber_id pack_subscriber_id_(std::size_t             slot_index,
                                                                     subscriber_generation_t generation) noexcept
    {
        return generation * subscriber_id_span_ + static_cast<subscriber_id>(slot_index) + 1U;
    }

    /// @brief Combines the per-instance salt, slot index, and generation into an opaque ID.
    [[nodiscard]] subscriber_id encode_subscriber_id_(std::size_t             slot_index,
                                                      subscriber_generation_t generation) const noexcept
    {
        return subscriber_cookie_() ^ pack_subscriber_id_(slot_index, generation);
    }

    [[nodiscard]] static constexpr subscriber_id unpack_subscriber_counter_(subscriber_id packed_id) noexcept
    {
        return packed_id - 1U;
    }

    [[nodiscard]] static constexpr std::size_t slot_index_from_packed_subscriber_id_(subscriber_id packed_id) noexcept
    {
        return static_cast<std::size_t>(unpack_subscriber_counter_(packed_id) % subscriber_id_span_);
    }

    [[nodiscard]] static constexpr subscriber_generation_t
    generation_from_packed_subscriber_id_(subscriber_id packed_id) noexcept
    {
        return unpack_subscriber_counter_(packed_id) / subscriber_id_span_;
    }

    /// @brief Validates the ID encoding and extracts a candidate slot index.
    /// @return @c MaxSubscribers when @p id cannot encode a valid slot.
    [[nodiscard]] std::size_t slot_index_from_id_(subscriber_id id) const noexcept
    {
        if (id == invalid_subscriber_id)
        {
            return MaxSubscribers;
        }

        const subscriber_id packed_id = id ^ subscriber_cookie_();
        if (packed_id == 0U || packed_id > (subscriber_generation_modulus_ * subscriber_id_span_))
        {
            return MaxSubscribers;
        }

        return slot_index_from_packed_subscriber_id_(packed_id);
    }

    /// @brief Checks whether an ID identifies the active generation in a slot.
    [[nodiscard]] bool subscriber_id_matches_slot_(subscriber_id id, std::size_t slot_index) const noexcept
    {
        if (slot_index >= MaxSubscribers || !slots_[slot_index].in_use_ || id == invalid_subscriber_id)
        {
            return false;
        }

        const subscriber_id packed_id = id ^ subscriber_cookie_();
        if (packed_id == 0U || packed_id > (subscriber_generation_modulus_ * subscriber_id_span_))
        {
            return false;
        }

        return slot_index_from_packed_subscriber_id_(packed_id) == slot_index &&
               generation_from_packed_subscriber_id_(packed_id) == slots_[slot_index].generation_;
    }

    /// @brief Advances a slot generation so IDs from an earlier registration are rejected.
    void advance_subscriber_generation_(std::size_t slot_index) noexcept
    {
        auto& generation = slots_[slot_index].generation_;
        generation       = (generation + 1U) % subscriber_generation_modulus_;
        if (encode_subscriber_id_(slot_index, generation) == invalid_subscriber_id)
        {
            generation = (generation + 1U) % subscriber_generation_modulus_;
        }
    }

    /// @brief Produces a non-sentinel identifier for a slot's current generation.
    [[nodiscard]] subscriber_id ensure_subscriber_id_(std::size_t slot_index) noexcept
    {
        subscriber_id id = encode_subscriber_id_(slot_index, slots_[slot_index].generation_);
        if (id == invalid_subscriber_id)
        {
            advance_subscriber_generation_(slot_index);
            id = encode_subscriber_id_(slot_index, slots_[slot_index].generation_);
        }
        return id;
    }

    struct [[nodiscard]] scoped_lock
    {
        mutex& mtx_;

        explicit scoped_lock(mutex& mtx) noexcept : mtx_{mtx} { mtx_.lock(); }
        ~scoped_lock() noexcept { mtx_.unlock(); }

        scoped_lock(const scoped_lock&)            = delete;
        scoped_lock& operator=(const scoped_lock&) = delete;
        scoped_lock(scoped_lock&&)                 = delete;
        scoped_lock& operator=(scoped_lock&&)      = delete;
    };

    std::array<subscriber_slot, MaxSubscribers> slots_{};
    std::array<observer_fn, MaxSubscribers>     observers_{};
    mutable mutex                               mutex_{};
    std::size_t                                 observer_count_{0U};
    std::size_t                                 subscriber_count_{0U};
};

#else

// ===========================================================================
// Hosted fallback used when the Zephyr tag is instantiated outside a Zephyr build
// ===========================================================================

/// @brief Zephyr-tagged pub/sub topic that delegates to the generic runtime on non-Zephyr builds.
template<queue_element T, std::size_t MaxSubscribers, std::size_t PerSubCapacity>
    requires(MaxSubscribers > 0U && PerSubCapacity > 0U)
class osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_zephyr>
    : public osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>
{
public:
    using base_type  = osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>;
    using value_type = T;
};

#endif

}  // namespace osal
