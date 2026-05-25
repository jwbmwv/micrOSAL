// SPDX-License-Identifier: Apache-2.0
/// @file example_signal_zephyr.cpp
/// @brief Zephyr-tagged pub/sub example.
///
/// Demonstrates osal_signal<T, N, M, bus_backend_zephyr>. Today that backend
/// tag delegates to the generic runtime, so the example runs on hosted builds
/// while preserving the future Zephyr-native integration surface.
///
/// For a future native Zephyr target:
///   - ZBUS_CHAN_DEFINE() must appear at file scope.
///   - Each subscriber registers a ZBUS_SUBSCRIBER_DEFINE listener.
///   - publish() → zbus_chan_pub()
///   - receive() → zbus_sub_wait()
///
/// Build (Zephyr west build):
/// @code
///   west build -b <board> examples/bus
/// @endcode
///
/// Build (hosted fallback run):
/// @code
///   cmake -B build -DOSAL_BACKEND=LINUX -DOSAL_BUILD_EXAMPLES=ON
///   cmake --build build -- example_signal_zephyr
/// @endcode
#include <microsal/bus/osal_signal_premium.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>

namespace
{

template<typename Integer>
void print_value_line(const char* prefix, Integer value)
{
    (void)std::fputs(prefix, stdout);

    std::array<char, 32> buffer{};
    const auto           result = std::to_chars(buffer.data(), buffer.data() + buffer.size() - 1, value);
    if (result.ec == std::errc{})
    {
        *result.ptr = '\n';
        (void)std::fwrite(buffer.data(), 1, static_cast<std::size_t>(result.ptr - buffer.data()) + 1U, stdout);
        return;
    }

    (void)std::fputs("?\n", stdout);
}

}  // namespace

// ---------------------------------------------------------------------------
// On Zephyr: define the channel at file scope with ZBUS_CHAN_DEFINE.
// TODO: ZBUS_CHAN_DEFINE(sensor_chan, std::uint32_t, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0))
// ---------------------------------------------------------------------------

int main()
{
    using TopicType = osal::osal_signal_premium<std::uint32_t, 4U, 8U, osal::bus_backend_zephyr>;

    TopicType sensor_topic;

    // Subscribe a queue-based consumer.
    osal::subscriber_id sub{osal::invalid_subscriber_id};
    if (!sensor_topic.subscribe(sub))
    {
        std::puts("[zephyr-example] subscribe failed\n");
        return 1;
    }

    // Register a Zbus observer callback.
    static auto obs = [](const std::uint32_t& v) noexcept
    { print_value_line("[zephyr-example] observer callback: ", v); };
    // Cast lambda to function pointer (stateless lambda is convertible).
    (void)sensor_topic.subscribe_observer(static_cast<void (*)(const std::uint32_t&) noexcept>(obs));

    // Publish a sensor reading.
    (void)sensor_topic.publish(9876U);

    // Zero-copy publish currently uses the portable copy fallback.
    std::uint32_t msg = 1111U;
    (void)sensor_topic.publish_zero_copy(&msg);

    // Drain queue subscriber.
    std::uint32_t val{0U};
    while (sensor_topic.try_receive(sub, val))
    {
        print_value_line("[zephyr-example] queue-subscriber received: ", val);
    }

    if (sub != osal::invalid_subscriber_id)
    {
        (void)sensor_topic.unsubscribe(sub);
    }

    std::puts("[zephyr-example] done.");
    return 0;
}
