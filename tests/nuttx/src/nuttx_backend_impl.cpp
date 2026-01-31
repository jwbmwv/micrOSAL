// SPDX-License-Identifier: Apache-2.0
// This file is a thin shim that compiles the micrOSAL NuttX backend.
// It ensures nuttx_backend.cpp is part of the NuttX apps build even though it
// lives outside the nuttx-apps tree.
// (NuttX's nuttx_add_application uses file(GLOB) to check for SRCS existence,
//  which only finds files within glob-reachable paths; absolute paths outside
//  the tree need this workaround.)
//
// Header-order shim: the NuttX sim build adds -isystem<nuttx>/include BEFORE
// the GCC C++ headers, but C++ wrappers like <algorithm>/<cstdlib> use
// #include_next which skips past the GCC include dir and finds glibc's
// stdlib.h instead of NuttX's.  We pre-include NuttX's stdlib.h here and
// then set glibc's guard macro so the glibc copy is elided.
#include <stdlib.h>          // NuttX's stdlib.h (found first in -isystem order)
#define _STDLIB_H 1          // Block glibc's /usr/include/stdlib.h
#include <string.h>          // NuttX's string.h (avoids <cstring> pulling glibc's)
#define _STRING_H 1          // Block glibc's /usr/include/string.h (same pattern)

#include "../../../src/nuttx/nuttx_backend.cpp"
