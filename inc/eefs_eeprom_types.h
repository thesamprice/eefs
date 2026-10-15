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
 * Filename: eefs_eeprom_types.h
 *
 * Purpose: On-media and RAM types of the EEPROM backend (File Allocation Table, File Header, Inode Table and File
 *   Descriptor).  Included by eefs_fileapi.h when EEFS_BACKEND_NAND is not defined; do not include it directly.
 *   See the design notes in eefs_fileapi.h.
 *
 */

#ifndef _eefs_eeprom_types_
#define _eefs_eeprom_types_

typedef struct
{
    uint32                              Crc;
    uint32                              Magic;
    uint32                              Version;
    uint32                              FreeMemoryOffset;
    uint32                              FreeMemorySize;
    uint32                              NumberOfFiles;
} EEFS_FileAllocationTableHeader_t;

typedef struct
{
    uint32                              FileHeaderOffset;   /* relative offset of the file header from the beginning of the file system */
    uint32                              MaxFileSize;        /* max size of the slot in bytes (note this number does NOT include the file header) */
} EEFS_FileAllocationTableEntry_t;

typedef struct
{
    EEFS_FileAllocationTableHeader_t    Header;
    EEFS_FileAllocationTableEntry_t     File[EEFS_MAX_FILES];
} EEFS_FileAllocationTable_t;

typedef struct
{
    uint32                              Crc;
    uint32                              InUse;  /* if InUse is FALSE then the file has been deleted */
    uint32                              Attributes;
    uint32                              FileSize;
    uint32                              ModificationDate;   /* time_t seconds, stored as 32 bits so the layout is host independent */
    uint32                              CreationDate;
    char                                Filename[EEFS_MAX_FILENAME_SIZE];
} EEFS_FileHeader_t;

typedef struct
{
    void                               *FileHeaderPointer;
    uint32                              MaxFileSize;
} EEFS_InodeTableEntry_t;

typedef struct
{
    cpuaddr                             BaseAddress;
    void                               *FreeMemoryPointer;
    uint32                              FreeMemorySize;
    uint32                              NumberOfFiles;
    EEFS_InodeTableEntry_t              File[EEFS_MAX_FILES];
} EEFS_InodeTable_t;

typedef struct
{
    uint32                              InUse;
    uint32                              Mode;
    void                               *FileHeaderPointer;
    void                               *FileDataPointer;
    uint32                              ByteOffset;
    uint32                              FileSize;
    uint32                              MaxFileSize;
    EEFS_InodeTable_t                  *InodeTable;
    uint32                              InodeIndex;
} EEFS_FileDescriptor_t;

#endif

/************************/
/*  End of File Comment */
/************************/
