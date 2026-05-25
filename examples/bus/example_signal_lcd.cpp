// SPDX-License-Identifier: Apache-2.0
/// @file example_topic_lcd.cpp
/// @brief Minimal LCD pub/sub example using the generic micrOSAL backend.
///
/// Demonstrates:
///   - Creating an osal_signal with the generic backend.
///   - Subscribing two consumers.
///   - Publishing a message — fan-out to both queues.
///   - try_receive on each subscriber.
///   - Unsubscribing cleanly.
///
/// This example compiles and runs on any micrOSAL backend.
///
/// Build (standalone, e.g. Linux backend):
/// @code
///   cmake -B build -DOSAL_BACKEND=LINUX -DOSAL_BUILD_EXAMPLES=ON
///   cmake --build build
///   ./build/examples/bus/example_topic_lcd
/// @endcode
#include <osal/bus/osal_signal.hpp>

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

int main()
{
    // Define a topic that carries std::uint32_t values,
    // supports up to 4 subscribers, each with a queue depth of 8.
    osal::osal_signal<std::uint32_t, 4U, 8U, osal::bus_backend_generic> sensor_topic;

    // Subscribe two consumers.
    osal::subscriber_id sub_a{osal::invalid_subscriber_id};
    osal::subscriber_id sub_b{osal::invalid_subscriber_id};

    if (!sensor_topic.subscribe(sub_a))
    {
        std::puts("[example] ERROR: subscribe A failed\n");
        return 1;
    }
    if (!sensor_topic.subscribe(sub_b))
    {
        std::puts("[example] ERROR: subscribe B failed\n");
        (void)sensor_topic.unsubscribe(sub_a);
        return 1;
    }

    print_value_line("[example] subscriber_count = ", sensor_topic.subscriber_count());

    // Publish a sensor reading — fans out to both queues.
    const std::uint32_t reading = 1234U;
    if (!sensor_topic.publish(reading))
    {
        std::puts("[example] WARNING: no subscriber received the message\n");
    }

    // Consumer A receives.
    std::uint32_t val_a{0U};
    if (sensor_topic.try_receive(sub_a, val_a))
    {
        print_value_line("[example] subscriber A received: ", val_a);
    }

    // Consumer B receives.
    std::uint32_t val_b{0U};
    if (sensor_topic.try_receive(sub_b, val_b))
    {
        print_value_line("[example] subscriber B received: ", val_b);
    }

    // Publish multiple messages and drain.
    for (std::uint32_t i = 0U; i < 3U; ++i)
    {
        (void)sensor_topic.publish(i * 10U);
    }

    std::puts("[example] draining subscriber A:");
    std::uint32_t v{0U};
    while (sensor_topic.try_receive(sub_a, v))
    {
        print_value_line("  -> ", v);
    }

    // Clean up.
    (void)sensor_topic.unsubscribe(sub_a);
    (void)sensor_topic.unsubscribe(sub_b);

    print_value_line("[example] subscriber_count after cleanup = ", sensor_topic.subscriber_count());
    std::puts("[example] done.");
    return 0;
}
