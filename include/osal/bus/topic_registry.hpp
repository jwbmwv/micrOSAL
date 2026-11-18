// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <osal/mutex.hpp>
#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <mutex>

namespace osal
{
/// @brief Identifier used to address a topic registration.
using signal_id = std::uint32_t;
/// @brief Sentinel used for an invalid or unavailable topic identifier.
inline constexpr signal_id invalid_signal_id = 0U;

/// @brief Outcome of a typed topic routing operation.
enum class route_status
{
    delivered,       ///< Destination accepted the message.
    no_subscribers,  ///< Destination exists but accepted no message.
    not_found,       ///< No registration has the requested identifier.
    busy,            ///< The destination is already being dispatched.
    invalid          ///< The registry mutex is not initialized.
};

/// @brief Fixed-capacity registry for routing messages to borrowed topics.
/// @details Registered topics are not owned. Each topic and this registry must
///          remain alive while registered and during dispatch. A topic cannot
///          be unregistered while its dispatch is active; recursive/cyclic
///          dispatch to that topic reports @c route_status::busy.
/// @note Registry operations use a mutex and are not ISR-safe.
/// @tparam T         Message type published by every registered topic.
/// @tparam MaxTopics Maximum number of registrations; must be nonzero.
template<typename T, std::size_t MaxTopics>
    requires(MaxTopics > 0U)
class topic_registry
{
public:
    /// @brief Reports whether the registry mutex initialized.
    /// @return @c true when registry operations can be used.
    [[nodiscard]] bool valid() const noexcept { return mutex_.valid(); }

    /// @brief Registers a compatible topic without taking ownership.
    /// @tparam Topic  Topic type whose @c value_type is @c T and whose publish accepts @c T.
    /// @param[in]  topic  Topic to borrow; it must outlive its registration.
    /// @param[out] out    Receives a non-sentinel identifier on success, or the invalid sentinel on failure.
    /// @return @c true when registered; @c false for an invalid registry, duplicate topic, full registry,
    ///         or exhausted identifier space.
    /// @pre The topic remains alive until unregister_topic() succeeds.
    template<typename Topic>
        requires std::same_as<typename Topic::value_type, T> && requires(Topic& topic, const T& value) {
            { topic.publish(value) } noexcept -> std::same_as<bool>;
        }
    [[nodiscard]] bool register_topic(Topic& topic, signal_id& out) noexcept
    {
        out = invalid_signal_id;
        if (!valid())
            return false;
        std::lock_guard lock{mutex_};
        for (const auto& entry : entries_)
            if (entry.topic == &topic)
                return false;
        for (auto& entry : entries_)
        {
            if (entry.topic == nullptr && next_id_ != invalid_signal_id)
            {
                entry = {&topic, [](void* object, const T& value) noexcept
                         { return static_cast<Topic*>(object)->publish(value); }, next_id_++, false};
                out   = entry.id;
                return true;
            }
        }
        return false;
    }

    /// @brief Removes a topic registration when it is not being dispatched.
    /// @param[in] id  Identifier returned by register_topic().
    /// @return @c route_status::delivered if removed, @c route_status::busy if dispatch is active,
    ///         @c route_status::not_found if no registration matches, or @c route_status::invalid
    ///         when the registry is not initialized.
    [[nodiscard]] route_status unregister_topic(signal_id id) noexcept
    {
        if (!valid())
            return route_status::invalid;
        std::lock_guard lock{mutex_};
        for (auto& entry : entries_)
        {
            if (entry.topic != nullptr && entry.id == id)
            {
                if (entry.active)
                    return route_status::busy;
                entry = {};
                return route_status::delivered;
            }
        }
        return route_status::not_found;
    }

    /// @brief Synchronously publishes a value to the registered topic identified by @p id.
    /// @param[in] id     Destination identifier returned by register_topic().
    /// @param[in] value  Value passed to the destination's publish function.
    /// @return @c route_status::delivered if accepted, @c route_status::no_subscribers if the
    ///         topic accepted no message, @c route_status::busy for recursive/concurrent dispatch,
    ///         @c route_status::not_found when @p id is unknown, or @c route_status::invalid
    ///         when the registry is not initialized.
    /// @note Dispatch runs in the caller's context and may synchronously invoke topic observers.
    /// @pre The registered topic remains alive for the duration of this call.
    [[nodiscard]] route_status dispatch(signal_id id, const T& value) noexcept
    {
        if (!valid())
            return route_status::invalid;
        entry_type* selected = nullptr;
        {
            std::lock_guard lock{mutex_};
            for (auto& entry : entries_)
            {
                if (entry.topic != nullptr && entry.id == id)
                {
                    if (entry.active)
                        return route_status::busy;
                    entry.active = true;
                    selected     = &entry;
                    break;
                }
            }
        }
        if (selected == nullptr)
            return route_status::not_found;
        const bool accepted = selected->publish(selected->topic, value);
        {
            std::lock_guard lock{mutex_};
            selected->active = false;
        }
        return accepted ? route_status::delivered : route_status::no_subscribers;
    }

private:
    struct entry_type
    {
        void* topic{};
        bool (*publish)(void*, const T&) noexcept {};
        signal_id id{};
        bool      active{};
    };
    mutex                             mutex_;
    std::array<entry_type, MaxTopics> entries_{};
    signal_id                         next_id_{1U};
};
}  // namespace osal
