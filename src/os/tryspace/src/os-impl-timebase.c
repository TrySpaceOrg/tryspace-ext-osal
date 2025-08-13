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

#include "cfe_psp_timebase.h"
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
// Use simulith_client_initialized from PSP

/*
 * Reference count for active timebases using simulith
 * When this reaches zero, we can shutdown the simulith client
 */
static int simulith_timebase_count = 0;

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

/****************************************************************************************
                                INITIALIZATION FUNCTION
 ***************************************************************************************/
int32 OS_Posix_TimeBaseAPI_Impl_Init(void)
{
    int32 return_code = OS_SUCCESS;

    /* Initialize the timebase table */
    memset(OS_impl_timebase_table, 0, sizeof(OS_impl_timebase_table));

    /* Set the clock accuracy to the Simulith interval */
    POSIX_GlobalVars.ClockAccuracyNsec = INTERVAL_NS;

    /* Initialize Simulith client early in OSAL timebase initialization */
    OS_DEBUG("OS_Posix_TimeBaseAPI_Impl_Init: Initializing Simulith client\n");
    CFE_PSP_InitSimulithTime();

    if (INTERVAL_NS == 0)
    {
        OS_DEBUG("Error: INTERVAL_NS cannot be zero\n");
        return OS_ERROR;
    }

    OS_SharedGlobalVars.TicksPerSecond = (uint32)(1000000000UL / INTERVAL_NS);
    OS_SharedGlobalVars.MicroSecPerTick = (uint32)(INTERVAL_NS / 1000UL);

    if (OS_SharedGlobalVars.TicksPerSecond == 0 || OS_SharedGlobalVars.MicroSecPerTick == 0)
    {
        OS_DEBUG("Error: Invalid tick time globals\n");
        return OS_ERROR;
    }

    /* Debug logs for initialization values */
    OS_DEBUG("INTERVAL_NS: %lu\n", (unsigned long)INTERVAL_NS);
    OS_DEBUG("TicksPerSecond: %u\n", OS_SharedGlobalVars.TicksPerSecond);
    OS_DEBUG("MicroSecPerTick: %u\n", OS_SharedGlobalVars.MicroSecPerTick);
    OS_DEBUG("OS_Posix_TimeBaseAPI_Impl_Init: Initialization successful\n");

    return return_code;
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
    OS_timebase_internal_record_t *     timebase;

    timebase = OS_OBJECT_TABLE_GET(OS_timebase_table, *token);

    /* Delegate tick synchronization to PSP */
    if (timebase->external_sync == NULL)
    {
        /* Simulith client should already be initialized by OS_Posix_TimeBaseAPI_Impl_Init */
        timebase->external_sync = CFE_PSP_WaitForSimulithTick;
        simulith_timebase_count++;
        return_code = OS_SUCCESS;
    }
    else
    {
        return_code = OS_ERROR;
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

    OS_DEBUG("[DBG] OS_TimeBaseDelete_Impl ENTRY: token=%p, local=%p, handler_thread=%lu, simulith_timebase_count=%d\n", (void*)token, (void*)local, (unsigned long)local->handler_thread, simulith_timebase_count);

    pthread_cancel(local->handler_thread);

    /* Decrement reference count */
    simulith_timebase_count--;
    OS_DEBUG("[DBG] OS_TimeBaseDelete_Impl: Simulith timebase deleted, count now: %d\n", simulith_timebase_count);

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
