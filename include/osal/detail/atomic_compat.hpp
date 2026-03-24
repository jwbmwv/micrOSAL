// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(OSAL_BACKEND_NUTTX) && defined(__GNUC__)
#include <bits/c++config.h>

#pragma push_macro("_GLIBCXX_HOSTED")
#undef _GLIBCXX_HOSTED
#define _GLIBCXX_HOSTED 0
#include <atomic>
#pragma pop_macro("_GLIBCXX_HOSTED")
#else
#include <atomic>
#endif
