// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <osal/thread.hpp>
#include <pthread.h>
#include <sched.h>
#include <cerrno>

namespace osal::detail
{
inline int configure_pthread_thread_attributes(pthread_attr_t* attr, priority_t priority, affinity_t affinity,
                                               void* stack, stack_size_t stack_bytes) noexcept
{
    if (stack != nullptr && stack_bytes == 0U)
    {
        return EINVAL;
    }
    int status = pthread_attr_setdetachstate(attr, PTHREAD_CREATE_JOINABLE);
    if (status == 0 && stack != nullptr)
    {
        status = pthread_attr_setstack(attr, stack, stack_bytes);
    }
    else if (status == 0 && stack_bytes != 0U)
    {
        status = pthread_attr_setstacksize(attr, stack_bytes);
    }
    if (status == 0 && priority != PRIORITY_NORMAL)
    {
        const int minimum = sched_get_priority_min(SCHED_FIFO);
        const int maximum = sched_get_priority_max(SCHED_FIFO);
        if (minimum < 0 || maximum < minimum)
        {
            return errno;
        }
        sched_param parameter{};
        parameter.sched_priority =
            minimum +
            static_cast<int>((static_cast<std::uint32_t>(priority) * static_cast<std::uint32_t>(maximum - minimum)) /
                             static_cast<std::uint32_t>(PRIORITY_HIGHEST));
        status = pthread_attr_setschedpolicy(attr, SCHED_FIFO);
        if (status == 0)
        {
            status = pthread_attr_setschedparam(attr, &parameter);
        }
        if (status == 0)
        {
            status = pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
        }
    }
    if (status == 0 && affinity != AFFINITY_ANY)
    {
#if defined(__linux__)
        cpu_set_t cpuset{};
        CPU_ZERO(&cpuset);
        for (unsigned cpu = 0U; cpu < 32U; ++cpu)
        {
            if ((affinity & (1U << cpu)) != 0U)
            {
                CPU_SET(cpu, &cpuset);
            }
        }
        status = pthread_attr_setaffinity_np(attr, sizeof(cpuset), &cpuset);
#else
        status = ENOTSUP;
#endif
    }
    return status;
}
}  // namespace osal::detail
