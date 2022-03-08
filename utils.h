#ifndef MYSOCKS_UTILS_H_
#define MYSOCKS_UTILS_H_

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/sysinfo.h>
#include <string>

char *sock_ntop(const struct sockaddr *sa, char str[], int strsize, int *port);
int getsockaddr_from_fd(int fd, char ip[], int ipsize, int *port);

static inline std::string whatstr(int what) {
  std::string s;
  if (what & BEV_EVENT_READING) s += "|READ";
  if (what & BEV_EVENT_WRITING) s += "|WRITE";
  if (what & BEV_EVENT_EOF) s += "|EOF";
  if (what & BEV_EVENT_ERROR) s += "|ERROR";
  if (what & BEV_EVENT_TIMEOUT) s += "|TIMEOUT";
  if (what & BEV_EVENT_CONNECTED) s += "|CONNECTED";
  if (!s.empty()) s.erase(0, 1);
  return s;
}
#define WHATSTR(w) whatstr(w).c_str()

static inline long get_uptime() {
  struct sysinfo info = {0};
  sysinfo(&info);
  return info.uptime;
}
#define UPTIME get_uptime()

#endif  // MYSOCKS_UTILS_H_