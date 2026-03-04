/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file c_api_c_check.c
 * @brief Compilation check — proves osal_c.h parses as valid C11.
 *
 * This file is compiled as C (not C++).  If it compiles, the header is
 * C-compatible.  It also provides a smoke-test function called from the
 * C++ doctest harness.
 */
#include <osal/osal_c.h>
#include <stddef.h>

/* Verify a few sizeof / constant assumptions at compile time. */
typedef char static_assert_handle_size[sizeof(osal_mutex_handle) == sizeof(void*) ? 1 : -1];
#if defined(OSAL_CFG_TICK_TYPE_U64)
typedef char static_assert_tick_size[sizeof(osal_tick_t) == 8 ? 1 : -1];
#elif defined(OSAL_CFG_TICK_TYPE_U16)
typedef char static_assert_tick_size[sizeof(osal_tick_t) == 2 ? 1 : -1];
#else
typedef char static_assert_tick_size[sizeof(osal_tick_t) == 4 ? 1 : -1];
#endif

/**
 * osal_c_smoke_test — exercises basic C-API calls and returns 0 on success.
 *
 * Called from test_c_api.cpp (doctest) to validate runtime behaviour.
 */
int osal_c_smoke_test(void)
{
    /* ---- Clock ---- */
    int64_t  mono = osal_c_clock_monotonic_ms();
    (void)mono;
    osal_tick_t ticks = osal_c_clock_ticks();
    (void)ticks;

    /* ---- Mutex ---- */
    osal_mutex_handle mtx;
    osal_result_t rc = osal_c_mutex_create(&mtx, 0);
    if (rc != OSAL_OK) return 1;

    rc = osal_c_mutex_lock(&mtx, OSAL_WAIT_FOREVER);
    if (rc != OSAL_OK) return 2;

    rc = osal_c_mutex_unlock(&mtx);
    if (rc != OSAL_OK) return 3;

    rc = osal_c_mutex_destroy(&mtx);
    if (rc != OSAL_OK) return 4;

    /* ---- Semaphore ---- */
    osal_semaphore_handle sem;
    rc = osal_c_semaphore_create(&sem, 1, 10);
    if (rc != OSAL_OK) return 10;

    rc = osal_c_semaphore_take(&sem, OSAL_NO_WAIT);
    if (rc != OSAL_OK) return 11;

    rc = osal_c_semaphore_give(&sem);
    if (rc != OSAL_OK) return 12;

    rc = osal_c_semaphore_destroy(&sem);
    if (rc != OSAL_OK) return 13;

    /* ---- Queue ---- */
    osal_queue_handle q;
    int queue_buf[4]; /* backing storage for 4 ints */
    rc = osal_c_queue_create(&q, queue_buf, sizeof(int), 4);
    if (rc != OSAL_OK) return 20;

    int val = 42;
    rc = osal_c_queue_send(&q, &val, OSAL_WAIT_FOREVER);
    if (rc != OSAL_OK) return 21;

    if (osal_c_queue_count(&q) != 1) return 22;

    int out = 0;
    rc = osal_c_queue_receive(&q, &out, OSAL_WAIT_FOREVER);
    if (rc != OSAL_OK) return 23;
    if (out != 42) return 24;

    rc = osal_c_queue_destroy(&q);
    if (rc != OSAL_OK) return 25;

    /* ---- Event Flags ---- */
    osal_event_flags_handle ef;
    rc = osal_c_event_flags_create(&ef);
    if (rc != OSAL_OK) return 30;

    rc = osal_c_event_flags_set(&ef, 0x05);
    if (rc != OSAL_OK) return 31;

    osal_event_bits_t bits = osal_c_event_flags_get(&ef);
    if ((bits & 0x05) != 0x05) return 32;

    rc = osal_c_event_flags_clear(&ef, 0x01);
    if (rc != OSAL_OK) return 33;

    rc = osal_c_event_flags_destroy(&ef);
    if (rc != OSAL_OK) return 34;

    /* ---- Condition Variable ---- */
    osal_condvar_handle cv;
    rc = osal_c_condvar_create(&cv);
    if (rc != OSAL_OK) return 40;

    /* notify on empty waiters should still succeed */
    rc = osal_c_condvar_notify_one(&cv);
    if (rc != OSAL_OK) return 41;

    rc = osal_c_condvar_destroy(&cv);
    if (rc != OSAL_OK) return 42;

    /* ---- Read-Write Lock ---- */
    osal_rwlock_handle rw;
    rc = osal_c_rwlock_create(&rw);
    if (rc != OSAL_OK) return 50;

    rc = osal_c_rwlock_read_lock(&rw, OSAL_WAIT_FOREVER);
    if (rc != OSAL_OK) return 51;

    rc = osal_c_rwlock_read_unlock(&rw);
    if (rc != OSAL_OK) return 52;

    rc = osal_c_rwlock_write_lock(&rw, OSAL_WAIT_FOREVER);
    if (rc != OSAL_OK) return 53;

    rc = osal_c_rwlock_write_unlock(&rw);
    if (rc != OSAL_OK) return 54;

    rc = osal_c_rwlock_destroy(&rw);
    if (rc != OSAL_OK) return 55;

    /* ---- Thread-Local Data ---- */
    {
        osal_tls_key_handle tls;
        rc = osal_c_tls_key_create(&tls);
        if (rc == OSAL_NOT_SUPPORTED) return 0;
        if (rc != OSAL_OK) return 56;

        int tls_value = 123;
        rc = osal_c_tls_set(&tls, &tls_value);
        if (rc != OSAL_OK) return 57;
        if (osal_c_tls_get(&tls) != &tls_value) return 58;

        rc = osal_c_tls_key_destroy(&tls);
        if (rc != OSAL_OK) return 59;
        if (osal_c_tls_get(&tls) != NULL) return 66;
    }

    /* ---- Config-based creation (FLASH-friendly) ---- */
    {
        const osal_mutex_config mcfg = {0};
        osal_mutex_handle cmtx;
        rc = osal_c_mutex_create_with_cfg(&cmtx, &mcfg);
        if (rc != OSAL_OK) return 60;
        rc = osal_c_mutex_destroy(&cmtx);
        if (rc != OSAL_OK) return 61;
    }
    {
        const osal_semaphore_config scfg = {1, 5};
        osal_semaphore_handle csem;
        rc = osal_c_semaphore_create_with_cfg(&csem, &scfg);
        if (rc != OSAL_OK) return 62;
        rc = osal_c_semaphore_destroy(&csem);
        if (rc != OSAL_OK) return 63;
    }
    {
        int qbuf[2];
        const osal_queue_config qcfg = {qbuf, sizeof(int), 2};
        osal_queue_handle cq;
        rc = osal_c_queue_create_with_cfg(&cq, &qcfg);
        if (rc != OSAL_OK) return 64;
        rc = osal_c_queue_destroy(&cq);
        if (rc != OSAL_OK) return 65;
    }

    /* ---- Stream Buffer ---- */
    {
        /* capacity=16, so backing storage must be 17 bytes */
        unsigned char sb_storage[17];
        osal_stream_buffer_handle sb;
        rc = osal_c_stream_buffer_create(&sb, sb_storage, 16, 1);
        if (rc != OSAL_OK) return 70;

        unsigned char tx[4] = {0x01, 0x02, 0x03, 0x04};
        rc = osal_c_stream_buffer_send(&sb, tx, sizeof(tx), OSAL_NO_WAIT);
        if (rc != OSAL_OK) return 71;

        if (osal_c_stream_buffer_available(&sb) != 4) return 72;

        unsigned char rx[4] = {0};
        size_t n = osal_c_stream_buffer_receive(&sb, rx, sizeof(rx), OSAL_NO_WAIT);
        if (n != 4) return 73;
        if (rx[0] != 0x01 || rx[3] != 0x04) return 74;

        rc = osal_c_stream_buffer_reset(&sb);
        if (rc != OSAL_OK) return 75;

        if (osal_c_stream_buffer_available(&sb) != 0) return 76;

        rc = osal_c_stream_buffer_destroy(&sb);
        if (rc != OSAL_OK) return 77;
    }

    /* ---- Message Buffer ---- */
    {
        /* capacity=32, backing storage must be 33 bytes */
        unsigned char mb_storage[33];
        osal_message_buffer_handle mb;
        rc = osal_c_message_buffer_create(&mb, mb_storage, 32);
        if (rc != OSAL_OK) return 80;

        unsigned char msg[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        rc = osal_c_message_buffer_send(&mb, msg, sizeof(msg), OSAL_NO_WAIT);
        if (rc != OSAL_OK) return 81;

        /* next_message_size should equal payload length */
        if (osal_c_message_buffer_available(&mb) != sizeof(msg)) return 82;

        unsigned char rxm[4] = {0};
        size_t n = osal_c_message_buffer_receive(&mb, rxm, sizeof(rxm), OSAL_NO_WAIT);
        if (n != sizeof(msg)) return 83;
        if (rxm[0] != 0xAA || rxm[3] != 0xDD) return 84;

        rc = osal_c_message_buffer_reset(&mb);
        if (rc != OSAL_OK) return 85;

        rc = osal_c_message_buffer_destroy(&mb);
        if (rc != OSAL_OK) return 86;
    }

    return 0; /* all good */
}
