// SPDX-License-Identifier: Apache-2.0
/// @file posix_bounded_semaphore.inl
/// @brief Internal bounded sem_t storage for hosted backends.
#pragma once

struct posix_bounded_semaphore
{
    sem_t                 sem;
    std::atomic<unsigned> count;
    unsigned              maximum;
};

static_assert(std::atomic<unsigned>::is_always_lock_free);

static int bounded_sem_complete(posix_bounded_semaphore* s, int rc) noexcept
{
    if (rc == 0)
    {
        s->count.fetch_sub(1U, std::memory_order_acq_rel);
    }
    return rc;
}

static osal::result bounded_sem_post(posix_bounded_semaphore* s) noexcept
{
    unsigned count = s->count.load(std::memory_order_acquire);
    do
    {
        if (count >= s->maximum)
        {
            return osal::error_code::overflow;
        }
    } while (!s->count.compare_exchange_weak(count, count + 1U, std::memory_order_acq_rel));
    if (sem_post(&s->sem) == 0)
    {
        return osal::ok();
    }
    s->count.fetch_sub(1U, std::memory_order_acq_rel);
    return errno == EOVERFLOW ? osal::error_code::overflow : osal::error_code::unknown;
}
