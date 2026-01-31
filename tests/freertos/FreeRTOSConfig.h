// SPDX-License-Identifier: Apache-2.0
/// @file FreeRTOSConfig.h
/// @brief FreeRTOS kernel configuration for the GCC/POSIX simulation port.
///
/// This file is consumed by the FreeRTOS-Kernel v11 when building the
/// GCC_POSIX port on Linux.  It is tuned for a CI test environment:
/// maximum debugging aids enabled, generous stack and heap, all optional
/// primitives turned on so every MicrOSAL backend path is exercised.
///
/// Reference: FreeRTOS-Kernel/portable/ThirdParty/GCC/Posix/FreeRTOSConfig.h

#pragma once

// ---------------------------------------------------------------------------
// Hooks (all disabled — the test binary does not need them)
// ---------------------------------------------------------------------------
#define configUSE_IDLE_HOOK                        0
#define configUSE_TICK_HOOK                        0
#define configUSE_MALLOC_FAILED_HOOK               0
#define configUSE_DAEMON_TASK_STARTUP_HOOK         0
#define configCHECK_FOR_STACK_OVERFLOW             0

// ---------------------------------------------------------------------------
// Scheduler / kernel
// ---------------------------------------------------------------------------
#define configUSE_PREEMPTION                       1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION    0
#define configUSE_TICKLESS_IDLE                    0
#define configTICK_RATE_HZ                         ((TickType_t)1000)   // 1 ms tick
#define configMAX_PRIORITIES                       32
#define configMINIMAL_STACK_SIZE                   ((unsigned short)256)
#define configMAX_TASK_NAME_LEN                    24
#define configUSE_16_BIT_TICKS                     0
#define configIDLE_SHOULD_YIELD                    1
#define configUSE_TASK_NOTIFICATIONS               1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES      3
#define configUSE_MUTEXES                          1
#define configUSE_RECURSIVE_MUTEXES                1
#define configUSE_COUNTING_SEMAPHORES              1
#define configUSE_ALTERNATIVE_API                  0   // deprecated
#define configQUEUE_REGISTRY_SIZE                  20
#define configUSE_QUEUE_SETS                       0
#define configUSE_TIME_SLICING                     1
#define configUSE_NEWLIB_REENTRANT                 0
#define configENABLE_BACKWARD_COMPATIBILITY        0
#define configNUM_READER_WRITER_LOCKS              10
#define configSTACK_DEPTH_TYPE                     uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE           size_t
#define configHEAP_CLEAR_MEMORY_ON_FREE            1

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
#define configTOTAL_HEAP_SIZE                      ((size_t)(64 * 1024 * 1024))   // 64 MiB (POSIX process)
#define configSUPPORT_STATIC_ALLOCATION            1
#define configSUPPORT_DYNAMIC_ALLOCATION           1
#define configAPPLICATION_ALLOCATED_HEAP           0

// Thread-local storage (used by osal::thread_local_data)
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS    4

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------
#define configUSE_TIMERS                           1
#define configTIMER_TASK_PRIORITY                  (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                   20
#define configTIMER_TASK_STACK_DEPTH               (configMINIMAL_STACK_SIZE * 4)

// ---------------------------------------------------------------------------
// Event groups / stream buffers / message buffers
// ---------------------------------------------------------------------------
#define configUSE_EVENT_GROUPS                     1

// ---------------------------------------------------------------------------
// Task suspension (enables osal thread_suspend_resume)
// ---------------------------------------------------------------------------
#define INCLUDE_vTaskSuspend                       1

// ---------------------------------------------------------------------------
// Debugging and trace
// ---------------------------------------------------------------------------
#define configUSE_TRACE_FACILITY                   1
#define configUSE_STATS_FORMATTING_FUNCTIONS       1
#define configGENERATE_RUN_TIME_STATS              0
#define configASSERT(x)                            \
    if ((x) == 0)                                  \
    {                                              \
        taskDISABLE_INTERRUPTS();                  \
        for (;;)                                   \
            ;                                      \
    }

// ---------------------------------------------------------------------------
// POSIX simulation port specifics
// ---------------------------------------------------------------------------
// Maximum number of simulated interrupt handlers; this must be high enough to
// cover all signals (SIGRTMIN .. SIGRTMIN + configINTERRUPT_QUEUE_LENGTH - 1).
#define configINTERRUPT_QUEUE_LENGTH               10
// Keep signal handlers reasonably sized.
#define configPOSIX_STACK_SIZE                     ((StackType_t)(65536 / sizeof(StackType_t)))

// ---------------------------------------------------------------------------
// INCLUDE_* — optional API functions
// ---------------------------------------------------------------------------
#define INCLUDE_vTaskPrioritySet                   1
#define INCLUDE_uxTaskPriorityGet                  1
#define INCLUDE_vTaskDelete                        1
#define INCLUDE_vTaskCleanUpResources              0
#define INCLUDE_vTaskDelayUntil                    1
#define INCLUDE_vTaskDelay                         1
#define INCLUDE_xTaskGetSchedulerState             1
#define INCLUDE_xTaskGetCurrentTaskHandle          1
#define INCLUDE_uxTaskGetStackHighWaterMark        1
#define INCLUDE_uxTaskGetStackHighWaterMark2       1
#define INCLUDE_xTaskGetIdleTaskHandle             1
#define INCLUDE_eTaskGetState                      1
#define INCLUDE_xTimerPendFunctionCall             1
#define INCLUDE_xTaskAbortDelay                    1
#define INCLUDE_xTaskGetHandle                     1
#define INCLUDE_xQueueGetMutexHolder               1
#define INCLUDE_xSemaphoreGetMutexHolder           1
