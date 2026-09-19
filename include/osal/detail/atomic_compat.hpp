// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(OSAL_BACKEND_NUTTX) && defined(__GNUC__)
#include <bits/c++config.h>
#include <version>

#pragma push_macro("_GLIBCXX_HOSTED")
#undef _GLIBCXX_HOSTED
#define _GLIBCXX_HOSTED 0
#undef __cpp_lib_atomic_wait
#include <atomic>
#pragma pop_macro("_GLIBCXX_HOSTED")
#else
#include <atomic>
#endif
