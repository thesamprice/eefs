/*
**      Copyright (c) 2010-2014, United States government as represented by the 
**      administrator of the National Aeronautics Space Administration.  
**      All rights reserved. This software was created at NASAs Goddard 
**      Space Flight Center pursuant to government contracts.
**
**      This is governed by the NASA Open Source Agreement and may be used, 
**      distributed and modified only pursuant to the terms of that agreement.
**
** Filename: eefs_config.h
**
** Purpose: This file contains system dependent macros for the eefs.
**
** Design Notes:
**
** References:
**
*/

#ifndef _eefs_config_
#define _eefs_config_

/*
 * Macro Definitions
 */

/* Maximum number of files in the file system */
#define EEFS_MAX_FILES                      64

/* Maximum number of file descriptors */
#define EEFS_MAX_OPEN_FILES                 20

/* Default number of spare bytes added to the end of a slot when a 
   new file is created by calling the EEFS_LibCreat function */
#define EEFS_DEFAULT_CREAT_SPARE_BYTES      512

/*
 * NAND backend (EEFS_BACKEND_NAND).  These are macros only, so they do not affect the EEPROM backend.  Each one may be
 * overridden with -D.  See docs/nand-implementation.md section 2.2 and docs/nand-format.md section 1.2.
 */

/* Largest page and OOB the build supports; the volume's page buffers are sized from these */
#ifndef EEFS_NAND_MAX_PAGE_SIZE
#define EEFS_NAND_MAX_PAGE_SIZE             4096
#endif
#ifndef EEFS_NAND_MAX_OOB_SIZE
#define EEFS_NAND_MAX_OOB_SIZE              224
#endif

/* Largest device (blocks = bytes of block map) and directory region the build supports */
#ifndef EEFS_NAND_MAX_BLOCKS
#define EEFS_NAND_MAX_BLOCKS                4096
#endif
#ifndef EEFS_NAND_MAX_DIR_BLOCKS
#define EEFS_NAND_MAX_DIR_BLOCKS            32
#endif

/* Format-affecting sizes: a build that changes any of them must also change EEFS_NAND_LAYOUT_VERSION */
#ifndef EEFS_NAND_MAX_PATH_SIZE
#define EEFS_NAND_MAX_PATH_SIZE             256     /* stored path, including the NUL */
#endif
#ifndef EEFS_NAND_CFS_NAME_SIZE
#define EEFS_NAND_CFS_NAME_SIZE             20      /* cFS app/lib name (OS_MAX_API_NAME) */
#endif
#ifndef EEFS_NAND_CFS_SYMBOL_SIZE
#define EEFS_NAND_CFS_SYMBOL_SIZE           64      /* cFS entry symbol */
#endif
#ifndef EEFS_NAND_LAYOUT_VERSION
#define EEFS_NAND_LAYOUT_VERSION            1
#endif

/* Pending cleanup list; an operation that would overflow it returns EEFS_TRANSACTION_ERROR */
#ifndef EEFS_NAND_MAX_CLEANUP_BLOCKS
#define EEFS_NAND_MAX_CLEANUP_BLOCKS        256
#endif

/* Extra blocks allocated to new and relocated files */
#ifndef EEFS_NAND_CREAT_SPARE_BLOCKS
#define EEFS_NAND_CREAT_SPARE_BLOCKS        0
#endif

/* Read back every programmed page */
#ifndef EEFS_NAND_VERIFY_WRITES
#define EEFS_NAND_VERIFY_WRITES             1
#endif

/* Open checks every block CRC, not just the metadata page */
#ifndef EEFS_NAND_VERIFY_ON_OPEN
#define EEFS_NAND_VERIFY_ON_OPEN            1
#endif

/* Commit after every mutating call */
#ifndef EEFS_NAND_AUTO_COMMIT
#define EEFS_NAND_AUTO_COMMIT               0
#endif

/* Restarts of a file or directory write after a grown bad block */
#ifndef EEFS_NAND_WRITE_RETRIES
#define EEFS_NAND_WRITE_RETRIES             3
#endif

/* Added to the metadata page write sequence at mount */
#ifndef EEFS_NAND_WRITESEQ_MOUNT_GAP
#define EEFS_NAND_WRITESEQ_MOUNT_GAP        1024
#endif

/* Health check report sizes */
#ifndef EEFS_NAND_MAX_HEALTH_FINDINGS
#define EEFS_NAND_MAX_HEALTH_FINDINGS       32
#endif
#ifndef EEFS_NAND_HEALTH_PATH_SIZE
#define EEFS_NAND_HEALTH_PATH_SIZE          64      /* truncated path copy in a finding */
#endif

#if defined(EEFS_BACKEND_NAND) && (EEFS_MAX_FILES > 126)
#error "EEFS_MAX_FILES must be <= 126 for the NAND block map encoding"
#endif

#endif 

/************************/
/*  End of File Comment */
/************************/

