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

#define MAX_LINE 16384

#define xlog(format, args...)                                              \
  do {                                                                     \
    struct timeval tv;                                                     \
    gettimeofday(&tv, NULL);                                               \
    printf("%ld.%ld [%s] " format, tv.tv_sec, tv.tv_usec / 1000, __func__, \
           ##args);                                                        \
  } while (0)

extern void readcb(struct bufferevent *bev, void *arg);
extern void writecb(struct bufferevent *bev, void *arg);
extern void eventcb(struct bufferevent *bev, short events, void *arg);

void readcb(struct bufferevent *bev, void *arg) {
  xlog("fd:%d, rw:%d\n", bufferevent_getfd(bev), bufferevent_get_enabled(bev));

  struct evbuffer *input, *output;

  input = bufferevent_get_input(bev);
  output = bufferevent_get_output(bev);

  char buf[1024] = {0};
  while (evbuffer_get_length(input)) {
    int n = evbuffer_remove(input, buf, sizeof(buf) - 1);
    /*休眠模拟耗时任务*/
    sleep(n);
    evbuffer_add(output, buf, n);

    xlog("send(%d), %s\n", n, buf);

    /*服务器收全数据后，不再读input buffer的数据
     *同时，开启写回调监控数据是否发完*/
    if (buf[n - 1] == '\n') {
      bufferevent_setcb(bev, NULL, writecb, eventcb, NULL);
      break;
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
  xlog("fd:%d, rw:%d\n", bufferevent_getfd(bev), bufferevent_get_enabled(bev));

  struct evbuffer *output = bufferevent_get_output(bev);

  /*当output buffer的数据发送完成
   *服务器主动关闭连接*/
  if (evbuffer_get_length(output) == 0) {
    bufferevent_free(bev);
    xlog("Closing\n");
  }
}

void eventcb(struct bufferevent *bev, short events, void *ptr) {
  xlog("fd:%d, rw:%d, events:0x%x\n", bufferevent_getfd(bev),
       bufferevent_get_enabled(bev), events);

  struct evbuffer *input = bufferevent_get_input(bev);

  if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    if (events & BEV_EVENT_ERROR) {
      xlog("error:%s\n", evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    }

    if (events & BEV_EVENT_EOF) {
      xlog("left:%ld\n", evbuffer_get_length(input));
    }

    xlog("Closing\n");
    bufferevent_free(bev);
  }
}

static void accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *a, int slen, void *arg) {
  xlog("fd:%d\n", fd);

  struct event_base *base = (struct event_base *)arg;

  struct bufferevent *bev;
  bev = bufferevent_socket_new(base, fd,
                               BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
  if (!bev) {
    perror("bufferevent_socket_new");
    return;
  }

  /*设置非阻塞模式O_NONBLOCK，当read/write错误且errno等于EAGAIN时重试*/
  evutil_make_socket_nonblocking(bufferevent_getfd(bev));

  /*指定回调函数*/
  bufferevent_setcb(bev, readcb, NULL, eventcb, NULL);

  /*读数据时，就是从内核协议栈读入数据到input buffer中
   *低水位：表示buffer中只有读够这个值才触发读回调，否则继续等待；
   *高水位：表示buffer中的数据到这个值就不再读入了，直到低于这个值才继续读*/
  // bufferevent_setwatermark(bev, EV_READ, 2, 4);

  /*新创建的bufferevent，默认disable读回调，enable写回调
   *如果要读数据，需要明确地enable读回调*/
  bufferevent_enable(bev, EV_READ);
}

static void syntax(void) {
  fputs("Syntax:\n", stderr);
  fputs("   server <listen-on-addr>\n", stderr);
  fputs("Example:\n", stderr);
  fputs("   server 127.0.0.1:8081\n", stderr);

  exit(1);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    syntax();
  }

  struct sockaddr_storage listen_on_addr;
  memset(&listen_on_addr, 0, sizeof(listen_on_addr));
  int listen_on_addrlen = sizeof(listen_on_addr);
  if (evutil_parse_sockaddr_port(argv[1], (struct sockaddr *)&listen_on_addr,
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

  xlog("listener on %s\n", argv[1]);

  event_base_dispatch(base);

  evconnlistener_free(listener);
  event_base_free(base);
  return 0;
}