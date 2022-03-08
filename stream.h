
#ifndef MYSOCKS_STREAM_H_
#define MYSOCKS_STREAM_H_

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/dns.h>
#include <event2/listener.h>
#include <event2/util.h>

#define MAX_OUTPUT (512 * 1024)
// #define MAX_OUTPUT 2

struct STREAM {
  int stream_id;

  struct bufferevent *bev_in;
  struct bufferevent *bev_out;
};

struct HEADER {
  char addr[128];
  int port;
};

void stream_readcb(struct bufferevent *bev, void *arg);
void stream_eventcb(struct bufferevent *bev, short events, void *arg);
void stream_drained_writecb(struct bufferevent *bev, void *arg);

STREAM *stream_new(struct bufferevent *bev_in);
void stream_free(STREAM *stream);

#endif  // MYSOCKS_STREAM_H_