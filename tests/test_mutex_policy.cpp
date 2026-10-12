// SPDX-License-Identifier: Apache-2.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <osal/osal.hpp>
#include <osal/osal_c.h>
#include <atomic>
#include <thread>

TEST_CASE("checked mutex close and acquisition preserve errors")
{
    osal::mutex mutex;
    REQUIRE(mutex.initialization_status().ok());
    REQUIRE(mutex.checked_lock().ok());
    CHECK(mutex.close() == osal::error_code::would_block);
    CHECK(mutex.valid());
    REQUIRE(mutex.checked_unlock().ok());
    REQUIRE(mutex.close().ok());
    CHECK(mutex.close().ok());
    osal::mutex::checked_lock_guard guard{mutex};
    CHECK_FALSE(guard.owns_lock());
    CHECK(guard.status() == osal::error_code::not_initialized);
    CHECK(guard.unlock() == osal::error_code::not_owner);
}

TEST_CASE("explicit mutex policies configure native attributes")
{
    osal::mutex inherited{{osal::mutex_type::normal, osal::mutex_protocol::priority_inheritance}};
    REQUIRE(inherited.initialization_status().ok());
    osal::mutex::checked_lock_guard guard{inherited};
    CHECK(guard.status().ok());
    CHECK(guard.owns_lock());

    osal::mutex bad_ceiling{
        {osal::mutex_type::normal, osal::mutex_protocol::priority_ceiling, osal::mutex_robustness::non_robust, -1}};
    CHECK(bad_ceiling.initialization_status() == osal::error_code::invalid_argument);
    CHECK_FALSE(bad_ceiling.valid());
}

#if defined(__linux__)
TEST_CASE("robust mutex reports acquired owner death and supports repair")
{
    osal::mutex mutex{{osal::mutex_type::normal, osal::mutex_protocol::none, osal::mutex_robustness::robust}};
    REQUIRE(mutex.initialization_status().ok());
    std::atomic<bool> acquired{false};
    std::thread       owner{[&] { acquired.store(mutex.checked_lock().ok()); }};
    owner.join();
    REQUIRE(acquired.load());
    osal::mutex::checked_lock_guard guard{mutex};
    REQUIRE(guard.status() == osal::error_code::owner_died);
    REQUIRE(guard.owns_lock());
    REQUIRE(mutex.consistent().ok());
    REQUIRE(guard.unlock().ok());
    CHECK(mutex.checked_lock(osal::milliseconds{0}).ok());
    REQUIRE(mutex.checked_unlock().ok());
    CHECK(mutex.close().ok());
}

TEST_CASE("unrepaired robust state becomes explicitly not recoverable")
{
    osal::mutex mutex{{osal::mutex_type::normal, osal::mutex_protocol::none, osal::mutex_robustness::robust}};
    REQUIRE(mutex.valid());
    std::atomic<bool> acquired{false};
    std::thread       owner{[&] { acquired.store(mutex.checked_lock().ok()); }};
    owner.join();
    REQUIRE(acquired.load());
    {
        osal::mutex::checked_lock_guard guard{mutex};
        REQUIRE(guard.status() == osal::error_code::owner_died);
        REQUIRE(guard.owns_lock());
    }
    CHECK(mutex.checked_lock() == osal::error_code::not_recoverable);
}
#endif

TEST_CASE("C mutex policy descriptor preserves legacy configuration")
{
    osal_mutex_handle              handle{};
    const osal_mutex_policy_config config{0, OSAL_MUTEX_PRIORITY_INHERITANCE, OSAL_MUTEX_NON_ROBUST, 0};
    REQUIRE(osal_c_mutex_create_with_policy(&handle, &config) == OSAL_OK);
    CHECK(osal_c_mutex_create_with_policy(&handle, &config) == OSAL_ALREADY_EXISTS);
    REQUIRE(osal_c_mutex_lock(&handle, OSAL_NO_WAIT) == OSAL_OK);
    CHECK(osal_c_mutex_destroy(&handle) == OSAL_WOULD_BLOCK);
    REQUIRE(handle.native != nullptr);
    REQUIRE(osal_c_mutex_unlock(&handle) == OSAL_OK);
    REQUIRE(osal_c_mutex_destroy(&handle) == OSAL_OK);
    static_assert(OSAL_OWNER_DIED == static_cast<int>(osal::error_code::owner_died));
    static_assert(OSAL_NOT_RECOVERABLE == static_cast<int>(osal::error_code::not_recoverable));
    static_assert(OSAL_CANCELLED == static_cast<int>(osal::error_code::cancelled));
}
