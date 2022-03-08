#ifndef MYSOCKS_XLOG_H_
#define MYSOCKS_XLOG_H_

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define xlog(format, args...)                                            \
  do {                                                                   \
    struct timeval tv;                                                   \
    gettimeofday(&tv, NULL);                                             \
    struct tm tm;                                                        \
    char buff[32] = {0};                                                 \
    time_t tt = tv.tv_sec;                                               \
    localtime_r(&tt, &tm);                                               \
    strftime(buff, 31, "%m-%d %H:%M:%S", &tm);                           \
    printf("[%s.%ld][%-20s] " format, buff, tv.tv_usec / 1000, __func__, \
           ##args);                                                      \
  } while (0)

#endif  // MYSOCKS_XLOG_H_
