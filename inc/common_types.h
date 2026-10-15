/*---------------------------------------------------------------------------
**
**  Filename:
**    $Id: common_types.h 1.2 2011/05/17 14:54:35GMT-05:00 sslegel Exp  $
**
**      Copyright (c) 2004-2014, United States government as represented by the 
**      administrator of the National Aeronautics Space Administration.  
**      All rights reserved. This software was created at NASAs Goddard 
**      Space Flight Center pursuant to government contracts.
**
**      This is governed by the NASA Open Source Agreement and may be used, 
**      distributed and modified only pursuant to the terms of that agreement. 
**
**  Purpose:
**	    Unit specification for common types.
**
**  Design Notes:
**         Assumes make file has defined processor family
**
**  References:
**     Flight Software Branch C Coding Standard Version 1.0a
**
**
**	Notes:
**
**
**  $Date: 2011/05/17 14:54:35GMT-05:00 $
**  $Revision: 1.2 $
**  $Log: common_types.h  $
**  Revision 1.2 2011/05/17 14:54:35GMT-05:00 sslegel 
**  Merged in code walkthrough changes from GPM
**  Revision 1.2 2011/04/26 14:06:02EDT sslegel 
**  EEFS Code Review Changes
**-------------------------------------------------------------------------*/

#ifndef _common_types_
#define _common_types_

/*
** Includes
*/

/*
** Macro Definitions
*/

/*
** Define compiler specific macros
** The __extension__ compiler pragma is required
** for the uint64 type using GCC with the ANSI C90 standard.
** Other macros can go in here as needed, for example alignment 
** pragmas.
*/
#if defined (__GNUC__)
   #define _EXTENSION_    __extension__
   #define OS_PACK        __attribute__ ((packed))
   #define OS_ALIGN(n)  __attribute__((aligned(n)))
#else
   #define _EXTENSION_ 
   #define OS_PACK
   #define OS_ALIGN(n) 
#endif

#if defined(_ix86_)
/* ----------------------- Intel x86 processor family -------------------------*/
  /* Little endian */
  #undef   _STRUCT_HIGH_BIT_FIRST_
  #define  _STRUCT_LOW_BIT_FIRST_

#ifndef _USING_RTEMS_INCLUDES_
  typedef unsigned char                         boolean;
#endif

#elif defined(__PPC__)
   /* ----------------------- Motorola Power PC family ---------------------------*/
   /* The PPC can be programmed to be big or little endian, we assume native */
   /* Big endian */
   #define _STRUCT_HIGH_BIT_FIRST_
   #undef  _STRUCT_LOW_BIT_FIRST_

   typedef unsigned char                        boolean;

#elif defined(_m68k_)
   /* ----------------------- Motorola m68k/Coldfire family ---------------------------*/
   /* Big endian */
   #define _STRUCT_HIGH_BIT_FIRST_
   #undef  _STRUCT_LOW_BIT_FIRST_

   typedef unsigned char                        boolean;

#elif defined(__SPARC__)
   /* ----------------------- SPARC/LEON family ---------------------------*/
   /* SPARC Big endian */
   #define _STRUCT_HIGH_BIT_FIRST_
   #undef  _STRUCT_LOW_BIT_FIRST_

   typedef unsigned char                        boolean;

#else  /* not any of the above: ARM, AArch64, RISC-V, MicroBlaze, ... */
   /* Take the endianness from the compiler when it says (GCC and Clang do) */
   #if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
      #define _STRUCT_HIGH_BIT_FIRST_
      #undef  _STRUCT_LOW_BIT_FIRST_
   #else
      #undef  _STRUCT_HIGH_BIT_FIRST_
      #define _STRUCT_LOW_BIT_FIRST_
   #endif

#ifndef _USING_RTEMS_INCLUDES_
   typedef unsigned char                        boolean;
#endif
#endif  /* processor types */

/*
** Fixed-width integer types.  These are used for on-media structures, so they
** must be exactly the named width on every host and target (an LP64 host would
** otherwise make "long" 8 bytes and corrupt the image layout).
*/
#include <stdint.h>

typedef int8_t                                  int8;
typedef int16_t                                 int16;
typedef int32_t                                 int32;
typedef uint8_t                                 uint8;
typedef uint16_t                                uint16;
typedef uint32_t                                uint32;
typedef uint64_t                                uint64;

/* Integer type wide enough to hold a pointer (memory addresses) */
typedef uintptr_t                               cpuaddr;

#ifndef NULL              /* pointer to nothing */
   #define NULL ((void *) 0)
#endif

#ifndef TRUE              /* Boolean true */
   #define TRUE (1)
#endif

#ifndef FALSE              /* Boolean false */
   #define FALSE (0)
#endif

#endif  /* _common_types_ */
