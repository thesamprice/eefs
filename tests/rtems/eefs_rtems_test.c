/*
 * RTEMS smoke test for the EEFS library.
 *
 * Mounts a RAM copy of the little-endian golden image (embedded at build time as
 * eefs_test_image[]) through the standalone driver, then:
 *   1. reads both files back and checks their contents,
 *   2. looks a file up with MicroEEFS_FindFile, as a bootloader would,
 *   3. creates, writes and reads back a new file.
 * With EEFS_LOCK=hooks the test supplies the lock hooks and checks that every lock is released; with EEFS_LOCK=rtems
 * it runs on the EEFS-provided RTEMS lock (single task: no waits).
 * Prints "EEFS RTEMS TEST PASSED" or "EEFS RTEMS TEST FAILED" and exits.
 */

#include <rtems.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "common_types.h"
#include "eefs_fileapi.h"
#include "eefs_filesys.h"
#include "microeefs.h"
#ifdef EEFS_LIB_LOCK_RTEMS
#include "eefs_rtems_lock.h"
#endif

extern const unsigned char eefs_test_image[];
extern const unsigned int  eefs_test_image_size;

#define EEPROM_SIZE 65536

static uint8 Eeprom[EEPROM_SIZE] __attribute__((aligned(8)));
static int   Failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            Failures++;                                                 \
        }                                                               \
    } while (0)

#if defined(EEFS_LIB_LOCK_HOOKS) && !defined(EEFS_LIB_LOCK_RTEMS)
/* EEFS_LOCK=hooks: the application provides the lock; this one counts and checks nesting (one task) */
static uint32 LockCalls;
static int32  LockDepth;

void EEFS_LibLockHook(void)
{
    LockCalls++;
    LockDepth++;
}

void EEFS_LibUnlockHook(void)
{
    LockDepth--;
}
#endif

static const char HelloText[] = "Hello from the EEPROM File System\n";

static void check_hello(void)
{
    char  Buffer[64];
    int32 Fd;
    int32 Bytes;

    Fd = EEFS_Open("/EEFS0/hello.txt", O_RDONLY);
    CHECK(Fd >= 0);
    if (Fd < 0) return;

    memset(Buffer, 0, sizeof(Buffer));
    Bytes = EEFS_Read(Fd, Buffer, sizeof(Buffer));
    CHECK(Bytes == (int32)strlen(HelloText));
    CHECK(memcmp(Buffer, HelloText, strlen(HelloText)) == 0);
    EEFS_Close(Fd);
}

static void check_pattern(void)
{
    static uint8 Buffer[3000];
    EEFS_Stat_t  Stat;
    int32        Fd;
    int32        Bytes;
    uint32       i;
    int          Mismatches = 0;

    CHECK(EEFS_Stat("/EEFS0/pattern.bin", &Stat) == EEFS_SUCCESS);
    CHECK(Stat.FileSize == sizeof(Buffer));
    CHECK(Stat.Attributes == EEFS_ATTRIBUTE_READONLY);
    CHECK(Stat.CreationDate == 1700000000);

    Fd = EEFS_Open("/EEFS0/pattern.bin", O_RDONLY);
    CHECK(Fd >= 0);
    if (Fd < 0) return;

    Bytes = EEFS_Read(Fd, Buffer, sizeof(Buffer));
    CHECK(Bytes == (int32)sizeof(Buffer));
    for (i = 0; i < sizeof(Buffer); i++) {
        if (Buffer[i] != (uint8)((i * 7 + 3) & 0xff)) Mismatches++;
    }
    CHECK(Mismatches == 0);
    EEFS_Close(Fd);
}

static void check_microeefs(void)
{
    EEFS_FileHeader_t *Header;

    Header = MicroEEFS_FindFile((cpuaddr)Eeprom, "hello.txt");
    CHECK(Header != NULL);
    if (Header != NULL) {
        CHECK(Header->FileSize == strlen(HelloText));
        CHECK(memcmp((uint8 *)Header + sizeof(EEFS_FileHeader_t), HelloText, strlen(HelloText)) == 0);
    }
    CHECK(MicroEEFS_FindFile((cpuaddr)Eeprom, "missing.txt") == NULL);
}

static void check_create(void)
{
    static const char NewText[] = "written on RTEMS";
    char  Buffer[32];
    int32 Fd;

    Fd = EEFS_Creat("/EEFS0/new.txt", 0);
    CHECK(Fd >= 0);
    if (Fd < 0) return;
    CHECK(EEFS_Write(Fd, (void *)NewText, sizeof(NewText)) == (int32)sizeof(NewText));
    EEFS_Close(Fd);

    Fd = EEFS_Open("/EEFS0/new.txt", O_RDONLY);
    CHECK(Fd >= 0);
    if (Fd < 0) return;
    memset(Buffer, 0, sizeof(Buffer));
    CHECK(EEFS_Read(Fd, Buffer, sizeof(Buffer)) == (int32)sizeof(NewText));
    CHECK(strcmp(Buffer, NewText) == 0);
    EEFS_Close(Fd);

    /* The original files must be untouched */
    check_hello();
}

static rtems_task Init(rtems_task_argument ignored)
{
    (void)ignored;

    printf("\nEEFS %d.%d.%d RTEMS test, image %u bytes\n",
           EEFS_MAJOR_VERSION, EEFS_MINOR_VERSION, EEFS_REVISION, eefs_test_image_size);

    memcpy(Eeprom, eefs_test_image, eefs_test_image_size);

    CHECK(EEFS_InitFS("/EEDEV0", (cpuaddr)Eeprom) == EEFS_SUCCESS);
    CHECK(EEFS_Mount("/EEDEV0", "/EEFS0") == EEFS_SUCCESS);

    check_hello();
    check_pattern();
    check_microeefs();
    check_create();

#if defined(EEFS_LIB_LOCK_HOOKS) && !defined(EEFS_LIB_LOCK_RTEMS)
    printf("library lock hooks: %lu calls\n", (unsigned long)LockCalls);
    CHECK(LockCalls > 0);
    CHECK(LockDepth == 0);
#endif
#ifdef EEFS_LIB_LOCK_RTEMS
    printf("EEFS RTEMS lock: %lu waits\n", (unsigned long)EEFS_RtemsLockWaits());
    CHECK(EEFS_RtemsLockWaits() == 0);
#endif

    printf(Failures == 0 ? "EEFS RTEMS TEST PASSED\n" : "EEFS RTEMS TEST FAILED (%d)\n", Failures);
    exit(Failures == 0 ? 0 : 1);
}

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER
#define CONFIGURE_MAXIMUM_TASKS            1
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ATTRIBUTES     RTEMS_FLOATING_POINT
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
