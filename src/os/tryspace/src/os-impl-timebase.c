/************************************************************************
 * NASA Docket No. GSC-18,719-1, and identified as “core Flight System: Bootes”
 *
 * Copyright (c) 2020 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ************************************************************************/

/**
 * \file
 * \ingroup  posix
 * \author   joseph.p.hickey@nasa.gov
 *
 * This file contains the OSAL Timebase API for POSIX systems.
 *
 * This implementation depends on the POSIX Timer API which may not be available
 * in older versions of the Linux kernel. It was developed and tested on
 * RHEL 5 ./ CentOS 5 with Linux kernel 2.6.18
 */

/****************************************************************************************
                                    INCLUDE FILES
 ***************************************************************************************/

#include "os-posix.h"
#include "os-impl-timebase.h"
#include "os-impl-tasks.h"

#include "os-shared-timebase.h"
#include "os-shared-idmap.h"
#include "os-shared-common.h"
#include "simulith.h"
#include <unistd.h>
#include <stdbool.h>

/****************************************************************************************
                                EXTERNAL FUNCTION PROTOTYPES
 ***************************************************************************************/

/****************************************************************************************
                                INTERNAL FUNCTION PROTOTYPES
 ***************************************************************************************/

/****************************************************************************************
                                     DEFINES
 ***************************************************************************************/

/****************************************************************************************
                                     GLOBALS
 ***************************************************************************************/

OS_impl_timebase_internal_record_t OS_impl_timebase_table[OS_MAX_TIMEBASES];

/*
 * Global flag to track if simulith client has been initialized
 * Only initialize it once for the entire process
 */
static uint8_t simulith_client_initialized = 0;

/*
 * Reference count for active timebases using simulith
 * When this reaches zero, we can shutdown the simulith client
 */
static int simulith_timebase_count = 0;

/*
 * Shared tick distribution mechanism
 * Since Simulith expects only one client to wait for ticks, we need
 * a single master receiver that distributes ticks to all timebases
 * 
 * These are exported for use by OS_TaskDelay_Impl
 */
static pthread_t tick_distribution_thread;
static uint64_t latest_tick_time_ns = 0;
static uint64_t previous_tick_time_ns = 0;
pthread_mutex_t tick_mutex;
pthread_cond_t tick_condition;
volatile bool tick_thread_running = false;
volatile uint64_t tick_generation = 0;

/*
 * Master tick receiver thread
 * This thread receives ticks from Simulith and distributes them to all timebases
 */
static void* OS_SimulithTickDistributionThread(void* arg)
{
    uint64_t tick_time_ns;
    uint32_t tick_count = 0;
        
    while (tick_thread_running)
    {
        if (simulith_client_wait_for_tick(&tick_time_ns) == 0)
        {
            tick_count++;
            //if (tick_count % 1000 == 0)  /* Log every 1000 ticks (10 seconds) */
            //{
            //    OS_DEBUG("Tick distribution: received tick %u, time_ns: %lu\n", 
            //             tick_count, (unsigned long)tick_time_ns);
            //}
            
            pthread_mutex_lock(&tick_mutex);
            previous_tick_time_ns = latest_tick_time_ns;
            latest_tick_time_ns = tick_time_ns;
            tick_generation++;
            pthread_cond_broadcast(&tick_condition);
            pthread_mutex_unlock(&tick_mutex);
        }
        else
        {
            OS_DEBUG("simulith_client_wait_for_tick() failed\n");
            /* If tick wait fails, sleep briefly to avoid spinning */
            usleep(1000);
        }
    }
    return NULL;
}

/*----------------------------------------------------------------
 *
 *  Purpose: Implemented per internal OSAL API
 *           See prototype for argument/return detail
 *
 *-----------------------------------------------------------------*/
void OS_TimeBaseLock_Impl(const OS_object_token_t *token)
{
    OS_impl_timebase_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_timebase_table, *token);

    pthread_mutex_lock(&impl->handler_mutex);
}

/*----------------------------------------------------------------
 *
 *  Purpose: Implemented per internal OSAL API
 *           See prototype for argument/return detail
 *
 *-----------------------------------------------------------------*/
void OS_TimeBaseUnlock_Impl(const OS_object_token_t *token)
{
    OS_impl_timebase_internal_record_t *impl;

    impl = OS_OBJECT_TABLE_GET(OS_impl_timebase_table, *token);

    pthread_mutex_unlock(&impl->handler_mutex);
}

/*----------------------------------------------------------------
 *
 *  Purpose: Local helper routine, not part of OSAL API.
 *
 *-----------------------------------------------------------------*/
static uint32 OS_TimeBase_SimulithWaitImpl(osal_id_t obj_id)
{
    OS_object_token_t                   token;
    OS_impl_timebase_internal_record_t *impl;
    OS_timebase_internal_record_t *     timebase;
    uint32                              interval_time;
    static __thread uint64_t            last_tick_generation = 0;

    interval_time = 0;

    if (OS_ObjectIdGetById(OS_LOCK_MODE_NONE, OS_OBJECT_TYPE_OS_TIMEBASE, obj_id, &token) == OS_SUCCESS)
    {
        impl     = OS_OBJECT_TABLE_GET(OS_impl_timebase_table, token);
        timebase = OS_OBJECT_TABLE_GET(OS_timebase_table, token);

        /* Wait for the next tick from the shared distribution thread */
        pthread_mutex_lock(&tick_mutex);
        
        /* Make sure the tick distribution thread is running */
        if (!tick_thread_running)
        {
            OS_DEBUG("Tick distribution thread not running\n");
            pthread_mutex_unlock(&tick_mutex);
            return 0;
        }
        
        /* Wait for a new tick using generation counter to avoid missing ticks */
        while (tick_generation == last_tick_generation)
        {
            pthread_cond_wait(&tick_condition, &tick_mutex);
        }
        last_tick_generation = tick_generation;
        
        pthread_mutex_unlock(&tick_mutex);

        if (impl->reset_flag == 0)
        {
            /*
             * Normal steady-state behavior.
             * interval_time reflects the configured interval time.
             */
            interval_time = timebase->nominal_interval_time;
            
            /* If interval time is not set yet, use a default to avoid spin loop */
            if (interval_time == 0)
            {
                interval_time = 10000;  /* 10ms = 10000 microseconds */
            }
        }
        else
        {
            /*
             * Reset/First interval behavior.
             * timer_set() was invoked since the previous interval occurred (if any).
             * interval_time reflects the configured start time.
             */
            interval_time = timebase->nominal_start_time;
            
            /* If start time is not set yet, use a default to avoid spin loop */
            if (interval_time == 0)
            {
                interval_time = 10000;  /* 10ms = 10000 microseconds */
            }
            
            impl->reset_flag = 0;
        }
        
        OS_ObjectIdRelease(&token);
    }
    else
    {
        OS_DEBUG("Failed to get timebase object by ID\n");
    }

    return interval_time;
}

/****************************************************************************************
                                INITIALIZATION FUNCTION
 ***************************************************************************************/

/******************************************************************************
 *
 *  Purpose:  Initialize the timer implementation layer
 *
 *  Arguments:
 *
 *  Return:
 */
int32 OS_Posix_TimeBaseAPI_Impl_Init(void)
{
    int                 status;
    osal_index_t        idx;
    pthread_mutexattr_t mutex_attr;
    int32               return_code;

    return_code = OS_SUCCESS;

    do
    {
        /*
        ** Mark all timers as available
        */
        memset(OS_impl_timebase_table, 0, sizeof(OS_impl_timebase_table));

        /*
        ** For simulith time, we use a fixed resolution of INTERVAL_NS (currently 10ms)
        ** This is much more deterministic than POSIX clock resolution
        */
        POSIX_GlobalVars.ClockAccuracyNsec = INTERVAL_NS; /* Use simulith interval directly */

        /*
        ** initialize the attribute with default values
        */
        status = pthread_mutexattr_init(&mutex_attr);
        if (status != 0)
        {
            OS_DEBUG("Error: pthread_mutexattr_init failed: %s\n", strerror(status));
            return_code = OS_ERROR;
            break;
        }

        /*
        ** Allow the mutex to use priority inheritance
        */
        status = pthread_mutexattr_setprotocol(&mutex_attr, PTHREAD_PRIO_INHERIT);
        if (status != 0)
        {
            OS_DEBUG("Error: pthread_mutexattr_setprotocol failed: %s\n", strerror(status));
            return_code = OS_ERROR;
            break;
        }

        for (idx = 0; idx < OS_MAX_TIMEBASES; ++idx)
        {
            /*
            ** create the timebase sync mutex
            ** This gives a mechanism to synchronize updates to the timer chain with the
            ** expiration of the timer and processing the chain.
            */
            status = pthread_mutex_init(&OS_impl_timebase_table[idx].handler_mutex, &mutex_attr);
            if (status != 0)
            {
                OS_DEBUG("Error: Mutex could not be created: %s\n", strerror(status));
                return_code = OS_ERROR;
                break;
            }
        }

        /*
         * For simulith time, we simulate the tick rate based on INTERVAL_NS
         * This gives us 100 ticks per second (10ms intervals)
         * Use explicit values to avoid macro expansion issues
         */
        OS_SharedGlobalVars.TicksPerSecond = 1000000000UL / (10UL * 1000000UL);
        
        /*
         * Set microseconds per tick based on INTERVAL_NS
         * Convert nanoseconds to microseconds: 10ms = 10000 microseconds
         */
        OS_SharedGlobalVars.MicroSecPerTick = (10UL * 1000000UL) / 1000UL;

        /*
         * Initialize simulith client if not already done
         * This must be done once per process before any simulith_time_init() calls
         */
        if (simulith_client_initialized == 0)
        {
            status = simulith_client_init(LOCAL_PUB_ADDR, LOCAL_REP_ADDR, "tryspace-fsw", INTERVAL_NS);
            if (status != 0)
            {
                OS_DEBUG("Error: simulith_client_init failed: %d\n", status);
                return_code = OS_ERROR;
                break;
            }

            status = simulith_client_handshake();
            if (status != 0)
            {
                OS_DEBUG("Error: simulith_client_handshake failed: %d\n", status);
                simulith_client_shutdown();
                return_code = OS_ERROR;
                break;
            }

            /* Initialize tick distribution synchronization */
            status = pthread_mutex_init(&tick_mutex, NULL);
            if (status != 0)
            {
                OS_DEBUG("Error: pthread_mutex_init failed: %s\n", strerror(status));
                simulith_client_shutdown();
                return_code = OS_ERROR;
                break;
            }

            status = pthread_cond_init(&tick_condition, NULL);
            if (status != 0)
            {
                OS_DEBUG("Error: pthread_cond_init failed: %s\n", strerror(status));
                pthread_mutex_destroy(&tick_mutex);
                simulith_client_shutdown();
                return_code = OS_ERROR;
                break;
            }

            /* Start the tick distribution thread */
            tick_thread_running = true;
            status = pthread_create(&tick_distribution_thread, NULL, OS_SimulithTickDistributionThread, NULL);
            if (status != 0)
            {
                OS_DEBUG("Error: pthread_create for tick distribution failed: %s\n", strerror(status));
                tick_thread_running = false;
                pthread_cond_destroy(&tick_condition);
                pthread_mutex_destroy(&tick_mutex);
                simulith_client_shutdown();
                return_code = OS_ERROR;
                break;
            }

            simulith_client_initialized = 1;
            OS_DEBUG("Simulith client initialized successfully with tick distribution thread\n");
        }
    } while (0);

    return return_code;
}

/****************************************************************************************
                                   Time Base API
 ***************************************************************************************/

static void *OS_TimeBasePthreadEntry(void *arg)
{
    OS_VoidPtrValueWrapper_t local_arg;

    /* cppcheck-suppress unreadVariable // intentional use of other union member */
    local_arg.opaque_arg = arg;
    OS_TimeBase_CallbackThread(local_arg.id);

    return NULL;
}

/*----------------------------------------------------------------
 *
 *  Purpose: Implemented per internal OSAL API
 *           See prototype for argument/return detail
 *
 *-----------------------------------------------------------------*/
int32 OS_TimeBaseCreate_Impl(const OS_object_token_t *token)
{
    int32                               return_code;
    OS_impl_timebase_internal_record_t *local;
    OS_timebase_internal_record_t *     timebase;
    OS_VoidPtrValueWrapper_t            arg;

    local    = OS_OBJECT_TABLE_GET(OS_impl_timebase_table, *token);
    timebase = OS_OBJECT_TABLE_GET(OS_timebase_table, *token);

    /*
     * Spawn a dedicated time base handler thread
     *
     * This alleviates the need to handle expiration in the context of a signal handler -
     * The handler thread can call simulith synchronized delay implementation as well as the
     * application callback function.  It should run with elevated priority to reduce latency.
     *
     * Note the thread will not actually start running until this function exits and releases
     * the global table lock.
     */
    memset(&arg, 0, sizeof(arg));

    /* cppcheck-suppress unreadVariable // intentional use of other union member */
    arg.id      = OS_ObjectIdFromToken(token);
    return_code = OS_Posix_InternalTaskCreate_Impl(&local->handler_thread, OSAL_PRIORITY_C(0), OSAL_TASK_STACK_ALLOCATE,
                                                   PTHREAD_STACK_MIN, OS_TimeBasePthreadEntry, arg.opaque_arg);
    if (return_code != OS_SUCCESS)
    {
        return return_code;
    }

    /*
     * Use simulith client for tick synchronization
     * 
     * This replaces the separate simulith_time_init() approach with 
     * the shared client connection that properly participates in the tick protocol
     */
    if (timebase->external_sync == NULL)
    {
        /* Set the external sync function to use simulith client ticks */
        timebase->external_sync = OS_TimeBase_SimulithWaitImpl;
        
        /* Increment reference count for simulith timebases */
        simulith_timebase_count++;
        OS_DEBUG("Simulith timebase created, count now: %d\n", simulith_timebase_count);
    }

    return return_code;
}

/*----------------------------------------------------------------
 *
 *  Purpose: Implemented per internal OSAL API
 *           See prototype for argument/return detail
 *
 *-----------------------------------------------------------------*/
int32 OS_TimeBaseSet_Impl(const OS_object_token_t *token, uint32 start_time, uint32 interval_time)
{
    OS_impl_timebase_internal_record_t *local;
    int32                               return_code;
    OS_timebase_internal_record_t *     timebase;

    local       = OS_OBJECT_TABLE_GET(OS_impl_timebase_table, *token);
    timebase    = OS_OBJECT_TABLE_GET(OS_timebase_table, *token);
    return_code = OS_SUCCESS;

    /*
     * For simulith time, we don't need to program hardware timers.
     * The timing is controlled by the simulith time provider.
     * We just set the accuracy based on the INTERVAL_NS interval.
     */
    if (interval_time > 0)
    {
        /* Use the requested interval, but note simulith runs at 10ms ticks */
        timebase->accuracy_usec = interval_time;
    }
    else
    {
        /* One-shot timer uses start time */
        timebase->accuracy_usec = start_time;
    }

    local->reset_flag = (return_code == OS_SUCCESS);
    return return_code;
}

/*----------------------------------------------------------------
 *
 *  Purpose: Implemented per internal OSAL API
 *           See prototype for argument/return detail
 *
 *-----------------------------------------------------------------*/
int32 OS_TimeBaseDelete_Impl(const OS_object_token_t *token)
{
    OS_impl_timebase_internal_record_t *local;

    local = OS_OBJECT_TABLE_GET(OS_impl_timebase_table, *token);

    pthread_cancel(local->handler_thread);

    /*
    ** Clean up simulith timebase reference
    */
    /* Decrement reference count */
    simulith_timebase_count--;
    OS_DEBUG("Simulith timebase deleted, count now: %d\n", simulith_timebase_count);
    
        /* If this was the last timebase, shutdown the simulith client */
        if (simulith_timebase_count <= 0 && simulith_client_initialized == 1)
        {
            /* Stop the tick distribution thread */
            tick_thread_running = false;
            pthread_cancel(tick_distribution_thread);
            pthread_join(tick_distribution_thread, NULL);
            
            /* Clean up synchronization objects */
            pthread_cond_destroy(&tick_condition);
            pthread_mutex_destroy(&tick_mutex);
            
            simulith_client_shutdown();
            simulith_client_initialized = 0;
            simulith_timebase_count = 0; /* Ensure it doesn't go negative */
            OS_DEBUG("Simulith client shutdown - all timebases deleted\n");
        }    return OS_SUCCESS;
}

/*----------------------------------------------------------------
 *
 *  Purpose: Implemented per internal OSAL API
 *           See prototype for argument/return detail
 *
 *-----------------------------------------------------------------*/
int32 OS_TimeBaseGetInfo_Impl(const OS_object_token_t *token, OS_timebase_prop_t *timer_prop)
{
    return OS_SUCCESS;
}
