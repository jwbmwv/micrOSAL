// SPDX-License-Identifier: Apache-2.0
/// @file example_topic_zephyr.cpp
/// @brief Zephyr Zbus-backed pub/sub example (skeleton).
///
/// Demonstrates the intended usage of osal_signal<T, N, M, bus_backend_zephyr>
/// inside a Zephyr application.  The actual Zbus integration is marked with
/// TODO — this skeleton compiles on generic backends and shows the API surface.
///
/// On a real Zephyr target:
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
/// Build (standalone generic fallback, for compilation checks):
/// @code
///   cmake -B build -DOSAL_BACKEND=LINUX -DOSAL_BUILD_EXAMPLES=ON
///   cmake --build build -- example_topic_zephyr
/// @endcode
#include <microsal/bus/osal_signal_premium.hpp>
#include <cstdint>
#include <cstdio>

// ---------------------------------------------------------------------------
// On Zephyr: define the channel at file scope with ZBUS_CHAN_DEFINE.
// TODO: ZBUS_CHAN_DEFINE(sensor_chan, std::uint32_t, NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0))
// ---------------------------------------------------------------------------

int main()
{
    // The bus_backend_zephyr topic stubs return false until wired to zbus.
    // Swap to bus_backend_generic to run this example on a non-Zephyr host.
#if defined(OSAL_BACKEND_ZEPHYR)
    using TopicType = osal::osal_signal_premium<std::uint32_t, 4U, 8U, osal::bus_backend_zephyr>;
#else
    // Compile-check on non-Zephyr hosts using the generic fallback.
    using TopicType = osal::osal_signal_premium<std::uint32_t, 4U, 8U, osal::bus_backend_generic>;
#endif

    TopicType sensor_topic;

    // Subscribe a queue-based consumer.
    osal::subscriber_id sub{osal::invalid_subscriber_id};
    if (!sensor_topic.subscribe(sub))
    {
        std::puts("[zephyr-example] subscribe failed (stub backend returns false on Zephyr)\n");
        // Expected until zbus wiring is complete.
    }

    // Register a Zbus observer callback.
    static auto obs = [](const std::uint32_t& v) noexcept {
        std::printf("[zephyr-example] observer callback: %u\n", static_cast<unsigned>(v));
    };
    // Cast lambda to function pointer (stateless lambda is convertible).
    (void)sensor_topic.subscribe_observer(static_cast<void(*)(const std::uint32_t&) noexcept>(obs));

    // Publish a sensor reading.
    (void)sensor_topic.publish(9876U);

    // Zero-copy publish (maps to zbus_chan_pub_claim on Zephyr).
    std::uint32_t msg = 1111U;
    (void)sensor_topic.publish_zero_copy(&msg);

    // Drain queue subscriber.
    std::uint32_t val{0U};
    while (sensor_topic.try_receive(sub, val))
    {
        std::printf("[zephyr-example] queue-subscriber received: %u\n", static_cast<unsigned>(val));
    }

    if (sub != osal::invalid_subscriber_id)
    {
        (void)sensor_topic.unsubscribe(sub);
    }

    std::puts("[zephyr-example] done.");
    return 0;
}
