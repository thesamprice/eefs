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
 * Filename: eefs_rtems_lock.h
 *
 * Purpose: The EEFS-provided library lock for RTEMS 6/7, built into the eefs library by the CMake option
 *   EEFS_LOCK=rtems (which also defines EEFS_LIB_LOCK_HOOKS and EEFS_LIB_LOCK_RTEMS for every consumer).
 *
 * Design Notes:
 *   EEFS_LIB_LOCK / EEFS_LIB_UNLOCK of the standalone eefs_macros.h call EEFS_LibLockHook() / EEFS_LibUnlockHook(),
 *   which eefs_rtems_lock.c implements with one library-wide RTEMS self-contained recursive mutex
 *   (rtems_recursive_mutex, <rtems/thread.h>).  That mutex uses the priority inheritance protocol: a high-priority
 *   task that waits for the library (a writer) lends its priority to the low-priority task that holds it (a
 *   background health check step), so the step finishes promptly.
 *
 *   The mutex is statically initialized (RTEMS_RECURSIVE_MUTEX_INITIALIZER).  There is nothing to create: no init
 *   function to call before the first EEFS call, no creation failure, no race between tasks that make their first
 *   EEFS call at the same time, and no Classic API semaphore to add to CONFIGURE_MAXIMUM_SEMAPHORES.  It may be used
 *   from any task, not from interrupt context (the library must not be either).
 *
 * References:
 *   docs/nand-implementation.md section 13.
 */

#ifndef _eefs_rtems_lock_
#define _eefs_rtems_lock_

/*
 * Includes
 */

#include "common_types.h"

/*
 * Exported Functions
 */

/* The lock hooks (also declared by eefs_macros.h when EEFS_LIB_LOCK_HOOKS is defined) */
void    EEFS_LibLockHook(void);
void    EEFS_LibUnlockHook(void);

/* Number of times a task had to wait for the library lock because another task held it (a diagnostic: it shows how
 * often a background health check delayed a writer, for example).  Counted while the lock is held; reading it
 * needs no lock. */
uint32  EEFS_RtemsLockWaits(void);

#endif

/************************/
/*  End of File Comment */
/************************/
