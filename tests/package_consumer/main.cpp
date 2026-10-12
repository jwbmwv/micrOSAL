// SPDX-License-Identifier: Apache-2.0
#include <osal/mutex.hpp>

int main()
{
    osal::mutex mutex;
    return mutex.valid() ? 0 : 1;
}
