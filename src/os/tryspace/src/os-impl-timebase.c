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
    int                                 ret;
    OS_object_token_t                   token;
    OS_impl_timebase_internal_record_t *impl;
    OS_timebase_internal_record_t *     timebase;
    uint32                              interval_time;

    interval_time = 0;

    if (OS_ObjectIdGetById(OS_LOCK_MODE_NONE, OS_OBJECT_TYPE_OS_TIMEBASE, obj_id, &token) == OS_SUCCESS)
    {
        impl     = OS_OBJECT_TABLE_GET(OS_impl_timebase_table, token);
        timebase = OS_OBJECT_TABLE_GET(OS_timebase_table, token);

        ret = simulith_time_wait_for_next_tick(impl->simulith_time_handle);

        if (ret != 0)
        {
            /*
             * the simulith_time_wait_for_next_tick call failed.
             * returning 0 will cause the process to repeat.
             */
        }
        else if (impl->reset_flag == 0)
        {
            /*
             * Normal steady-state behavior.
             * interval_time reflects the configured interval time.
             */
            interval_time = timebase->nominal_interval_time;
        }
        else
        {
            /*
             * Reset/First interval behavior.
             * timer_set() was invoked since the previous interval occurred (if any).
             * interval_time reflects the configured start time.
             */
            interval_time    = timebase->nominal_start_time;
            impl->reset_flag = 0;
        }
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
        ** For simulith time, we use a fixed resolution of INTERVAL_NS (10ms)
        ** This is much more deterministic than POSIX clock resolution
        */
        POSIX_GlobalVars.ClockAccuracyNsec = 10000000; /* 10ms in nanoseconds */

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
         */
        OS_SharedGlobalVars.TicksPerSecond = 100;
        
        /*
         * Calculate microseconds per tick: 10ms = 10000 microseconds
         */
        OS_SharedGlobalVars.MicroSecPerTick = 10000;
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
     * Initialize simulith time provider
     * 
     * This replaces the POSIX timer setup with simulith time provider initialization
     */
    if (timebase->external_sync == NULL)
    {
        local->simulith_time_handle = simulith_time_init();
        if (local->simulith_time_handle == NULL)
        {
            OS_DEBUG("Failed to initialize simulith time provider\n");
            pthread_cancel(local->handler_thread);
            return OS_TIMER_ERR_UNAVAILABLE;
        }

        timebase->external_sync = OS_TimeBase_SimulithWaitImpl;
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
     * We just set the accuracy based on the fixed 10ms interval.
     */
    if (local->simulith_time_handle != NULL)
    {
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
    ** Clean up simulith time provider
    */
    if (local->simulith_time_handle != NULL)
    {
        simulith_time_cleanup(local->simulith_time_handle);
        local->simulith_time_handle = NULL;
    }

    return OS_SUCCESS;
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
