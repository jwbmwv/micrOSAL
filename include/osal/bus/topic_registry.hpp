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
using signal_id                              = std::uint32_t;
inline constexpr signal_id invalid_signal_id = 0U;

enum class route_status
{
    delivered,
    no_subscribers,
    not_found,
    busy,
    invalid
};

/// Typed, borrowed topic registrations. Topics and registry must outlive dispatch.
/// Unregister returns busy while the target is dispatching (including cycles).
template<typename T, std::size_t MaxTopics>
    requires(MaxTopics > 0U)
class topic_registry
{
public:
    [[nodiscard]] bool valid() const noexcept { return mutex_.valid(); }

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
