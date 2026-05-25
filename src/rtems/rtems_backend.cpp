// SPDX-License-Identifier: Apache-2.0
/// @file rtems_backend.cpp
/// @brief RTEMS implementation of all OSAL C-linkage functions
/// @details RTEMS uses the shared POSIX-family backend implementation and
///          therefore requires the pthread/semaphore/timer/poll profile used
///          by the generic POSIX backend.

#ifndef OSAL_BACKEND_RTEMS
#define OSAL_BACKEND_RTEMS
#endif

#define OSAL_POSIXLIKE_BACKEND_SELECTED 1
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../posix/posix_backend.cpp"
