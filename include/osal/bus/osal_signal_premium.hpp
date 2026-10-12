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
///          | publish_zero_copy(T*)| copying overload  | copying overload      |
///          | route_to()           | typed registry    | typed registry        |
///
///          Observer callbacks are invoked synchronously during publish() on
///          the currently implemented backends. Generic backends store them in
///          the premium wrapper itself, while capable backends can own the
///          observer registration and dispatch path.
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
///          topic.publish_zero_copy(&v); // compatibility copying overload
///          @endcode
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
/// @ingroup osal_bus
#pragma once

#include <osal/bus/osal_signal.hpp>
#include <osal/bus/topic_registry.hpp>
#include <osal/bus/zero_copy_signal.hpp>
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

// ---------------------------------------------------------------------------
// osal_signal_premium
// ---------------------------------------------------------------------------

/// @brief Premium pub/sub topic — extends osal_signal with observer support,
///        copying pointer publish, and typed registry routing.
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

private:
    static constexpr bool uses_backend_native_observers_ =
        native_observer_backend<BackendTag> &&
        requires(base_type& topic, const base_type& const_topic, observer_fn fn, const T& msg) {
            { topic.native_subscribe_observer(fn) } -> std::same_as<bool>;
            { topic.native_unsubscribe_observer(fn) } -> std::same_as<bool>;
            { const_topic.native_observer_count() } -> std::convertible_to<std::size_t>;
            { topic.native_publish_observers(msg) } -> std::same_as<bool>;
        };

public:
    // ---- observer management -----------------------------------------------

    /// @brief Registers a callback observer.
    /// @param[in] fn  Function pointer to invoke on each publish.
    /// @return true if the observer was registered; false when the observer
    ///         table is full or @p fn is nullptr.
    [[nodiscard]] bool subscribe_observer(observer_fn fn) noexcept
    {
        if (fn == nullptr)
        {
            return false;
        }
        if constexpr (uses_backend_native_observers_)
        {
            return this->native_subscribe_observer(fn);
        }
        else
        {
            return store_observer(fn);
        }
    }

    /// @brief Removes a previously registered callback observer.
    /// @param[in] fn  The function pointer to remove.
    /// @return true if found and removed; false if not registered.
    [[nodiscard]] bool unsubscribe_observer(observer_fn fn) noexcept
    {
        if (fn == nullptr)
        {
            return false;
        }
        if constexpr (uses_backend_native_observers_)
        {
            return this->native_unsubscribe_observer(fn);
        }
        if (!observer_mutex_.valid())
            return false;
        std::lock_guard lock{observer_mutex_};
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
    [[nodiscard]] std::size_t observer_count() const noexcept
    {
        if constexpr (uses_backend_native_observers_)
        {
            return this->native_observer_count();
        }
        if (!observer_mutex_.valid())
            return 0U;
        std::lock_guard lock{observer_mutex_};
        return observer_count_;
    }

    // ---- enhanced publish --------------------------------------------------

    /// @brief Publishes a message to queue-subscribers AND observer callbacks.
    /// @param[in] msg  Message to broadcast.
    /// @return true if at least one queue-subscriber or observer received it.
    [[nodiscard]] bool publish(const T& msg) noexcept
    {
        bool any = base_type::publish(msg);
        if constexpr (uses_backend_native_observers_)
        {
            any |= this->native_publish_observers(msg);
        }
        else
        {
            any |= invoke_observers(msg);
        }
        return any;
    }

    /// @brief Compatibility copying overload; does not transfer pointer ownership.
    /// @param[in] ptr  Borrowed message pointer. Must not be nullptr.
    /// @details Use zero_copy_signal::publish_zero_copy(producer_loan&&) for
    ///          ownership-preserving transport; this overload always copies.
    /// @return true on success.
    [[nodiscard]] bool publish_zero_copy(T* ptr) noexcept
    {
        if (ptr == nullptr)
        {
            return false;
        }
        return publish(*ptr);
    }

    // ---- typed routing ----------------------------------------------------

    /// Borrow registry until unbind_registry() succeeds; registry must outlive routing.
    template<std::size_t MaxTopics>
    [[nodiscard]] bool bind_registry(topic_registry<T, MaxTopics>& registry) noexcept
    {
        if (!route_mutex_.valid() || !registry.valid())
            return false;
        std::lock_guard lock{route_mutex_};
        if (routing_ != 0U || registry_ != nullptr)
            return false;
        registry_ = &registry;
        dispatch_ = [](void* context, signal_id id, const T& value) noexcept
        { return static_cast<topic_registry<T, MaxTopics>*>(context)->dispatch(id, value); };
        return true;
    }

    [[nodiscard]] bool unbind_registry() noexcept
    {
        if (!route_mutex_.valid())
            return false;
        std::lock_guard lock{route_mutex_};
        if (routing_ != 0U)
            return false;
        registry_ = nullptr;
        dispatch_ = nullptr;
        return true;
    }

    template<std::size_t MaxTopics>
    [[nodiscard]] route_status route_to(topic_registry<T, MaxTopics>& registry, signal_id dest, const T& msg) noexcept
    {
        return registry.dispatch(dest, msg);
    }

    /// @brief Routes a message to another topic by ID.
    /// @details Dispatches through the borrowed registry installed by bind_registry().
    /// @param[in] dest  Destination topic ID (see signal_id).
    /// @param[in] msg   Message to route.
    /// @return true only when the destination accepted the message.
    [[nodiscard]] bool route_to(signal_id dest, const T& msg) noexcept
    {
        if (!route_mutex_.valid())
            return false;
        void* registry{};
        route_status (*dispatch)(void*, signal_id, const T&) noexcept {};
        {
            std::lock_guard lock{route_mutex_};
            if (!registry_)
                return false;
            ++routing_;
            registry = registry_;
            dispatch = dispatch_;
        }
        const auto status = dispatch(registry, dest, msg);
        {
            std::lock_guard lock{route_mutex_};
            --routing_;
        }
        return status == route_status::delivered;
    }

private:
    // ---- observer table ----------------------------------------------------

    static constexpr std::size_t max_observers_ = MaxSubscribers;

    std::array<observer_fn, max_observers_> observers_{};
    std::size_t                             observer_count_{0U};
    mutable mutex                           observer_mutex_;
    mutex                                   route_mutex_;
    void*                                   registry_{};
    route_status (*dispatch_)(void*, signal_id, const T&) noexcept {};
    std::size_t routing_{};

    [[nodiscard]] bool store_observer(observer_fn fn) noexcept
    {
        if (!observer_mutex_.valid())
            return false;
        std::lock_guard lock{observer_mutex_};
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
        if (!observer_mutex_.valid())
            return false;
        std::array<observer_fn, max_observers_> snapshot{};
        std::size_t                             count{};
        {
            std::lock_guard lock{observer_mutex_};
            snapshot = observers_;
            count    = observer_count_;
        }
        const bool any = (count > 0U);
        for (std::size_t i = 0U; i < count; ++i)
        {
            if (snapshot[i] != nullptr)
            {
                snapshot[i](msg);
            }
        }
        return any;
    }
};

/// @} // osal_signal_premium

}  // namespace osal
