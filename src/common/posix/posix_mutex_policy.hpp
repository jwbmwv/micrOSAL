// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <osal/mutex.hpp>
#include <pthread.h>
#include <cerrno>
#include <new>

namespace osal::detail
{
inline result pthread_mutex_status(int status) noexcept
{
    switch (status)
    {
    case 0:
        return ok();
    case EBUSY:
        return error_code::would_block;
    case ETIMEDOUT:
        return error_code::timeout;
    case EINVAL:
        return error_code::invalid_argument;
    case EPERM:
        return error_code::permission_denied;
    case ENOMEM:
    case EAGAIN:
        return error_code::out_of_resources;
    case ENOTSUP:
        return error_code::not_supported;
    case EDEADLK:
        return error_code::deadlock_detected;
#ifdef EOWNERDEAD
    case EOWNERDEAD:
        return error_code::owner_died;
    case ENOTRECOVERABLE:
        return error_code::not_recoverable;
#endif
    default:
        return error_code::unknown;
    }
}

inline result create_pthread_mutex(active_traits::mutex_handle_t* handle, const mutex_config& config) noexcept
{
    if (handle == nullptr)
    {
        return error_code::invalid_argument;
    }
    if (handle->native != nullptr)
    {
        return error_code::already_exists;
    }
    pthread_mutexattr_t attr{};
    int                 status = pthread_mutexattr_init(&attr);
    if (status != 0)
    {
        return pthread_mutex_status(status);
    }
    status = pthread_mutexattr_settype(&attr, config.type == mutex_type::recursive ? PTHREAD_MUTEX_RECURSIVE
                                                                                   : PTHREAD_MUTEX_ERRORCHECK);
    if (status == 0 && config.protocol != mutex_protocol::native_default)
    {
        int protocol = PTHREAD_PRIO_NONE;
        if (config.protocol == mutex_protocol::priority_inheritance)
        {
            protocol = PTHREAD_PRIO_INHERIT;
        }
        else if (config.protocol == mutex_protocol::priority_ceiling)
        {
            protocol = PTHREAD_PRIO_PROTECT;
        }
        status = pthread_mutexattr_setprotocol(&attr, protocol);
        if (status == 0 && config.protocol == mutex_protocol::priority_ceiling)
        {
            status = pthread_mutexattr_setprioceiling(&attr, config.priority_ceiling);
        }
    }
    if (status == 0 && config.robustness == mutex_robustness::robust)
    {
#if defined(__linux__)
        status = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
#else
        status = ENOTSUP;
#endif
    }
    auto* mutex = status == 0 ? new (std::nothrow) pthread_mutex_t : nullptr;
    if (status == 0)
    {
        status = mutex == nullptr ? ENOMEM : pthread_mutex_init(mutex, &attr);
    }
    const int cleanup_status = pthread_mutexattr_destroy(&attr);
    if (status != 0)
    {
        delete mutex;
        return pthread_mutex_status(status);
    }
    if (cleanup_status != 0)
    {
        (void)pthread_mutex_destroy(mutex);
        delete mutex;
        return pthread_mutex_status(cleanup_status);
    }
    handle->native = mutex;
    return ok();
}
}  // namespace osal::detail
