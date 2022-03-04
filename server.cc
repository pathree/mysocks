#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>

#include "xlog.h"

#define MAX_LINE 16384

using std::string;

extern void readcb(struct bufferevent *bev, void *arg);
extern void writecb(struct bufferevent *bev, void *arg);
extern void eventcb(struct bufferevent *bev, short events, void *arg);

enum bufferevent_filter_result filtercb_input(struct evbuffer *source,
                                              struct evbuffer *destination,
                                              ev_ssize_t dst_limit,
                                              enum bufferevent_flush_mode mode,
                                              void *arg) {
  xlog("dst_limit:%ld, mode:%d\n", dst_limit, mode);

  char buf[1024] = {0};
  int len = sizeof(buf) - 1;
  if (dst_limit > 0) len = dst_limit;
  while (evbuffer_get_length(source)) {
    int n = evbuffer_remove(source, buf, len);

    evbuffer_add(destination, buf, n);

    xlog("Transformed %d bytes\n", n);
  }

  if (evbuffer_get_length(source) > 0)
    return BEV_NEED_MORE;
  else
    return BEV_OK;
}

enum bufferevent_filter_result filtercb_output(struct evbuffer *source,
                                               struct evbuffer *destination,
                                               ev_ssize_t dst_limit,
                                               enum bufferevent_flush_mode mode,
                                               void *arg) {
  xlog("dst_limit:%ld, mode:%d\n", dst_limit, mode);

  char buf[1024] = {0};
  int len = sizeof(buf) - 1;
  if (dst_limit > 0) len = dst_limit;
  while (evbuffer_get_length(source)) {
    int n = evbuffer_remove(source, buf, len);

    evbuffer_add(destination, buf, n);

    xlog("Transformed %d bytes\n", n);
  }

  if (evbuffer_get_length(source) > 0)
    return BEV_NEED_MORE;
  else
    return BEV_OK;
}

void readcb(struct bufferevent *bev, void *arg) {
  struct evbuffer *input, *output;

  input = bufferevent_get_input(bev);
  xlog("Read from fd:%d, %ld bytes\n", bufferevent_getfd(bev),
       evbuffer_get_length(input));

  output = bufferevent_get_output(bev);

  char buf[1024] = {0};
  while (evbuffer_get_length(input)) {
    int n = evbuffer_remove(input, buf, sizeof(buf) - 1);
    /*休眠模拟耗时任务*/
    // sleep(1);
    evbuffer_add(output, buf, n);

    xlog("Writting to fd:%d, %d bytes\n", bufferevent_getfd(bev), n);

    /*服务器收全数据后，不再读input buffer的数据
     *同时，开启写回调监控output buffer数据是否发完*/
    if (buf[n - 1] == '\n') {
      /*如果参数非空，表示回调函数是从filter触发的，写回调set在underlying上
       *否则是从普通bufferevent触发的*/
      if (arg) {
        struct bufferevent *bev_underlying = (struct bufferevent *)arg;
        bufferevent_setcb(bev_underlying, NULL, writecb, NULL, bev);
      } else {
        bufferevent_setcb(bev, NULL, writecb, eventcb, NULL);
        break;
      }
    }
  }

  /*
  char resp[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=UTF-8\r\n"
      "Date: Thu, 20 Jan 2022 04:03:42 GMT\r\n"
      "Content-Length: 2\r\n\r\n"
      "OK\r\n";
  evbuffer_add(output, resp, strlen(resp));*/
}

void writecb(struct bufferevent *bev, void *arg) {
  struct evbuffer *output = bufferevent_get_output(bev);

  xlog("Written to fd:%d, left:%ld bytes\n", bufferevent_getfd(bev),
       evbuffer_get_length(output));

  /*当output buffer的数据发送完成
   *服务器主动关闭连接*/
  if (evbuffer_get_length(output) == 0) {
    /*如果参数非空，表示回调函数是从underlying bufferevent触发的
     *否则是从普通bufferevent触发的*/
    if (arg) {
      struct bufferevent *bev_filtering = (struct bufferevent *)arg;
      bufferevent_free(bev_filtering);
      xlog("Closing fd:%d\n", bufferevent_getfd(bev_filtering));
    } else {
      bufferevent_free(bev);
      xlog("Closing fd:%d\n", bufferevent_getfd(bev));
    }
  }
}

void eventcb(struct bufferevent *bev, short events, void *arg) {
  if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    if (events & BEV_EVENT_ERROR) {
      xlog("error:%s\n", evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    }

    xlog("Closing fd:%d\n", bufferevent_getfd(bev));
    bufferevent_free(bev);
  }
}

static void accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *a, int slen, void *arg) {
  char client_ip[256] = {0};
  int client_port = 0;
  sock_ntop(a, client_ip, sizeof(client_ip), &client_port);

  xlog("New conn from client(%s:%d), fd:%d\n", client_ip, client_port, fd);

  struct event_base *base = (struct event_base *)arg;

  /*创建一个底层underlying bufferevent
   *注意，不要在底层bufferevent上设置回调函数，
   *否则上层的filter bufferevent拿不到数据*/
  struct bufferevent *bev_underlying;
  bev_underlying = bufferevent_socket_new(
      base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
  if (!bev_underlying) {
    perror("bufferevent_socket_new");
    return;
  }

  /*创建一个filtering bufferevent，配置两个回调函数，数据流如下：
   *Kernel-->underlying input buffer-->filter_input_cb(src)
   *--编码-->filter_input_cb(dst)-->filtering output buffer
   *-->filter_output_cb(src)--编码-->filter_output_cb(dst)
   *-->underlying output buffer-->Kernel
   *如果回调函数为NULL，所有数据自动从src写到dst*/
  struct bufferevent *bev;
  bev = bufferevent_filter_new(bev_underlying, filtercb_input, filtercb_output,
                               BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS,
                               NULL, NULL);

  /*设置非阻塞模式O_NONBLOCK，当read/write错误且errno等于EAGAIN时重试*/
  evutil_make_socket_nonblocking(bufferevent_getfd(bev));

  /*指定回调函数
   *对于filtering场景，write回调函数不能确保underlying被成功写出去了；
   *需要在underlying上设置write回调函数进行监控*/
  bufferevent_setcb(bev, readcb, NULL, eventcb, bev_underlying);

  /*读数据时，就是从内核协议栈读入数据到input buffer中
   *低水位：表示buffer中只有读够这个值才触发读回调，否则继续等待；
   *低水位默认0，表示只要读到1bytes就触发回调
   *高水位：表示buffer中的数据到这个值就不再读入了，直到低于这个值再继续读
   *高水位默认无限大，所以不会停止读入数据*/

  /*对于filtering场景，read的高低水位都不会直接生效，需要set到underlying上才有效；
   *但是，在上层bufferevent设置的read的高水位值，
   *会被传递到input_filter_cb回调函数的dst_limit参数中，提供给编码人员使用*/
  bufferevent_setwatermark(bev, EV_READ, 0, 4);

  /*写数据时，就是把output buffer中的数据发送到内核协议栈中。
   *低水位：表示buffer中的数据低于这个值，触发写回调，否则继续等待；
   *低水位默认是0，就是buffer清空才触发；
   *高水位：不能直接使用，只在filtering场景中有效*/

  /*对于filtering场景，write的高低水位都不会直接生效，需要set到underlying上才有效；
   *但是，在underlying bufferevent设置的write高水位值，
   *会被传递到output_filter_cb回调函数的dst_limit参数中，提供给编码人员使用*/
  bufferevent_setwatermark(bev_underlying, EV_WRITE, 0, 6);

  /*新创建的bufferevent，默认disable读回调，enable写回调
   *如果要读数据，需要明确地enable读回调*/
  bufferevent_enable(bev, EV_READ);
}

static void syntax(void) {
  fputs("Syntax:\n", stderr);
  fputs("   server [listen-on-addr]\n", stderr);
  fputs("Example(default):\n", stderr);
  fputs("   server 0.0.0.0:8088\n", stderr);

  exit(1);
}

int main(int argc, char **argv) {
  char addr_port[128] = {0};
  if (argc == 1)
    sprintf(addr_port, "0.0.0.0:8088");
  else if (argc == 2)
    sprintf(addr_port, "%s", argv[1]);
  else
    syntax();

  struct sockaddr_storage listen_on_addr;
  memset(&listen_on_addr, 0, sizeof(listen_on_addr));
  int listen_on_addrlen = sizeof(listen_on_addr);
  if (evutil_parse_sockaddr_port(addr_port, (struct sockaddr *)&listen_on_addr,
                                 &listen_on_addrlen) < 0) {
    perror("evutil_parse_sockaddr_port");
    return 1;
  }

  struct event_base *base;
  base = event_base_new();
  if (!base) {
    perror("event_base_new");
    return 1;
  }

  struct evconnlistener *listener;
  listener = evconnlistener_new_bind(
      base, accept_cb, base,
      LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE, -1,
      (struct sockaddr *)&listen_on_addr, listen_on_addrlen);
  if (!listener) {
    perror("evconnlistener_new_bind");
    event_base_free(base);
    return 0;
  }

  xlog("Listening on %s\n", addr_port);

  event_base_dispatch(base);

  evconnlistener_free(listener);
  event_base_free(base);
  return 0;
}