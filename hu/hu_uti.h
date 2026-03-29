#pragma once

//#define NDEBUG // Ensure debug stuff

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>

#include <string.h>
#include <signal.h>

#include <pthread.h>

#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <dirent.h>                                                   // For opendir (), readdir (), closedir (), DIR, struct dirent.

#include <libudev.h>
// Enables for hex_dump:
extern int ena_hd_hu_aad_dmp;
extern int ena_hd_tra_send;
extern int ena_hd_tra_recv;

extern int ena_log_aap_send;

extern int ena_log_extra;
extern int ena_log_verbo;
extern int ena_log_debug;
extern int ena_log_warni;
extern int ena_log_error;

typedef unsigned char byte;


#ifdef __ANDROID_API__
  #include <android/log.h>
#else
    // UNKNOWN    0
  #define ANDROID_LOG_DEFAULT 1
  #define ANDROID_LOG_VERBOSE 2
  #define ANDROID_LOG_DEBUG   3
    // INFO       4
  #define ANDROID_LOG_WARN    5
  #define ANDROID_LOG_ERROR   6
    // FATAL      7
    // SILENT     8
#endif

#define hu_LOG_EXT   ANDROID_LOG_DEFAULT
#define hu_LOG_VER   ANDROID_LOG_VERBOSE
#define hu_LOG_DEB   ANDROID_LOG_DEBUG
#define hu_LOG_WAR   ANDROID_LOG_WARN
#define hu_LOG_ERR   ANDROID_LOG_ERROR

#ifdef NDEBUG

#define  logx(...)
#define  logv(...)
#define  logd(...)
#define  logw(...)
#define  loge(...)

#else

#define STR(s) STR2(s)
#define STR2(s) #s

#define  logx(...)  do { if (ena_log_extra) hu_log(hu_LOG_EXT,__FILE__ ":" STR(__LINE__),__FUNCTION__,__VA_ARGS__); } while(0)
#define  logv(...)  do { if (ena_log_verbo) hu_log(hu_LOG_VER,__FILE__ ":" STR(__LINE__),__FUNCTION__,__VA_ARGS__); } while(0)
#define  logd(...)  do { if (ena_log_debug) hu_log(hu_LOG_DEB,__FILE__ ":" STR(__LINE__),__FUNCTION__,__VA_ARGS__); } while(0)
#define  logw(...)  do { if (ena_log_warni) hu_log(hu_LOG_WAR,__FILE__ ":" STR(__LINE__),__FUNCTION__,__VA_ARGS__); } while(0)
#define  loge(...)  do { if (ena_log_error) hu_log(hu_LOG_ERR,__FILE__ ":" STR(__LINE__),__FUNCTION__,__VA_ARGS__); } while(0)

//!!
//  #define  logx(...)
//  #define  logv(...)
//  #define  logd(...)
//  #define  logw(...)

#endif

int hu_log (int prio, const char * tag, const char * func, const char * fmt, ...);


unsigned long ms_sleep        (unsigned long ms);
void hex_dump                 (const char * prefix, int width, unsigned char * buf, int len);

void hu_log_library_versions();

void hu_install_crash_handler();

// Alignment-safe big-endian read/write helpers.
// Use memcpy so the compiler emits safe load/store instructions on ARM
// even when the pointer is not naturally aligned.
#include <string.h>
#include <endian.h>
inline uint64_t read_be64(const void* p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return be64toh(v);
}
inline void write_be64(void* p, uint64_t v) {
    v = htobe64(v);
    memcpy(p, &v, sizeof(v));
}

int wait_for_device_connection();
#ifndef __ANDROID_API__
  #define strlcpy   strncpy
  #define strlcat   strncat
#endif

#if CMU

inline int pthread_setname_np (pthread_t __target_thread, const char *__name)
{
  return 0;
}

#endif
