// SPDX-License-Identifier: Apache-2.0
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#include "../../portable_feature_checks.hpp"

namespace
{
K_THREAD_STACK_DEFINE(wake_stack, 4096);

void check_feature(bool condition, const char* description)
{
    zexpect_true(condition, "%s", description);
}
}  // namespace

ZTEST(osal_portable_features, test_owned_zero_copy_loans)
{
    osal_test::verify_owned_loans(check_feature);
}

ZTEST(osal_portable_features, test_typed_topic_routing)
{
    osal_test::verify_typed_routing(check_feature);
}

ZTEST(osal_portable_features, test_checked_cancellation_lifecycle)
{
    osal_test::verify_checked_cancellation(check_feature);
}

ZTEST(osal_portable_features, test_event_driven_readiness)
{
    osal_test::verify_task_wakeup(check_feature, false, wake_stack, K_THREAD_STACK_SIZEOF(wake_stack));
}

ZTEST(osal_portable_features, test_event_driven_cancellation)
{
    osal_test::verify_task_wakeup(check_feature, true, wake_stack, K_THREAD_STACK_SIZEOF(wake_stack));
}

ZTEST(osal_portable_features, test_mutex_policy_and_diagnostics)
{
    osal_test::verify_policy_and_diagnostics(check_feature);
}

ZTEST(osal_portable_features, test_multi_object_waiting)
{
    osal_test::verify_object_waiting(check_feature);
}

ZTEST_SUITE(osal_portable_features, nullptr, nullptr, nullptr, nullptr, nullptr);
