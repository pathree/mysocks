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

#include "xlog.h"

struct STREAM {
  int stream_id;

  struct bufferevent *bev_in;
  struct bufferevent *bev_out;
};

std::unordered_map<int, STREAM *> streams;

static struct event_base *base;

#define MAX_OUTPUT (512 * 1024)

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

  int header_len = cp - beginning;
  char header[128] = {0};
  memcpy(header, beginning, header_len);

  xlog("Parse header: %s\n", header);

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

  /*放入streams, 使用bev_in的fd做stream_id*/
  STREAM *stream = new STREAM();
  stream->stream_id = stream_id;
  stream->bev_in = bev_in;
  stream->bev_out = bev_out;

  streams[stream_id] = stream;

  xlog("New stream: %d --> %d\n", bufferevent_getfd(bev_in),
       bufferevent_getfd(bev_out));

  /*移除header数据*/
  evbuffer_drain(input, header_len + 1);

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

  xlog("Deleted stream:%d\n", stream->stream_id);

  delete stream;
}

static void readcb(struct bufferevent *bev, void *arg) {
  struct evbuffer *input, *output;

  input = bufferevent_get_input(bev);
  xlog("Read from fd:%d, %ld bytes\n", bufferevent_getfd(bev),
       evbuffer_get_length(input));

  STREAM *stream = (STREAM *)arg;

  /*如果stream为空, 建立新的数据流*/
  if (stream == NULL) {
    stream = stream_new(bev);
    if (!stream) {
      xlog("Closing %d\n", bufferevent_getfd(bev));
      bufferevent_free(bev);
      return;
    }
  }

  struct bufferevent *partner = NULL;
  if (bev == stream->bev_in) {
    /*数据方向是bev_in --> bev_out*/
    partner = stream->bev_out;
  } else {
    /* 数据方向是bev_out --> bev_in*/
    partner = stream->bev_in;
  }

  /*数据转发*/
  output = bufferevent_get_output(partner);
  // evbuffer_add_buffer(output, input);
  char buf[1024] = {0};
  while (evbuffer_get_length(input)) {
    int n = evbuffer_remove(input, buf, sizeof(buf) - 1);
    evbuffer_add(output, buf, n);

    xlog("Forwarding %d --> %d,  %d bytes\n", bufferevent_getfd(bev),
         bufferevent_getfd(partner), n);
  }

  if (evbuffer_get_length(output) >= MAX_OUTPUT) {
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
  STREAM *stream = (STREAM *)arg;

  if (events & BEV_EVENT_CONNECTED) {
    xlog("Connected okay on fd:%d\n", bufferevent_getfd(bev));
  }

  struct bufferevent *partner = NULL;
  if (stream) {
    partner = (bev == stream->bev_in) ? stream->bev_out : stream->bev_in;
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
        xlog("Closing fd:%d\n", bufferevent_getfd(partner));
      }
    }

    xlog("Closing fd:%d\n", bufferevent_getfd(bev));
    stream_free(stream);
  }
}

static void accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *a, int slen, void *p) {
  char client_ip[256] = {0};
  int client_port = 0;
  sock_ntop(a, client_ip, sizeof(client_ip), &client_port);
  xlog("New conn from client(%s:%d), fd:%d\n", client_ip, client_port, fd);

  getsockaddr_from_fd(fd, client_ip, sizeof(client_ip), &client_port);
  xlog("New conn to server(%s:%d), fd:%d\n", client_ip, client_port, fd);

  struct bufferevent *bev_in;

  bev_in = bufferevent_socket_new(
      base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);

  bufferevent_setcb(bev_in, readcb, NULL, eventcb, NULL);
  bufferevent_enable(bev_in, EV_READ | EV_WRITE);
}

static void syntax(void) {
  fputs("Syntax:\n", stderr);
  fputs("   le-proxy [listen-on-addr]\n", stderr);
  fputs("Example(default):\n", stderr);
  fputs("   le-proxy 0.0.0.0:80\n", stderr);

  exit(1);
}

int main(int argc, char **argv) {
  char addr_port[128] = {0};
  if (argc == 1)
    sprintf(addr_port, "0.0.0.0:80");
  else if (argc == 2)
    sprintf(addr_port, "%s", argv[1]);
  else
    syntax();

  int socklen;

  struct evconnlistener *listener;

  struct sockaddr_storage listen_on_addr;
  memset(&listen_on_addr, 0, sizeof(listen_on_addr));
  socklen = sizeof(listen_on_addr);
  if (evutil_parse_sockaddr_port(addr_port, (struct sockaddr *)&listen_on_addr,
                                 &socklen) < 0)
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

  xlog("Listening on %s\n", addr_port);
  event_base_dispatch(base);

  evconnlistener_free(listener);
  event_base_free(base);

  return 0;
}
