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
#include <event2/dns.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "le-proxy.h"
#include "stream.h"
#include "utils.h"
#include "xlog.h"

/**
  从bufferevent input buffer第一个报文中提取header
*/
static int parse_header(struct bufferevent *bev, HEADER *header) {
  struct evbuffer *input;
  input = bufferevent_get_input(bev);

  char *beginning = (char *)evbuffer_pullup(input, -1);
  if (!beginning) return (-1);

  const char *cp = strchr(beginning, ' ');
  if (!cp) {
    xlog("none header\n");
    return (-1);
  }

  char header_str[1024] = {0};
  int header_len = cp - beginning + 1;
  strncpy(header_str, beginning, header_len);

  cp = strchr(header_str, ':');
  if (!cp) {
    xlog("error header:%s\n", header_str);
    return (-1);
  }

  xlog("header: %s\n", header_str);

  strncpy(header->addr, header_str, cp - header_str);
  cp++;
  header->port = atoi(cp);

  /*从input buffer中删除header数据*/
  evbuffer_drain(input, header_len);
  return 0;
}

STREAM *stream_new(struct bufferevent *bev_in) {
  /*如果stream已存在*/
  int stream_id = bufferevent_getfd(bev_in);
  const auto &iter = proxy.streams.find(stream_id);
  if (iter != proxy.streams.end()) {
    return (iter->second);
  }

  /*提取headerd*/
  HEADER header;
  if (parse_header(bev_in, &header)) {
    return NULL;
  }

  /*连接服务器地址端口*/
  struct bufferevent *bev_out = bufferevent_socket_new(
      proxy.base, -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
  if (bufferevent_socket_connect_hostname(bev_out, proxy.dns_base, AF_INET,
                                          header.addr,
                                          header.port)) {  // only ipv4
    xlog("bufferevent_socket_connect_hostname(%s:%d) failed", header.addr,
         header.port);
    bufferevent_free(bev_out);
    return NULL;
  }

  /*放入streams, 使用bev_in的fd做stream_id*/
  STREAM *stream = new STREAM();
  stream->stream_id = stream_id;
  stream->bev_in = bev_in;
  stream->bev_out = bev_out;

  proxy.streams[stream_id] = stream;

  xlog("New stream: %d --> %d\n", bufferevent_getfd(bev_in),
       bufferevent_getfd(bev_out));

  /*设置读回调函数*/
  bufferevent_setcb(bev_in, stream_readcb, NULL, stream_eventcb, stream);
  bufferevent_enable(bev_in, EV_READ | EV_WRITE);

  bufferevent_setcb(bev_out, stream_readcb, NULL, stream_eventcb, stream);
  bufferevent_enable(bev_out, EV_READ | EV_WRITE);
  return stream;
}

void stream_free(STREAM *stream) {
  if (!stream) return;
  proxy.streams.erase(stream->stream_id);

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

void stream_readcb(struct bufferevent *bev, void *arg) {
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
    /*解释一下：
     *数据转发后，立即检查一下对端output buffer的大小，
     *如果超过某个阈值MAX_OUTPUT，把本端的readcb禁了不读了。
     *然后给对端加一个drained_writecb写回调来监控对端output buffer大小，
     *如果对端output buffer低于MAX_OUTPUT的一半，触发drained_writecb
     *高水位MAX_OUTPUT在本代码中不生效的，只有filtering场景才行*/
    bufferevent_setcb(partner, stream_readcb, stream_drained_writecb,
                      stream_eventcb, bev);
    bufferevent_setwatermark(partner, EV_WRITE, MAX_OUTPUT / 2, MAX_OUTPUT);
    bufferevent_disable(bev, EV_READ);
  }
}

void stream_drained_writecb(struct bufferevent *bev, void *arg) {
  xlog("fd:%d\n", bufferevent_getfd(bev));

  struct bufferevent *partner = (struct bufferevent *)arg;

  /* We were choking the other side until we drained our outbuf a bit.
   * Now it seems drained. */
  /*解释一下：
   *这个回调发生，说明output buffer的大小低于MAX_OUTPUT一半了
   *这时就可以删除drained_writecb回调，清除写操作的高低水位。
   *此外，还需要恢复原来被禁止读的那边的bev的readcb回调*/
  bufferevent_setcb(bev, stream_readcb, NULL, stream_eventcb, partner);
  bufferevent_setwatermark(bev, EV_WRITE, 0, 0);
  if (partner) bufferevent_enable(partner, EV_READ);
}

static void stream_close_on_finished_writecb(struct bufferevent *bev,
                                             void *arg) {
  struct evbuffer *output = bufferevent_get_output(bev);

  /*当output buffer的数据发送完成
   *服务器主动关闭连接*/
  if (evbuffer_get_length(output) == 0) {
    xlog("Closing %d\n", bufferevent_getfd(bev));
    bufferevent_free(bev);
  }
}

void stream_eventcb(struct bufferevent *bev, short events, void *arg) {
  xlog("events: 0x%02x(%s)\n", events, WHATSTR(events));

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
      int err = bufferevent_socket_get_dns_error(bev);
      if (err)
        xlog("error:%s\n", evutil_gai_strerror(err));
      else
        xlog("error:%s\n",
             evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    }

    /*这里需要判断一下对端还在不在，因为对端可能先触发事件而close释放了*/
    if (partner) {
      /* Flush all pending data */
      /*有事件时，先把本端的input buffer都读出来，把残留数据都发给对端*/
      stream_readcb(bev, arg);

      if (evbuffer_get_length(bufferevent_get_output(partner))) {
        /* We still have to flush data from the other
         * side, but when that's done, close the other
         * side. */
        /*如果对端的output buffer中还有数据残留，
         *要确保对端数据写出去以后再关闭bev并释放，
         *回调函数close_on_finished_writecb就是做这个事情的
         *此外也要关闭对端的read操作，避免又读数据进来*/
        bufferevent_setcb(partner, NULL, stream_close_on_finished_writecb,
                          stream_eventcb, NULL);
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
