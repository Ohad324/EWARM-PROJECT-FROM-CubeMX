/*----------------------------------------------------------------------------/
/  FatFs - Generic FAT Filesystem Module  R0.15                               /
/  ffconf.h — Project configuration for STM32H747I-DISCO Voice Recorder       /
/----------------------------------------------------------------------------*/

#define FFCONF_DEF  80286   /* Revision ID — must match FF_DEFINED in ff.h */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY   0   /* 0:Read/Write  1:Read-only */
#define FF_FS_MINIMIZE   0   /* 0:All APIs enabled */
#define FF_USE_FIND      0
#define FF_USE_MKFS      1   /* Enable f_mkfs() */
#define FF_USE_FASTSEEK  1
#define FF_USE_EXPAND    1   /* Enable f_expand() — contiguous cluster pre-allocation (no-beep strategy) */
#define FF_USE_CHMOD     0
#define FF_USE_LABEL     0
#define FF_USE_FORWARD   0
#define FF_USE_STRFUNC   0
#define FF_PRINT_LLI     0
#define FF_PRINT_FLOAT   0
#define FF_STRF_ENCODE   3

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE  437   /* US ASCII — sufficient for REC_NNN.wav filenames */

#define FF_USE_LFN    1     /* Enable LFN — required for exFAT */
#define FF_MAX_LFN    255
#define FF_LFN_UNICODE  0   /* 0:ANSI/OEM */
#define FF_LFN_BUF    255
#define FF_SFN_BUF    12
#define FF_FS_RPATH   0

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES       1
#define FF_STR_VOLUME_ID 0
#define FF_VOLUME_STRS   "SD"
#define FF_MULTI_PARTITION  1
#define FF_MIN_SS    512
#define FF_MAX_SS    512
#define FF_LBA64     0
#define FF_MIN_GPT   0x10000000
#define FF_USE_TRIM  0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY      0
#define FF_FS_EXFAT     1   /* Enable exFAT — for SanDisk cards */
#define FF_FS_NORTC     1   /* No RTC — use fixed timestamp */
#define FF_NORTC_MON    1
#define FF_NORTC_MDAY   1
#define FF_NORTC_YEAR   2024
#define FF_FS_NOFSINFO  0
#define FF_FS_LOCK      2
#define FF_FS_REENTRANT 0
