// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_backend_zephyr.hpp
/// @brief Zephyr bus + signal backend tag
/// @details On real Zephyr builds this header provides a dedicated native
///          implementation using Zephyr kernel primitives directly. On hosted
///          builds that instantiate @c bus_backend_zephyr only for compile-time
///          coverage, the tag still delegates to the generic runtime so the
///          cross-backend doctest matrix keeps compiling.
///
///          Premium capability traits remain limited to the functionality that
///          is actually implemented today.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <osal/bus/detail/osal_signal_backend_generic.hpp>

#if defined(OSAL_BACKEND_ZEPHYR)
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

namespace zephyr_bus_detail
{

[[nodiscard]] inline k_timeout_t to_zephyr_timeout(tick_t timeout_ticks) noexcept
{
    if (timeout_ticks == WAIT_FOREVER)
    {
        return K_FOREVER;
    }
    if (timeout_ticks == NO_WAIT)
    {
        return K_NO_WAIT;
    }
    return K_MSEC(static_cast<int64_t>(timeout_ticks));
}

}  // namespace zephyr_bus_detail

// ===========================================================================
// osal_bus<T, Capacity, bus_backend_zephyr>
// ===========================================================================

/// @brief Zephyr-native channel backed by @c k_msgq.
template<queue_element T, std::size_t Capacity>
    requires(Capacity > 0U)
class osal_bus<T, Capacity, bus_backend_zephyr>
{
public:
    using value_type                      = T;
    static constexpr std::size_t capacity = Capacity;

    osal_bus() noexcept
    {
        k_msgq_init(&queue_, reinterpret_cast<char*>(storage_), sizeof(T), Capacity);
        valid_ = true;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    bool try_send(const T& item) noexcept { return k_msgq_put(&queue_, &item, K_NO_WAIT) == 0; }

    bool send(const T& item, tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        return k_msgq_put(&queue_, &item, zephyr_bus_detail::to_zephyr_timeout(timeout_ticks)) == 0;
    }

    bool try_receive(T& out) noexcept { return k_msgq_get(&queue_, &out, K_NO_WAIT) == 0; }

    bool receive(T& out, tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        return k_msgq_get(&queue_, &out, zephyr_bus_detail::to_zephyr_timeout(timeout_ticks)) == 0;
    }

private:
    template<typename U, std::size_t MaxSignalSubscribers, std::size_t SignalPerSubCapacity, typename BackendTag>
    friend class osal_signal;

    /// @brief Clears any buffered messages without blocking.
    void purge() noexcept
    {
        std::array<std::byte, sizeof(T)> scratch{};
        while (k_msgq_get(&queue_, scratch.data(), K_NO_WAIT) == 0)
        {
        }
    }

    bool valid_{false};
    struct k_msgq queue_
    {
    };
    alignas(T) std::uint8_t storage_[Capacity * sizeof(T)]{};
};

// ===========================================================================
// osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_zephyr>
// ===========================================================================

/// @brief Zephyr-native LCD pub/sub topic.
/// @details Uses a direct Zephyr-backed queue per subscriber slot so the
///          public MicrOSAL publish / receive contract matches the generic
///          implementation while avoiding the generic C queue ABI layer.
template<queue_element T, std::size_t MaxSubscribers, std::size_t PerSubCapacity>
    requires(MaxSubscribers > 0U && PerSubCapacity > 0U)
class osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_zephyr>
{
public:
    using value_type  = T;
    using observer_fn = void (*)(const T&) noexcept;

    [[nodiscard]] bool subscribe(subscriber_id& out_id) noexcept
    {
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

    [[nodiscard]] bool unsubscribe(subscriber_id id) noexcept
    {
        scoped_lock       lk{mutex_};
        const std::size_t slot_index = slot_index_from_id_(id);
        if (slot_index >= MaxSubscribers || !subscriber_id_matches_slot_(id, slot_index))
        {
            return false;
        }

        slots_[slot_index].in_use_ = false;
        advance_subscriber_generation_(slot_index);
        --subscriber_count_;
        return true;
    }

    [[nodiscard]] bool publish(const T& msg) noexcept
    {
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

    [[nodiscard]] bool try_receive(subscriber_id id, T& out) noexcept
    {
        const std::size_t slot_index = slot_index_from_id_(id);
        if (slot_index >= MaxSubscribers || !subscriber_id_matches_slot_(id, slot_index))
        {
            return false;
        }

        return slots_[slot_index].queue_.try_receive(out);
    }

    [[nodiscard]] bool receive(subscriber_id id, T& out, tick_t timeout = WAIT_FOREVER) noexcept
    {
        const std::size_t slot_index = slot_index_from_id_(id);
        if (slot_index >= MaxSubscribers || !subscriber_id_matches_slot_(id, slot_index))
        {
            return false;
        }

        return slots_[slot_index].queue_.receive(out, timeout);
    }

    [[nodiscard]] bool native_subscribe_observer(observer_fn fn) noexcept
    {
        if (fn == nullptr)
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

    [[nodiscard]] bool native_unsubscribe_observer(observer_fn fn) noexcept
    {
        if (fn == nullptr)
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

    [[nodiscard]] std::size_t native_observer_count() const noexcept { return observer_count_; }

    [[nodiscard]] bool native_publish_observers(const T& msg) noexcept
    {
        std::array<observer_fn, MaxSubscribers> snapshot{};
        std::size_t                             snapshot_count{0U};

        {
            scoped_lock lk{mutex_};
            snapshot_count = observer_count_;
            for (std::size_t i = 0U; i < snapshot_count; ++i)
            {
                snapshot[i] = observers_[i];
            }
        }

        const bool any = snapshot_count > 0U;
        for (std::size_t i = 0U; i < snapshot_count; ++i)
        {
            if (snapshot[i] != nullptr)
            {
                snapshot[i](msg);
            }
        }
        return any;
    }

    [[nodiscard]] std::size_t subscriber_count() const noexcept { return subscriber_count_; }

    static constexpr std::size_t max_subscribers() noexcept { return MaxSubscribers; }

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
        osal_bus<T, PerSubCapacity, bus_backend_zephyr> queue_{};
    };

    [[nodiscard]] subscriber_id subscriber_cookie_() const noexcept
    {
        constexpr subscriber_id mix = static_cast<subscriber_id>(0x9E3779B97F4A7C15ULL);
        return static_cast<subscriber_id>(reinterpret_cast<std::uintptr_t>(this)) ^ mix;
    }

    [[nodiscard]] static constexpr subscriber_id pack_subscriber_id_(std::size_t             slot_index,
                                                                     subscriber_generation_t generation) noexcept
    {
        return generation * subscriber_id_span_ + static_cast<subscriber_id>(slot_index) + 1U;
    }

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

    void advance_subscriber_generation_(std::size_t slot_index) noexcept
    {
        auto& generation = slots_[slot_index].generation_;
        generation       = (generation + 1U) % subscriber_generation_modulus_;
        if (encode_subscriber_id_(slot_index, generation) == invalid_subscriber_id)
        {
            generation = (generation + 1U) % subscriber_generation_modulus_;
        }
    }

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
    mutex                                       mutex_{};
    std::size_t                                 observer_count_{0U};
    std::size_t                                 subscriber_count_{0U};
};

#else

// ===========================================================================
// Hosted fallback used when the Zephyr tag is instantiated outside a Zephyr build
// ===========================================================================

/// @brief Zephyr-tagged channel that delegates to the generic runtime on non-Zephyr builds.
template<queue_element T, std::size_t Capacity>
    requires(Capacity > 0U)
class osal_bus<T, Capacity, bus_backend_zephyr>
{
public:
    using value_type                      = T;
    static constexpr std::size_t capacity = Capacity;

    [[nodiscard]] bool valid() const noexcept { return impl_.valid(); }

    bool try_send(const T& item) noexcept { return impl_.try_send(item); }

    bool send(const T& item, tick_t timeout_ticks = WAIT_FOREVER) noexcept { return impl_.send(item, timeout_ticks); }

    bool try_receive(T& out) noexcept { return impl_.try_receive(out); }

    bool receive(T& out, tick_t timeout_ticks = WAIT_FOREVER) noexcept { return impl_.receive(out, timeout_ticks); }

private:
    osal_bus<T, Capacity, bus_backend_generic> impl_{};
};

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
