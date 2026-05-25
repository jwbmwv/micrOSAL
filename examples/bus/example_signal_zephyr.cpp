// SPDX-License-Identifier: Apache-2.0
/// @file example_signal_zephyr.cpp
/// @brief Zephyr-tagged pub/sub example.
///
/// Demonstrates osal_signal<T, N, M, bus_backend_zephyr>. On real Zephyr
/// builds, that tag uses a dedicated queue-backed runtime. Hosted builds that
/// instantiate the tag for compile-time coverage still delegate to the generic
/// runtime, so this example remains runnable outside Zephyr as well.
///
/// Build (Zephyr west build):
/// @code
///   west build -b <board> examples/bus
/// @endcode
///
/// Build (hosted coverage run):
/// @code
///   cmake -B build -DOSAL_BACKEND=LINUX -DOSAL_BUILD_EXAMPLES=ON
///   cmake --build build -- example_signal_zephyr
/// @endcode
#include <osal/bus/osal_signal_premium.hpp>

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

// The Zephyr backend keeps the same fixed-capacity FIFO semantics as the
// generic implementation, but does so with native k_msgq-backed queues.

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

    // Observer registration and dispatch use the Zephyr backend's native hook.
    static auto obs = [](const std::uint32_t& v) noexcept
    { print_value_line("[zephyr-example] observer callback: ", v); };
    // Cast lambda to function pointer (stateless lambda is convertible).
    (void)sensor_topic.subscribe_observer(static_cast<void (*)(const std::uint32_t&) noexcept>(obs));

    // Publish a sensor reading.
    (void)sensor_topic.publish(9876U);

    // Zero-copy publish still uses the portable copy fallback.
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
