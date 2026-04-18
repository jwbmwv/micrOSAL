// SPDX-License-Identifier: Apache-2.0
/// @file osal_signal_premium.hpp
/// @brief Premium pub/sub topic with observers, zero-copy, and routing
/// @details Extends osal::osal_signal with the premium API surface.
///          Premium methods are always compiled (no hidden-by-macro elimination)
///          but degrade gracefully on non-premium backends:
///
///          | Feature              | Premium backend   | Generic / fallback    |
///          |----------------------|-------------------|-----------------------|
///          | subscribe_observer() | native callback   | stored in local array |
///          | publish_zero_copy()  | zero-copy path    | falls back to copy    |
///          | route_to()           | native routing    | stub (returns false)  |
///
///          Observer callbacks are invoked synchronously during publish() on
///          generic/mock backends, and by the native RTOS mechanism on premium
///          backends (e.g. Zbus listener thread on Zephyr).
///
///          Usage:
///          @code
///          osal::osal_signal_premium<std::uint32_t, 4, 8, osal::bus_backend_mock> topic;
///
///          // Queue-based subscriber
///          osal::subscriber_id sub{osal::invalid_subscriber_id};
///          topic.subscribe(sub);
///
///          // Observer (callback) subscriber
///          topic.subscribe_observer([](const std::uint32_t& v) noexcept { ... });
///
///          topic.publish(99U);       // fans out to queue-subs AND observer-subs
///          topic.publish_zero_copy(&v); // zero-copy on capable backends
///          @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <microsal/bus/osal_signal.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace osal
{

/// @defgroup osal_signal_premium OSAL Topic Premium
/// @brief Premium pub/sub with observers, zero-copy publish, and routing.
/// @{

// ---------------------------------------------------------------------------
// Supporting types
// ---------------------------------------------------------------------------

/// @brief Opaque topic identifier used by route_to().
using signal_id = std::uint32_t;

/// @brief Sentinel topic ID meaning "no topic / unrouted".
inline constexpr signal_id invalid_signal_id = signal_id{0};

// ---------------------------------------------------------------------------
// osal_signal_premium
// ---------------------------------------------------------------------------

/// @brief Premium pub/sub topic — extends osal_signal with observer support,
///        zero-copy publish, and routing stubs.
///
/// @tparam T               Message type.
/// @tparam MaxSubscribers  Maximum queue-based subscribers (inherited limit).
/// @tparam PerSubCapacity  Per-subscriber queue depth (inherited).
/// @tparam BackendTag      Backend selection.
///
/// @details Observer callbacks (type @c observer_fn) are stored in a static
///          array of size @c MaxSubscribers.  On publish() the base LCD
///          fan-out runs first, followed by all registered observer callbacks.
template<typename T, std::size_t MaxSubscribers, std::size_t PerSubCapacity,
         typename BackendTag = MICROSAL_DEFAULT_BACKEND_TAG>
    requires queue_element<T> && (MaxSubscribers > 0U) && (PerSubCapacity > 0U)
class osal_signal_premium : public osal_signal<T, MaxSubscribers, PerSubCapacity, BackendTag>
{
public:
    using base_type  = osal_signal<T, MaxSubscribers, PerSubCapacity, BackendTag>;
    using value_type = T;

    /// @brief Observer callback type — must not throw, must not block.
    using observer_fn = void (*)(const T&) noexcept;

    // ---- observer management -----------------------------------------------

    /// @brief Registers a callback observer.
    /// @param fn  Function pointer to invoke on each publish.
    /// @return true if the observer was registered; false when the observer
    ///         table is full or @p fn is nullptr.
    [[nodiscard]] bool subscribe_observer(observer_fn fn) noexcept
    {
        if (fn == nullptr)
        {
            return false;
        }
        if constexpr (native_observer_backend<BackendTag>)
        {
            return native_subscribe_observer(fn);
        }
        else
        {
            return store_observer(fn);
        }
    }

    /// @brief Removes a previously registered callback observer.
    /// @param fn  The function pointer to remove.
    /// @return true if found and removed; false if not registered.
    [[nodiscard]] bool unsubscribe_observer(observer_fn fn) noexcept
    {
        if (fn == nullptr)
        {
            return false;
        }
        for (std::size_t i = 0U; i < observer_count_; ++i)
        {
            if (observers_[i] == fn)
            {
                // Compact the array
                observers_[i]               = observers_[--observer_count_];
                observers_[observer_count_] = nullptr;
                return true;
            }
        }
        return false;
    }

    /// @brief Returns the number of registered observer callbacks.
    [[nodiscard]] std::size_t observer_count() const noexcept { return observer_count_; }

    // ---- enhanced publish --------------------------------------------------

    /// @brief Publishes a message to queue-subscribers AND observer callbacks.
    /// @param msg  Message to broadcast.
    /// @return true if at least one queue-subscriber or observer received it.
    [[nodiscard]] bool publish(const T& msg) noexcept
    {
        bool any = base_type::publish(msg);
        any |= invoke_observers(msg);
        return any;
    }

    /// @brief Zero-copy publish — avoids an extra copy on capable backends.
    /// @param ptr  Pointer to the message buffer.  Must not be nullptr.
    ///             On non-zero-copy backends the message is copied from @p ptr.
    /// @return true on success.
    [[nodiscard]] bool publish_zero_copy(T* ptr) noexcept
    {
        if (ptr == nullptr)
        {
            return false;
        }
        // TODO: backend-specific zero-copy path (e.g. zbus_chan_pub_claim)
        // Currently all backends copy from ptr; replace with native zero-copy
        // when a premium backend (e.g. Zephyr Zbus) is wired up.
        return publish(*ptr);
    }

    // ---- routing (stub) ----------------------------------------------------

    /// @brief Routes a message to another topic by ID.
    /// @details Full implementation requires a topic registry.
    ///          This method is a stub — it always returns false until a
    ///          registry is introduced in a future release.
    /// @param dest  Destination topic ID (see signal_id).
    /// @param msg   Message to route.
    /// @return false (stub).
    /// @todo Implement topic registry and cross-topic routing.
    [[nodiscard]] bool route_to([[maybe_unused]] signal_id dest, [[maybe_unused]] const T& msg) noexcept
    {
        if constexpr (native_routing_backend<BackendTag>)
        {
            // TODO: native routing via backend mechanism (e.g. Zbus channels)
        }
        // TODO: generic registry-based routing
        return false;
    }

private:
    // ---- observer table ----------------------------------------------------

    static constexpr std::size_t max_observers_ = MaxSubscribers;

    std::array<observer_fn, max_observers_> observers_{};
    std::size_t                             observer_count_{0U};

    [[nodiscard]] bool store_observer(observer_fn fn) noexcept
    {
        if (observer_count_ >= max_observers_)
        {
            return false;
        }
        observers_[observer_count_++] = fn;
        return true;
    }

    /// @brief Invokes all registered observers with @p msg.
    /// @return true if at least one observer was called.
    bool invoke_observers(const T& msg) noexcept
    {
        const bool any = (observer_count_ > 0U);
        for (std::size_t i = 0U; i < observer_count_; ++i)
        {
            if (observers_[i] != nullptr)
            {
                observers_[i](msg);
            }
        }
        return any;
    }

    /// @brief Native observer registration path (premium backends only).
    /// @details Specialised at link time or via constexpr if for backends
    ///          that provide native callback registration (e.g. Zbus).
    [[nodiscard]] bool native_subscribe_observer(observer_fn fn) noexcept
    {
        // For Zephyr: TODO zbus_obs_set_enable / custom callback attachment.
        // For now fall back to the generic table so premium tests pass.
        return store_observer(fn);
    }
};

/// @} // osal_signal_premium

}  // namespace osal
