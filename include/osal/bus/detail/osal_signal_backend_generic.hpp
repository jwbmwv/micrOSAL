// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_backend_generic.hpp
/// @brief Generic micrOSAL channel + topic backend (LCD implementation)
/// @details Provides osal::osal_bus and osal::osal_signal specialisations
///          for osal::bus_backend_generic.
///
///          Implementation uses only micrOSAL primitives:
///          - Per-subscriber queue: backed by osal_queue_create / osal_queue_receive
///            (avoids the ms-to-tick conversion in osal::queue::receive_for by
///             calling the C ABI directly with osal::tick_t).
///          - Subscriber slot management: protected by osal::mutex.
///
///          No dynamic allocation. All storage is compile-time static.
///          Publish cost: O(MaxSubscribers) try_send calls.
///
/// @warning Instances must be created after RTOS initialisation — the
///          constructor calls osal_queue_create() and osal_mutex_create().
///          Avoid static / global instances unless your platform supports
///          static-constructor RTOS calls.
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <osal/bus/detail/osal_bus_fwd.hpp>
#include <osal/queue.hpp>
#include <osal/mutex.hpp>
#include <osal/types.hpp>
#include <osal/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Bring in the C queue ABI (declared in <osal/queue.hpp>).
// osal_queue_create / osal_queue_destroy / osal_queue_send / osal_queue_receive
// are already visible after including <osal/queue.hpp>.

namespace osal
{

// ===========================================================================
// osal_bus<T, Capacity, bus_backend_generic>
// ===========================================================================

/// @brief Generic channel specialisation — queue backed by the micrOSAL C ABI.
/// @tparam T         Message type (trivially copyable + standard layout).
/// @tparam Capacity  Maximum number of buffered messages.
///
/// @details This specialisation owns its backing storage and calls the
///          osal_queue_* C functions directly so that @c tick_t timeouts
///          are forwarded verbatim without a ticks↔ms round-trip.
template<queue_element T, std::size_t Capacity>
    requires(Capacity > 0U)
class osal_bus<T, Capacity, bus_backend_generic>
{
public:
    using value_type                        = T;
    static constexpr std::size_t capacity   = Capacity;
    static constexpr std::size_t value_size = sizeof(T);

    // ---- construction / destruction ----------------------------------------

    osal_bus() noexcept { valid_ = osal_queue_create(&handle_, storage_, value_size, Capacity).ok(); }

    ~osal_bus() noexcept
    {
        if (valid_)
        {
            (void)osal_queue_destroy(&handle_);
            valid_ = false;
        }
    }

    osal_bus(const osal_bus&)            = delete;
    osal_bus& operator=(const osal_bus&) = delete;
    osal_bus(osal_bus&&)                 = delete;
    osal_bus& operator=(osal_bus&&)      = delete;

    // ---- query -------------------------------------------------------------

    /// @brief Returns true if the underlying queue was successfully created.
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    // ---- send --------------------------------------------------------------

    /// @brief Non-blocking send. Returns true if the item was enqueued.
    bool try_send(const T& item) noexcept { return osal_queue_send(&handle_, &item, NO_WAIT).ok(); }

    /// @brief Blocking send — waits at most @p timeout_ticks ticks.
    bool send(const T& item, tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        return osal_queue_send(&handle_, &item, timeout_ticks).ok();
    }

    // ---- receive -----------------------------------------------------------

    /// @brief Non-blocking receive. Returns true if an item was dequeued.
    bool try_receive(T& out) noexcept { return osal_queue_receive(&handle_, &out, NO_WAIT).ok(); }

    /// @brief Blocking receive — waits at most @p timeout_ticks ticks.
    bool receive(T& out, tick_t timeout_ticks = WAIT_FOREVER) noexcept
    {
        return osal_queue_receive(&handle_, &out, timeout_ticks).ok();
    }

private:
    template<typename U, std::size_t MaxSignalSubscribers, std::size_t SignalPerSubCapacity, typename BackendTag>
    friend class osal_signal;

    /// @brief Clears any buffered messages without blocking.
    void purge() noexcept
    {
        if (!valid_)
        {
            return;
        }

        std::array<std::byte, value_size> scratch{};
        while (osal_queue_receive(&handle_, scratch.data(), NO_WAIT).ok())
        {
        }
    }

    bool                          valid_{false};
    active_traits::queue_handle_t handle_{};
    alignas(T) std::uint8_t storage_[Capacity * sizeof(T)]{};
};

// ===========================================================================
// osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>
// ===========================================================================

/// @brief Generic LCD pub/sub topic specialisation.
/// @tparam T               Message type (trivially copyable + standard layout).
/// @tparam MaxSubscribers  Compile-time upper bound on concurrent subscribers.
/// @tparam PerSubCapacity  Depth of each subscriber's receive queue.
///
/// @details Implements the full LCD guarantee:
///          - Deterministic O(MaxSubscribers) publish.
///          - No dynamic allocation.
///          - Optional blocking receive per subscriber.
///          - Thread-safe subscribe / unsubscribe via osal::mutex.
template<queue_element T, std::size_t MaxSubscribers, std::size_t PerSubCapacity>
    requires(MaxSubscribers > 0U && PerSubCapacity > 0U)
class osal_signal<T, MaxSubscribers, PerSubCapacity, bus_backend_generic>
{
public:
    using value_type = T;

    // ---- subscribe / unsubscribe -------------------------------------------

    /// @brief Registers a new subscriber and returns its ID.
    /// @param[out] out_id  Receives the allocated subscriber_id on success.
    /// @return true if a free slot was available; false when at capacity.
    /// @blocking Never (mutex-locked, O(MaxSubscribers) scan).
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

    /// @brief Removes a subscriber by ID.
    /// @param id  Subscriber ID obtained from subscribe().
    /// @return true if the ID was valid and active; false otherwise.
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

    // ---- publish -----------------------------------------------------------

    /// @brief Fans the message out to all active subscriber queues.
    /// @param msg  Message to broadcast (copied into each subscriber queue).
    /// @return true if at least one subscriber received the message.
    /// @blocking Never (uses try_send; drops to full subscriber queues).
    /// @complexity O(MaxSubscribers).
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

    // ---- receive -----------------------------------------------------------

    /// @brief Non-blocking receive for a specific subscriber.
    /// @param id   Subscriber ID.
    /// @param[out] out  Receives the dequeued message on success.
    /// @return true if a message was available.
    [[nodiscard]] bool try_receive(subscriber_id id, T& out) noexcept
    {
        const std::size_t slot_index = slot_index_from_id_(id);
        if (slot_index >= MaxSubscribers || !subscriber_id_matches_slot_(id, slot_index))
        {
            return false;
        }
        return slots_[slot_index].queue_.try_receive(out);
    }

    /// @brief Blocking receive with optional timeout.
    /// @param id       Subscriber ID.
    /// @param[out] out Receives the dequeued message on success.
    /// @param timeout  Maximum wait in ticks; use WAIT_FOREVER to block indefinitely.
    /// @return true if a message was received within the timeout.
    [[nodiscard]] bool receive(subscriber_id id, T& out, tick_t timeout = WAIT_FOREVER) noexcept
    {
        const std::size_t slot_index = slot_index_from_id_(id);
        if (slot_index >= MaxSubscribers || !subscriber_id_matches_slot_(id, slot_index))
        {
            return false;
        }
        return slots_[slot_index].queue_.receive(out, timeout);
    }

    // ---- query -------------------------------------------------------------

    /// @brief Returns the current number of active subscribers.
    [[nodiscard]] std::size_t subscriber_count() const noexcept { return subscriber_count_; }

    /// @brief Returns the compile-time maximum subscriber count.
    static constexpr std::size_t max_subscribers() noexcept { return MaxSubscribers; }

    /// @brief Returns the compile-time per-subscriber queue capacity.
    static constexpr std::size_t per_subscriber_capacity() noexcept { return PerSubCapacity; }

private:
    // ---- subscriber slot ---------------------------------------------------

    using subscriber_generation_t = subscriber_id;

    static constexpr subscriber_id           subscriber_id_span_ = static_cast<subscriber_id>(MaxSubscribers);
    static constexpr subscriber_generation_t subscriber_generation_modulus_ =
        invalid_subscriber_id / subscriber_id_span_;

    static_assert(subscriber_generation_modulus_ > 0U,
                  "subscriber_id does not provide enough distinct values for this MaxSubscribers");

    struct subscriber_slot
    {
        subscriber_generation_t                          generation_{0U};
        bool                                             in_use_{false};
        osal_bus<T, PerSubCapacity, bus_backend_generic> queue_{};
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

    // ---- RAII mutex guard (private helper) ---------------------------------

    struct [[nodiscard]] scoped_lock
    {
        mutex& mtx_;

        explicit scoped_lock(mutex& m) noexcept : mtx_{m} { mtx_.lock(); }
        ~scoped_lock() noexcept { mtx_.unlock(); }

        scoped_lock(const scoped_lock&)            = delete;
        scoped_lock& operator=(const scoped_lock&) = delete;
        scoped_lock(scoped_lock&&)                 = delete;
        scoped_lock& operator=(scoped_lock&&)      = delete;
    };

    // ---- data --------------------------------------------------------------

    std::array<subscriber_slot, MaxSubscribers> slots_{};
    mutex                                       mutex_;
    std::size_t                                 subscriber_count_{0U};
};

}  // namespace osal
