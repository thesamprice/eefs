/*
**      Copyright (c) 2010-2014, United States government as represented by the
**      administrator of the National Aeronautics Space Administration.
**      All rights reserved. This software was created at NASAs Goddard
**      Space Flight Center pursuant to government contracts.
**
**      This is governed by the NASA Open Source Agreement and may be used,
**      distributed and modified only pursuant to the terms of that agreement.
*/

/*
 * Filename: eefs_rtems_lock.c
 *
 * Purpose: The EEFS-provided library lock for RTEMS 6/7 (CMake EEFS_LOCK=rtems): EEFS_LibLockHook and
 *   EEFS_LibUnlockHook on one statically initialized RTEMS recursive mutex with priority inheritance.
 *
 * Design Notes:
 *   See eefs_rtems_lock.h.  The mutex is recursive because the lock is library-wide and must tolerate a nested
 *   EEFS_LIB_LOCK; the library itself pairs every lock with one unlock.
 *
 * References:
 *   docs/nand-implementation.md section 13.
 */

/*
 * Includes
 */

#include <rtems/thread.h>
#include "common_types.h"
#include "eefs_rtems_lock.h"

/*
 * Local Data
 */

static rtems_recursive_mutex    EEFS_RtemsLock = RTEMS_RECURSIVE_MUTEX_INITIALIZER("EEFS");
static uint32                   EEFS_RtemsLockWaitCount = 0;

/*
 * Function Definitions
 */

/* EEFS_LIB_LOCK */
void EEFS_LibLockHook(void)
{
    if (rtems_recursive_mutex_try_lock(&EEFS_RtemsLock) != 0) {
        rtems_recursive_mutex_lock(&EEFS_RtemsLock);
        EEFS_RtemsLockWaitCount++;          /* under the lock */
    }
}

/* EEFS_LIB_UNLOCK */
void EEFS_LibUnlockHook(void)
{
    rtems_recursive_mutex_unlock(&EEFS_RtemsLock);
}

uint32 EEFS_RtemsLockWaits(void)
{
    return(EEFS_RtemsLockWaitCount);
}

/************************/
/*  End of File Comment */
/************************/
