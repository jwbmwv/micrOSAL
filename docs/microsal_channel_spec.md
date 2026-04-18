You are extending micrOSAL with a new capability: a portable, deterministic
"bus + pub/sub" abstraction with two layers:

1. A **Lowest Common Denominator (LCD)** layer that works on:
   - bare-metal
   - POSIX-like systems (Linux, RTEMS with POSIX)
   - FreeRTOS
   - Zephyr
   - any RTOS with basic IPC

2. A **Premium Backend** layer that exposes richer capabilities when the
   underlying RTOS supports them (e.g., Zephyr Zbus, QNX-style messaging,
   native pub/sub, routing, observers, zero-copy).

Design constraints:
- Target **C++20 or better**.
- No dynamic allocation in the core API.
- Deterministic, bounded memory, bounded latency.
- Traits-based, compile-time backend selection.
- Compile-time capability detection (traits, concepts, constexpr flags).
- Zero or minimal runtime overhead.
- No hidden global state.
- Assume `.clang-format` and `.clang-tidy` exist; follow modern C++20 style.

Generic fallback:
- Use **micrOSAL primitives** (mutex, semaphore, queue, timer) as the
  **generic fallback backend**.
- RTOS-specific backends may bypass micrOSAL primitives for performance.
- The generic backend defines the **canonical LCD behavior**.

Core abstractions:
------------------
1. **Typed bus** (point-to-point or per-subscriber queue):
    template<typename T, std::size_t Capacity, typename BackendTag>
    class osal_bus;

2. **Typed signal** (pub/sub):
    template<typename T, std::size_t MaxSubscribers, std::size_t PerSubCapacity,
             typename BackendTag = MICROSAL_DEFAULT_BACKEND_TAG>
    class osal_signal;

LCD pub/sub semantics:
----------------------
LCD pub/sub must be implementable on any backend using only micrOSAL
primitives. The LCD guarantees:

- A signal is a logical broadcast point.
- Subscribers register and receive a unique subscriber_id.
- Each subscriber has a bounded queue (PerSubCapacity).
- publish(msg) fans out to all active subscribers.
- No dynamic allocation.
- Deterministic O(MaxSubscribers) publish cost.
- Optional blocking receive per subscriber.

LCD API:
--------
template<typename T, std::size_t MaxSubscribers, std::size_t PerSubCapacity,
         typename BackendTag = MICROSAL_DEFAULT_BACKEND_TAG>
class osal_signal {
public:
    using value_type = T;

    [[nodiscard]] bool publish(const T& msg);

    [[nodiscard]] bool subscribe(subscriber_id& out_id);
    [[nodiscard]] bool unsubscribe(subscriber_id id);

    [[nodiscard]] bool try_receive(subscriber_id id, T& out);
    [[nodiscard]] bool receive(subscriber_id id, T& out, timeout_type timeout);

    std::size_t subscriber_count() const noexcept;
};

Premium pub/sub (conditionally enabled):
----------------------------------------
Premium backends may expose:

- native pub/sub
- native observers
- native routing
- zero-copy or near-zero-copy publish
- direct mapping to RTOS primitives

Premium API (enabled via traits or concepts):
---------------------------------------------
template<typename T, std::size_t MaxSubscribers, std::size_t PerSubCapacity,
         typename BackendTag = MICROSAL_DEFAULT_BACKEND_TAG>
class osal_signal_premium : public osal_signal<T, MaxSubscribers, PerSubCapacity, BackendTag> {
public:
    // Only enabled when backend supports it
    [[nodiscard]] bool subscribe(observer_fn);
    [[nodiscard]] bool unsubscribe(observer_fn);
    [[nodiscard]] bool publish_zero_copy(T* ptr);
    [[nodiscard]] bool route_to(signal_id, const T& msg);
};

Backend tags:
-------------
- osal_backend_bare_metal
- osal_backend_posix
- osal_backend_freertos
- osal_backend_zephyr
- osal_backend_generic_microsal

Backend traits:
---------------
template<typename BackendTag>
struct osal_signal_capabilities {
    static constexpr bool native_pubsub = false;
    static constexpr bool native_observers = false;
    static constexpr bool native_routing = false;
    static constexpr bool zero_copy = false;
};

Zbus-optimized backend:
-----------------------
For osal_backend_zephyr:

- Map each osal_signal<T> to a Zbus bus.
- publish() → zbus_chan_pub()
- subscribe() → register Zbus observer
- unsubscribe() → unregister observer
- receive() → observer callback enqueues into per-subscriber queue
- premium features enabled:
    native_pubsub = true
    native_observers = true
    native_routing = true
    zero_copy = true

Generic fallback backend:
-------------------------
For osal_backend_generic_microsal:

- Implement osal_signal using:
    - static array of subscriber slots
    - each slot contains:
        bool in_use;
        osal_bus<T, PerSubCapacity, BackendTag> queue;

- publish() iterates over subscriber slots and try_send() into each queue.
- subscribe() finds a free slot.
- unsubscribe() marks slot free.
- receive() delegates to the subscriber's queue.

File layout:
------------
- include/microsal/microsal_config.hpp
- include/microsal/bus/osal_bus.hpp
- include/microsal/bus/osal_signal.hpp
- include/microsal/bus/osal_signal_premium.hpp
- include/microsal/bus/detail/osal_signal_traits.hpp
- include/microsal/bus/detail/osal_signal_backend_*.hpp
- include/microsal/bus/detail/osal_signal_backend_generic.hpp
- tests/bus/test_osal_signal_lcd.cpp
- tests/bus/test_osal_signal_premium.cpp
- examples/bus/example_signal_lcd.cpp
- examples/bus/example_signal_zephyr.cpp
- docs/bus/*.md
- docs/diagrams/*.puml + generated .png/.svg/.pdf

Documentation requirements:
---------------------------
- All docs in `.md` files.
- Use PlantUML for diagrams.
- For each `.puml`, generate `.png`, `.svg`, and `.pdf`.
- Embed diagrams in `.md` files.

Testing requirements:
---------------------
- All generated code must compile and **pass all local tests**.
- Provide a minimal test harness (header-only).
- Include tests for:
  - LCD pub/sub behavior
  - generic fallback backend
  - Zbus backend (mocked if needed)
  - capability detection
  - routing and observer ordering

CI/CD requirements:
-------------------
- Enhance CI/CD to:
  - build all backends
  - run all tests
  - lint with clang-tidy
  - format-check with clang-format
  - generate PlantUML diagrams (png/svg/pdf)
  - verify no dynamic allocation in core paths (static analysis)
  - verify constexpr paths where applicable

Implementation strategy:
------------------------
1. Define backend tags and traits.
2. Implement generic micrOSAL signal backend.
3. Implement mock premium backend for tests.
4. Implement Zbus backend (premium).
5. Implement POSIX and FreeRTOS backends.
6. Add tests, examples, and documentation.
7. Add CI/CD enhancements.

Now generate:
- All file skeletons
- Generic micrOSAL signal backend
- Mock premium backend
- Tests, examples, docs, and PlantUML diagrams
- CI/CD enhancements

Focus on clarity, determinism, and minimal runtime overhead.
