// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>
#include <FreeRTOS.h>

#include "../../portable_feature_checks.hpp"

namespace
{
alignas(portBYTE_ALIGNMENT) StackType_t wake_stack[1024];

void check_feature(bool condition, const char* description)
{
    CHECK_MESSAGE(condition, doctest::String{description});
}
}  // namespace

TEST_CASE("FreeRTOS portable features: owned zero-copy loans")
{
    osal_test::verify_owned_loans(check_feature);
}

TEST_CASE("FreeRTOS portable features: typed topic routing")
{
    osal_test::verify_typed_routing(check_feature);
}

TEST_CASE("FreeRTOS portable features: checked cancellation lifecycle")
{
    osal_test::verify_checked_cancellation(check_feature);
}

TEST_CASE("FreeRTOS portable features: event-driven readiness")
{
    osal_test::verify_task_wakeup(check_feature, false, wake_stack, sizeof(wake_stack));
}

TEST_CASE("FreeRTOS portable features: event-driven cancellation")
{
    osal_test::verify_task_wakeup(check_feature, true, wake_stack, sizeof(wake_stack));
}

TEST_CASE("FreeRTOS portable features: mutex policy and diagnostics")
{
    osal_test::verify_policy_and_diagnostics(check_feature);
}

TEST_CASE("FreeRTOS portable features: multi-object waiting")
{
    osal_test::verify_object_waiting(check_feature);
}
