
#include <unistd.h>

#include "utils.h"
#include "xlog.h"

int use_ssl = 0;
static SSL_CTX *ssl_ctx = NULL;
char req[1024] = {0};

static void readcb(struct bufferevent *bev, void *arg);
static void writecb(struct bufferevent *bev, void *arg);
static void eventcb(struct bufferevent *bev, short events, void *arg);

static void send_req(struct bufferevent *bev) {
  struct evbuffer *output = bufferevent_get_output(bev);
  char buf[1024] = {0};
  int n = 0;

  n = sprintf(buf, req);
  n += sprintf(buf + n, "\n");
  evbuffer_add(output, buf, n);
  xlog("Writting to fd:%d, %d bytes\n", bufferevent_getfd(bev), n);
}

static void readcb(struct bufferevent *bev, void *arg) {
  struct evbuffer *input = bufferevent_get_input(bev);
  xlog("Read from fd:%d, %ld bytes\n", bufferevent_getfd(bev),
       evbuffer_get_length(input));

  char buf[1024];
  while (evbuffer_get_length(input)) {
    int n = evbuffer_remove(input, buf, sizeof(buf));
    fwrite(buf, 1, n, stdout);
    // fwrite("\n", 1, 1, stdout);
    fflush(stdout);
  }
}

static void writecb(struct bufferevent *bev, void *arg) {
  xlog("Written to fd:%d, rw:%d\n", bufferevent_getfd(bev),
       bufferevent_get_enabled(bev));
}

static void eventcb(struct bufferevent *bev, short events, void *arg) {
  xlog("events: 0x%02x(%s)\n", events, WHATSTR(events));

  /*等待连接完成后再发送数据*/
  if (events & BEV_EVENT_CONNECTED) {
    xlog("Connected okay on fd:%d\n", bufferevent_getfd(bev));
    send_req(bev);
  } else if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    if (events & BEV_EVENT_ERROR) {
      xlog("error:%s\n", evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    }

    xlog("Closing fd:%d\n", bufferevent_getfd(bev));
    bufferevent_free(bev);
  }
}

static void syntax(void) {
  fputs("Syntax:\n", stderr);
  fputs("   client <forward-to-addr> <contents>\n", stderr);
  fputs("Example:\n", stderr);
  fputs("   client [127.0.0.1:8088] HelloWorld\n", stderr);

  exit(1);
}

int main(int argc, char **argv) {
  opterr = 0;
  int c;
  while ((c = getopt(argc, argv, "s")) != -1) {
    switch (c) {
      case 's':
        use_ssl = 1;
        break;
      case '?':
        syntax();
    }
  }

  argc -= optind;
  argv += optind;

  if (!*argv) syntax();

  char forward_to_addr_port[128] = "127.0.0.1:8088";
  sprintf(req, "%s %s", forward_to_addr_port, *argv);

  if (use_ssl)
    xlog("Connect to %s using SSL\n", "127.0.0.1:80");
  else
    xlog("Connect to %s\n", "127.0.0.1:80");

  struct sockaddr_storage connect_to_addr;
  memset(&connect_to_addr, 0, sizeof(connect_to_addr));
  int connect_to_addrlen = sizeof(connect_to_addr);
  if (evutil_parse_sockaddr_port("127.0.0.1:80",
                                 (struct sockaddr *)&connect_to_addr,
                                 &connect_to_addrlen) < 0) {
    perror("evutil_parse_sockaddr_port");
    return 1;
  }

  struct event_base *base;
  base = event_base_new();
  if (!base) {
    perror("event_base_new");
    return 1;
  }

  /*设置BEV_OPT_DEFER_CALLBACKS，把回调推迟到eventloop中
   *否则没有回调事件时eventloop就退出了*/
  struct bufferevent *bev = NULL;
  if (use_ssl) {
    ssl_ctx = ssl_ctx_new(0, NULL, NULL);
    if (!ssl_ctx) {
      event_base_free(base);
      return 1;
    }

    SSL *ssl;
    ssl = SSL_new(ssl_ctx);
    bev = bufferevent_openssl_socket_new(
        base, -1, ssl, BUFFEREVENT_SSL_CONNECTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    bufferevent_openssl_set_allow_dirty_shutdown(bev, 1);
  } else {
    bev = bufferevent_socket_new(
        base, -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
  }

  if (!bev) {
    perror("bufferevent_socket_new/bufferevent_openssl_socket_new");
    event_base_free(base);
    return 1;
  }

  if (bufferevent_socket_connect(bev, (struct sockaddr *)&connect_to_addr,
                                 connect_to_addrlen) < 0) {
    perror("bufferevent_socket_connect");
    bufferevent_free(bev);
    event_base_free(base);
    return 1;
  }

  /*设置非阻塞模式O_NONBLOCK，当read/write错误且errno等于EAGAIN时重试*/
  evutil_make_socket_nonblocking(bufferevent_getfd(bev));

  /*指定回调函数
   *readcb: 数据到了后，触发读调用
   *writecb：数据发出去了后，触发写调用
   *eventcb：发生事件，比如读到末尾了、建立连接、拒绝连接、连接断开等*/
  bufferevent_setcb(bev, readcb, writecb, eventcb, NULL);

  /*读数据时，就是从内核协议栈读入数据到input buffer中
   *低水位：表示buffer中只有读够这个值才触发读回调，否则继续等待；
   *高水位：表示buffer中的数据到这个值就不再读入了，直到低于这个值才继续读*/
  // bufferevent_setwatermark(bev, EV_READ, 5, 10);

  /*新创建的bufferevent，默认disable读回调，enable写回调
   *如果要读数据，需要明确地enable读回调*/
  bufferevent_enable(bev, EV_READ);

  event_base_dispatch(base);

  /*释放资源*/
  // evdns_base_free(proxy.dns_base, 0);
  event_base_free(base);

  return 0;
}