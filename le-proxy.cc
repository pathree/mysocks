/*
  This example code shows how to write an (optionally encrypting) SSL proxy
  with Libevent's bufferevent layer.

  XXX It's a little ugly and should probably be cleaned up.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <unordered_map>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

struct STREAM {
  int stream_id;

  struct bufferevent *bev_in;
  struct bufferevent *bev_out;
};

std::unordered_map<int, STREAM *> streams;

static struct event_base *base;

#define MAX_OUTPUT (512 * 1024)

#define xlog(format, args...)                                              \
  do {                                                                     \
    struct timeval tv;                                                     \
    gettimeofday(&tv, NULL);                                               \
    printf("%ld.%ld [%s] " format, tv.tv_sec, tv.tv_usec / 1000, __func__, \
           ##args);                                                        \
  } while (0)

static void drained_writecb(struct bufferevent *bev, void *arg);
static void readcb(struct bufferevent *bev, void *arg);
static void eventcb(struct bufferevent *bev, short events, void *arg);

static STREAM *stream_new(struct bufferevent *bev_in) {
  /*stream已存在*/
  int stream_id = bufferevent_getfd(bev_in);
  const auto &iter = streams.find(stream_id);
  if (iter != streams.end()) {
    return (iter->second);
  }

  /*从客户端消息中获取服务器地址端口*/
  struct evbuffer *input;
  input = bufferevent_get_input(bev_in);

  char *beginning = (char *)evbuffer_pullup(input, -1);
  if (!beginning) return NULL;

  char *cp = strchr(beginning, ' ');
  if (!cp) {
    xlog("no header:%s\n", cp);
    return NULL;
  }

  int len = cp - beginning;
  char header[128] = {0};
  memcpy(header, beginning, len);

  xlog("forward to: %s\n", header);

  /*分析地址端口合法性*/
  struct sockaddr_storage connect_to_addr;
  memset(&connect_to_addr, 0, sizeof(connect_to_addr));
  int connect_to_addrlen = sizeof(connect_to_addr);
  if (evutil_parse_sockaddr_port(header, (struct sockaddr *)&connect_to_addr,
                                 &connect_to_addrlen) < 0) {
    xlog("header error:%s\n", header);
    return NULL;
  }

  /*连接服务器地址端口*/
  struct bufferevent *bev_out = bufferevent_socket_new(
      base, -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
  if (bufferevent_socket_connect(bev_out, (struct sockaddr *)&connect_to_addr,
                                 connect_to_addrlen) < 0) {
    perror("bufferevent_socket_connect");
    bufferevent_free(bev_out);
    return NULL;
  }

  /*放入streams*/
  STREAM *stream = new STREAM();
  stream->stream_id = stream_id;
  stream->bev_in = bev_in;
  stream->bev_out = bev_out;

  streams[stream_id] = stream;

  xlog("new stream:%d\n", stream_id);

  /*移除header数据*/
  evbuffer_drain(input, len);

  /*设置读回调函数*/
  bufferevent_setcb(bev_in, readcb, NULL, eventcb, stream);
  bufferevent_enable(bev_in, EV_READ | EV_WRITE);

  bufferevent_setcb(bev_out, readcb, NULL, eventcb, stream);
  bufferevent_enable(bev_out, EV_READ | EV_WRITE);
  return stream;
}

static void stream_free(STREAM *stream) {
  if (!stream) return;
  streams.erase(stream->stream_id);

  if (stream->bev_in) {
    bufferevent_free(stream->bev_in);
    stream->bev_in = NULL;
  }

  if (stream->bev_out) {
    bufferevent_free(stream->bev_out);
    stream->bev_out = NULL;
  }

  xlog("delete stream:%d\n", stream->stream_id);

  delete stream;
}

static void readcb(struct bufferevent *bev, void *arg) {
  xlog("=== curr_fd:%d, rw:%d\n", bufferevent_getfd(bev),
       bufferevent_get_enabled(bev));

  STREAM *stream = NULL;

  /*如果arg为空, 建立数据流stream*/
  if (arg == NULL) {
    stream = stream_new(bev);
    if (!stream) return;
  } else {
    stream = (STREAM *)arg;
  }

  struct bufferevent *partner = NULL;
  if (bev == stream->bev_in) {
    /*数据方向是bev_in --> bev_out*/
    partner = stream->bev_out;
  } else {
    /* 数据方向是bev_out --> bev_in*/
    partner = stream->bev_in;
  }

  /*把src的数据转发到dst*/
  struct evbuffer *src, *dst;
  src = bufferevent_get_input(bev);
  dst = bufferevent_get_output(partner);
  long src_len = evbuffer_get_length(src);

  evbuffer_add_buffer(dst, src);
  // char buf[1024] = {0};
  // while (evbuffer_get_length(src)) {
  //   int n = evbuffer_remove(src, buf, sizeof(buf) - 1);
  //   evbuffer_add(dst, buf, n);
  // }

  xlog("%d -> %d: %ld\n", bufferevent_getfd(bev), bufferevent_getfd(partner),
       src_len);

  xlog("dst len:%ld\n", evbuffer_get_length(dst));
  if (evbuffer_get_length(dst) >= MAX_OUTPUT) {
    /* We're giving the other side data faster than it can
     * pass it on.  Stop reading here until we have drained the
     * other side to MAX_OUTPUT/2 bytes. */
    bufferevent_setcb(partner, readcb, drained_writecb, eventcb, bev);
    bufferevent_setwatermark(partner, EV_WRITE, MAX_OUTPUT / 2, MAX_OUTPUT);
    bufferevent_disable(bev, EV_READ);
  }
}

static void drained_writecb(struct bufferevent *bev, void *arg) {
  xlog("=== curr_fd:%d, rw:%d\n", bufferevent_getfd(bev),
       bufferevent_get_enabled(bev));

  struct bufferevent *partner = (struct bufferevent *)arg;

  /* We were choking the other side until we drained our outbuf a bit.
   * Now it seems drained. */
  bufferevent_setcb(bev, readcb, NULL, eventcb, partner);
  bufferevent_setwatermark(bev, EV_WRITE, 0, 0);
  if (partner) bufferevent_enable(partner, EV_READ);
}

static void close_on_finished_writecb(struct bufferevent *bev, void *arg) {
  xlog("=== curr_fd:%d, rw:%d\n", bufferevent_getfd(bev),
       bufferevent_get_enabled(bev));

  struct evbuffer *b = bufferevent_get_output(bev);

  if (evbuffer_get_length(b) == 0) {
    xlog("Closing %d\n", bufferevent_getfd(bev));
    bufferevent_free(bev);
  }
}

static void eventcb(struct bufferevent *bev, short events, void *arg) {
  xlog("=== curr_fd:%d, rw:%d, events:0x%x\n", bufferevent_getfd(bev),
       bufferevent_get_enabled(bev), events);

  if (events & BEV_EVENT_CONNECTED) {
    xlog("Connect okay\n");
  }

  STREAM *stream = NULL;
  struct bufferevent *partner = NULL;
  if (arg) {
    stream = (STREAM *)arg;
    if (bev == stream->bev_in)
      partner = stream->bev_out;
    else
      partner = stream->bev_in;
  }

  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
    if (events & BEV_EVENT_ERROR) {
      xlog("error:%s\n", evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    }

    if (partner) {
      /* Flush all pending data */
      readcb(bev, arg);

      if (evbuffer_get_length(bufferevent_get_output(partner))) {
        /* We still have to flush data from the other
         * side, but when that's done, close the other
         * side. */
        bufferevent_setcb(partner, NULL, close_on_finished_writecb, eventcb,
                          NULL);
        bufferevent_disable(partner, EV_READ);
        xlog("bufferevent_disable read: %d\n", bufferevent_getfd(partner));
      } else {
        /* We have nothing left to say to the other
         * side; close it. */
        xlog("Closing %d\n", bufferevent_getfd(partner));
      }
    }

    xlog("Closing %d\n", bufferevent_getfd(bev));
    stream_free(stream);
  }
}

static void syntax(void) {
  fputs("Syntax:\n", stderr);
  fputs("   le-proxy <listen-on-addr>\n", stderr);
  fputs("Example:\n", stderr);
  fputs("   le-proxy 127.0.0.1:8088\n", stderr);

  exit(1);
}

static void accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *a, int slen, void *p) {
  struct bufferevent *bev_in;

  bev_in = bufferevent_socket_new(
      base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);

  bufferevent_setcb(bev_in, readcb, NULL, eventcb, NULL);
  bufferevent_enable(bev_in, EV_READ | EV_WRITE);
}

int main(int argc, char **argv) {
  int socklen;

  struct evconnlistener *listener;

  struct sockaddr_storage listen_on_addr;
  memset(&listen_on_addr, 0, sizeof(listen_on_addr));
  socklen = sizeof(listen_on_addr);
  if (evutil_parse_sockaddr_port(
          "127.0.0.1:80", (struct sockaddr *)&listen_on_addr, &socklen) < 0)
    syntax();

  base = event_base_new();
  if (!base) {
    perror("event_base_new()");
    return 1;
  }

  listener = evconnlistener_new_bind(
      base, accept_cb, NULL,
      LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE, -1,
      (struct sockaddr *)&listen_on_addr, socklen);

  xlog("listener on 127.0.0.1:80\n");
  event_base_dispatch(base);

  evconnlistener_free(listener);
  event_base_free(base);

  return 0;
}
