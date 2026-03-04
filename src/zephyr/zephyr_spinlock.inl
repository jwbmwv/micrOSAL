// SPDX-License-Identifier: Apache-2.0
/// @file zephyr_spinlock.inl
/// @brief Native Zephyr k_spinlock implementation.
/// @details Implements the five OSAL spinlock functions using Zephyr's
///          @c k_spinlock API.  A small static pool of @c zephyr_spinlock_obj
///          records (each holding a @c k_spinlock and the last-acquired
///          @c k_spinlock_key_t) is maintained so that osal_spinlock_create /
///          osal_spinlock_destroy follow the same pool pattern as the rest of
///          the Zephyr backend.
///
///          @note  The @c k_spinlock_key_t (interrupt-state save word) is
///                 stored inside the object because the OSAL spinlock API
///                 separates lock and unlock calls.  This is safe as long as
///                 a given spinlock object is only locked by one execution
///                 context at a time, which is the expected usage for a
///                 spinlock.
///
///          @note  @c osal_spinlock_try_lock falls back to a regular
///                 @c k_spin_lock (it will not contend but does not return
///                 immediately if another CPU holds the lock).  Zephyr does
///                 not expose a non-blocking try-lock at the k_spinlock level.
///
///          Prerequisites: @c <zephyr/kernel.h> included, extern "C" scope active.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.

// ---------------------------------------------------------------------------
// Spinlock (native — k_spinlock)
// ---------------------------------------------------------------------------

#ifndef OSAL_ZEPHYR_MAX_SPINLOCKS
/// @brief Maximum concurrent OSAL spinlock objects on Zephyr.
#define OSAL_ZEPHYR_MAX_SPINLOCKS 8U
#endif

namespace  // anonymous — internal to the including TU
{

struct zephyr_spinlock_obj
{
    struct k_spinlock lock;  ///< Zephyr spinlock (zero-init = unlocked).
    k_spinlock_key_t  key;   ///< ISR-state key from the most recent k_spin_lock.
};

static zephyr_spinlock_obj zep_sl_pool[OSAL_ZEPHYR_MAX_SPINLOCKS];
static bool                zep_sl_used[OSAL_ZEPHYR_MAX_SPINLOCKS];

static zephyr_spinlock_obj* zep_sl_acquire() noexcept
{
    for (std::size_t i = 0U; i < OSAL_ZEPHYR_MAX_SPINLOCKS; ++i)
    {
        if (!zep_sl_used[i])
        {
            zep_sl_used[i] = true;
            zep_sl_pool[i] = zephyr_spinlock_obj{};  // zero-init k_spinlock
            return &zep_sl_pool[i];
        }
    }
    return nullptr;
}

static void zep_sl_release(zephyr_spinlock_obj* p) noexcept
{
    for (std::size_t i = 0U; i < OSAL_ZEPHYR_MAX_SPINLOCKS; ++i)
    {
        if (&zep_sl_pool[i] == p)
        {
            zep_sl_used[i] = false;
            return;
        }
    }
}

}  // anonymous namespace

/// @brief Creates a Zephyr spinlock from the internal pool.
/// @param handle Output handle; populated on success.
/// @return @c osal::ok() on success; @c error_code::invalid_argument if null;
///         @c error_code::out_of_resources if the pool is exhausted.
osal::result osal_spinlock_create(osal::active_traits::spinlock_handle_t* handle) noexcept
{
    if (!handle)
    {
        return osal::error_code::invalid_argument;
    }
    zephyr_spinlock_obj* obj = zep_sl_acquire();
    if (!obj)
    {
        return osal::error_code::out_of_resources;
    }
    handle->native = obj;
    return osal::ok();
}

/// @brief Destroys a spinlock and returns its pool slot.
/// @param handle Handle to destroy; silently ignored if null.
/// @return @c osal::ok() always.
osal::result osal_spinlock_destroy(osal::active_traits::spinlock_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::ok();
    }
    zep_sl_release(static_cast<zephyr_spinlock_obj*>(handle->native));
    handle->native = nullptr;
    return osal::ok();
}

/// @brief Acquires the spinlock via @c k_spin_lock, saving the IRQ key.
/// @param handle Spinlock handle.
/// @return @c osal::ok() on success; @c error_code::not_initialized if null.
osal::result osal_spinlock_lock(osal::active_traits::spinlock_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return osal::error_code::not_initialized;
    }
    auto* obj = static_cast<zephyr_spinlock_obj*>(handle->native);
    obj->key  = k_spin_lock(&obj->lock);
    return osal::ok();
}

/// @brief Attempts to acquire the spinlock without contending.
/// @details Zephyr does not expose a non-blocking @c k_spin_trylock.  This
///          implementation calls @c k_spin_lock, which will busy-wait on SMP
///          if another CPU holds the lock.  On single-core Zephyr builds the
///          spinlock is always immediately acquired (interrupts are disabled).
/// @return @c true on success; @c false if the handle is null.
bool osal_spinlock_try_lock(osal::active_traits::spinlock_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return false;
    }
    auto* obj = static_cast<zephyr_spinlock_obj*>(handle->native);
    obj->key  = k_spin_lock(&obj->lock);
    return true;
}

/// @brief Releases the spinlock via @c k_spin_unlock, restoring the saved IRQ key.
/// @param handle Spinlock handle.
void osal_spinlock_unlock(osal::active_traits::spinlock_handle_t* handle) noexcept
{
    if (!handle || !handle->native)
    {
        return;
    }
    auto* obj = static_cast<zephyr_spinlock_obj*>(handle->native);
    k_spin_unlock(&obj->lock, obj->key);
}
